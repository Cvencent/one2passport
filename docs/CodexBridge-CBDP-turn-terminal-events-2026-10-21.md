# CodexBridge 侧改动请求：在 CBDP 上投递 turn 终态事件

- 日期：2026-10-21
- 提出方：设备端（AI Passport 固件）
- 接收方：CodexBridge 开发者
- 当前 Bridge 版本：`2.1.21`（本机实跑，未改动）
- 设备固件：本仓库 `main` @ `d2d7df1`

> **设备端这边已经不需要再改了。** 本仓库固件已经能解析并处理
> `0x0203 / 0x0204 / 0x0205`。缺的只是 Bridge 从来没把这三类事件写进
> EventSpine，所以 CBDP 设备收不到"某一条任务完成了"。

---

## 1. 现象

首页收音机播放时，任务完成提示音不响。

设备端原有的完成提醒路径是：

```text
CBDP DELTA event_type = 0x0203 (turn.completed)
  -> coding_bridge.c 解析 BODY / THREAD_ID / TURN_ID
  -> codex_background_apply_notice() 去重
  -> ui_sfx_play(UI_SFX_DONE_SPEECH)
```

这条路径在真机上从来没有被触发过。

已临时在设备端加了兜底：从聚合计数事件 `0x0704` 的
`completed_unread` 增长推断完成。但这是**推断**，有实际代价：

- 分不清是哪条任务完成，也拿不到结果正文/标题/耗时；
- 其它客户端（手机、另一个设备、桌面 UI）读了结果会让计数回落，
  再涨时会被误判成新完成。真机实测出现过 11 秒内误响 7 声，
  设备端只能加 4 秒落地窗口压掉，导致"很快又完成一条"可能被吞；
- 结果正文只能靠后续 `message.updated` 补，时序上不稳定。

要彻底解决，需要 Bridge 投递真正的终态事件。

---

## 2. 根因（已定位到具体代码）

### 2.1 Bridge 侧

在 `D:\CodexBridge` 中：

```text
codexbridge/cbdp/events.py
    _THREAD_EVENTS = frozenset({ ..., 0x0203, 0x0204, 0x0205, ... })
    event_scope(0x0203) -> SCOPE_THREAD        # 已支持
```

但全仓库检索 `_append_event(...)` 只有 10 处调用点，事件类型分别是：

```text
0x0301 / 0x0302   message.appended / updated   （hub.py: _append_message_event）
0x0704            system.codex_state           （hub.py: _publish_codex_state）
0x0601 / 0x0602 / 0x0603   device online/offline/policy
0x0303            message.read
```

**没有任何一处写入 `0x0203/0x0204/0x0205`。**

`codexbridge/cbdp/compat.py` 的 `LEGACY_EVENT_TYPES` 也只有：

```python
LEGACY_EVENT_TYPES = {
    "message": EVENT_MESSAGE_APPENDED,      # 0x0301
    "read_state": EVENT_MESSAGE_READ,       # 0x0303
    "device_event": 0x0C01,
    "command_result": 0x0503,
    "protocol_catalog": 0x0C0E,
    "capabilities_update": EVENT_DEVICE_POLICY_CHANGED,
    "error": EVENT_DEVICE_POLICY_CHANGED,
}
```

`turn_completed` / `turn_started` 这类 legacy 事件由
`codexbridge/companion/service.py` 的 `publish()` 产生，走的是
`Hub.poll_events()` → `{"type": "device_event"}` 的旧 JSON 兼容通道；
CBDP 网关不做对应的 EventSpine 写入，所以 CBDP 设备看不到。

### 2.2 设备端

设备端固件 `firmware/main/codex_bridge_protocol.h` 已经定义：

```c
CBDP_EVENT_TURN_STARTED     = 0x0201,
CBDP_EVENT_TURN_UPDATED     = 0x0202,
CBDP_EVENT_TURN_COMPLETED   = 0x0203,
CBDP_EVENT_TURN_FAILED      = 0x0204,
CBDP_EVENT_TURN_INTERRUPTED = 0x0205,
```

`coding_bridge.c` 也已经实现终态处理分支（读 BODY/SUMMARY、
THREAD_ID/THREAD_REF、TURN_ID/TURN_REF，去重后响铃并更新首页）。
**设备端不需要改。**

---

## 3. 需要的改动（Bridge）

### 3.1 在结果落库时写入终态事件

结果记录在 `codexbridge/codex/collector.py` 中生成，形如：

```python
MessageRecord(
    message_id=f"result:{result_thread}:{int(completed_at)}",
    thread_id=result_thread,
    kind="result",
    status=result_status,          # "completed" | "failed"
    ...
)
```

`Hub.publish()` 里 `record.kind in {"task", "result"}` 时会调
`_broadcast_codex_state()`，但**没有**写终态事件。建议在
`Hub.publish()` 内、`_append_message_event()` 之后增加：

```python
if record.kind == "result" and record.status in {"completed", "failed"}:
    self._append_turn_terminal_event(record)
```

### 3.2 事件号与 topic/scope

| 字段 | 值 |
|---|---|
| event_type | 完成 `0x0203`；失败 `0x0204`；中断 `0x0205` |
| topic_id | 该结果所属线程的 topic：`self._cbdp_topic(record.thread_id)` |
| scope | `SCOPE_THREAD`（`events.py` 已把这三个事件放进 `_THREAD_EVENTS`） |
| qos | `QOS_CRITICAL`（终态事件不能因预算被静默丢弃） |

**注意**：设备只有订阅了该线程 topic 才能收到。设备在选中任务时会订阅
`topic + SCOPE_THREAD`，这与现有 `message.appended` 的机制一致。
不要求改成 summary scope，否则会向所有设备泄漏其它会话的内容。

### 3.3 Payload TLV（沿用现有 tag，不需要新增）

| Tag | 名称 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| `0x0022` | THREAD_ID | utf8 | 是 | 结果所属 thread id |
| `0x0021` | THREAD_REF | u32 | 否 | 有引用表时带上，设备可省一次解析 |
| `0x0024` | TURN_ID | utf8 | 是 | 本轮 turn id，设备用它去重 |
| `0x0023` | TURN_REF | u32 | 否 | 同上 |
| `0x0045` | KIND | u8 | 否 | `1` = assistant_text（与现有 result 映射一致） |
| `0x0044` | STATUS | u16 | 是 | `2`=completed，`3`=failed，`4`=interrupted |
| `0x0040` | TITLE | utf8 | 否 | 任务标题 |
| `0x0041` | SUMMARY | utf8 | 否 | 结果摘要（设备优先显示这个） |
| `0x0042` | BODY | utf8 | 是 | 结果正文，受 `max_text_bytes` 裁剪 |
| `0x0026` | MESSAGE_ID | utf8 | 否 | result 的 message_id，便于设备做已读 |
| `0x004E` | COMPLETED_AT_MS | i64 | 否 | 完成时间戳 |

设备端解析代码已经按 `BODY → SUMMARY` 回退，所以 `BODY` 是主字段。

### 3.4 不要重复投递

设备端 `codex_background_apply_notice()` 会按
`turn:<turn_id>` / `thread:<thread_id>` / `seq:<global_seq>` 去重，
所以 Bridge 只要保证同一 `turn_id` 的终态事件只写一次即可：

- `record.message_id` 已经是 `result:{thread}:{completed_at}`，天然幂等；
- 同一 `turn_id` 的状态从 `working` → `completed` 只应产生一条；
- 不要在每个 collector 轮询周期重复写。

### 3.5 与 `0x0704` 聚合计数的关系

`0x0704` 继续保留，用于首页"运行中/完成/失败"数量，这部分设备端已经
稳定工作，不要删。

新增终态事件后，设备端会**优先用终态事件响铃**，聚合计数只用于徽标数字。
（设备端已做去重，两条路径同时到达不会双响。）

---

## 4. 建议的最小实现

在 `Hub` 中新增一个方法，并在 `publish()` 里调用：

```python
def _append_turn_terminal_event(self, record: MessageRecord) -> None:
    event_type = {
        "completed": 0x0203,
        "failed": 0x0204,
        "interrupted": 0x0205,
    }.get(str(record.status or ""))
    if event_type is None or not record.thread_id:
        return
    data = record.data if isinstance(record.data, dict) else {}
    task = data.get("task") if isinstance(data.get("task"), dict) else {}
    turn_id = str(task.get("turn_id") or "")
    payload = {
        TAG_THREAD_ID: utf8(str(record.thread_id)),
        TAG_STATUS: u16({"completed": 2, "failed": 3, "interrupted": 4}[record.status]),
        TAG_BODY: utf8(str(record.body or "")),
        TAG_SUMMARY: utf8(str(record.title or "")),
        TAG_TITLE: utf8(str(record.title or "")),
    }
    if turn_id:
        payload[TAG_TURN_ID] = utf8(turn_id)
        payload[TAG_TURN_REF] = u32(self._cbdp_ref("turn", turn_id))
    if record.message_id:
        payload[TAG_MESSAGE_ID] = utf8(str(record.message_id))
    self._append_event(
        self._cbdp_topic(record.thread_id),
        event_type,
        QOS_CRITICAL,
        payload,
    )
```

> `turn_id` 现成可用：`collector.py` 的 `STATUS_FIELDS` 第一个字段就是
> `turn_id`，`_task_data()` 会把它带进 `record.data["task"]`。无需改 collector。
> 万一某条快照确实缺 `turn_id`，设备端会退化为按 `thread_id` 去重，仍能响，
> 但同一线程连续完成两次只提醒一次，所以**请尽量带上 `TURN_ID`**。

---

## 5. 验收标准（设备端已可自测）

1. 设备进入 Codex，选中某项目下的任务（或新建任务后说一句话），
   再退回首页，让收音机继续播放。
2. Bridge 日志应出现一次终态事件写入（`event_type=0x0203`）。
3. 设备串口应出现：
   ```text
   ui_sfx: play effect=3 yield_radio=1 rate=44100/48000 vol=..
   ```
   且**不应**出现第二条 `codec 打开`。
4. 4 秒内连续完成两条不同任务，应响两次（聚合计数兜底路径会因落地窗口只响
   一次，这正是新增事件要解决的问题）。
5. 其它客户端读取结果导致 `completed_unread` 回落再上涨时，**不应**再响铃。

## 6. 边界与不做的事

- 不改 summary scope 的语义，终态事件只走 `SCOPE_THREAD`；
- 不删除 `0x0704` 聚合计数；
- 不改现有 `method.id` / 命令表；
- 不要求设备端新增订阅，设备选中任务时已经订阅线程 topic。

---

## 7. 设备端现状（供对照）

- 固件提交：`d2d7df1`
- 已验证：首页收音机播放中收到终端事件/计数时提示音与收音机共存，
  `codec opens` 保持 1 次，无电流声回归；
- 设备端临时兜底：`coding_bridge.c` 中 `0x0704` 计数差分 + 4 秒落地窗口，
  可在 Bridge 补齐终态事件后保留为"兼容旧 Bridge"的降级路径。
