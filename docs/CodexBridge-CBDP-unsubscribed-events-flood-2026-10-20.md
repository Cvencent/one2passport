# CodexBridge 缺陷：未订阅 topic 的事件仍被推给 CBDP 设备，挤爆 lwIP 内存导致电台卡顿（2026-10-20）

- 日期：2026-10-20
- 设备端：AI Passport（ESP32-C3，无 PSRAM，MAC `98:c3:77:f5:52:70`，IP `192.168.0.39`）
- Bridge：本机运行中的 `CodexBridge.exe` **v2.1.19**
- 说明：本文档只记录设备端观测到的**对端行为**与只读代码分析，供 Bridge 侧开发者判断与修复。
  **设备端仓库没有修改任何 Bridge 代码。**

## 摘要

首页只需要"运行中 / 已完成 / 失败"三个数字，设备也确实**只订阅了 summary topic
（`0x50000003`）**。但 Bridge 会把**设备从未订阅的其它线程的整段消息事件**也推下来
（实测一次 46 条、每条 300~670 字节）。这些帧在 ESP32-C3 上会实打实地占用 lwIP
收发缓冲，把正在播放的网络电台挤到分配失败，表现为**声音播几秒、卡十几秒**。

## 现象（真机证据）

订阅后立刻涌入大量"未订阅"事件，设备端逐条打印（`sub=0` 表示该 topic 未订阅）：

```
I coding_bridge: SUBSCRIBE topic=0x50000003 scope=0 cursor=0
I coding_bridge: delta type=0x0301 hdr_topic=0x10000008 sub=0 seq=3  tseq=3  len=374
I coding_bridge: delta type=0x0301 hdr_topic=0x10000000 sub=0 seq=4  tseq=100 len=346
I coding_bridge: delta type=0x0301 hdr_topic=0x10000001 sub=0 seq=5  tseq=77  len=657
I coding_bridge: delta type=0x0301 hdr_topic=0x10000001 sub=0 seq=6  tseq=78  len=404
... (一次共 ~46 条 0x0301/0x0302)
I coding_bridge: delta type=0x0704 hdr_topic=0x50000003 sub=1 ...   <- 这个才是需要的
```

统计（实测一轮 250 秒）：

| 事件 | topic | 设备是否订阅 | 条数 |
|---|---|---|---|
| `0x0704` codex_state（计数） | `0x50000003` | **是** | 36 |
| `0x0301/0x0302` 消息事件 | `0x10000001` / `0x10000006` / `0x10000008` / `0x10000030` 等 | **否** | 46 |

对应电台吞吐（`netdiag` 每个统计窗口）：

| 窗口 | 收包字节 | EAGAIN 次数 | 结果 |
|---|---|---|---|
| 正常 | 74k~103k | 0 | 播放 9936~9960ms / 10000ms |
| 卡顿 | 10k~38k | 9~16 | 播放 264~7296ms，静默 8~22 秒 |

同一批 URL 用电脑直连：`lhttp-hw.qtfm.cn/live/20211676/64k.mp3` 等
**40 秒 9.2 KB/s、0 次停顿** → 源站没问题。

另外做了一个对照：**进入完整收音机页面（设备会主动停掉 CBDP 桥接）后，连续 6 个
窗口全部满帧**（9936ms/10000ms、`eagain=0`、`rebuffers=0`）。关掉桥接就不卡，
开着就卡。

## 根因（Bridge 侧）

`codexbridge/hub.py` 的投递辅助函数把判断写反了：

```python
# hub.py:1388
@staticmethod
def _deliver_cbdp_event(session, event) -> None:
    """Send one committed event unless the subscription path already did."""
    gateway = getattr(session, "gateway", None)
    if gateway is not None and gateway.has_subscription(
        int(event.topic_id), event_scope(int(event.event_type))
    ):
        # deliver_event() already emitted the subscription copy.
        return                                  # ← 已订阅：跳过（正确）
    try:
        session.send_event(event)               # ← 未订阅：反而直接发送（错误）
    except (OSError, RuntimeError):
        return
```

`session.send_event()`（`codexbridge/devices/session.py:152`）只是编码并发送 DELTA，
**不做任何订阅校验**；而带订阅校验的是 `CbdpGateway.deliver_event()`
（`cbdp/gateway.py:657`，其中 `has_subscription()` 为假时会 `return None`）。

也就是说：`_deliver_cbdp_event()` 只在"已订阅"时跳过，在"**未**订阅"时却调用
`send_event()` 把事件直接推出去，完全绕过了订阅过滤器。而 `hub.publish()`
（`hub.py:543` → `_broadcast_messages()` → `hub.py:1351`）对每条消息记录都会走
这条路径，于是设备收到了它从未订阅的、其它线程的整段消息事件。

## 影响

- 首页只需要 3 个计数，却被迫接收全部线程的消息事件。
- ESP32-C3 无 PSRAM：这些突发帧占用 lwIP pbuf / TCP 接收窗口，正在播放的网络电台
  随即 `malloc` 失败 → socket 空读 → 音频停十几秒、每 20 秒触发一次 `network feed idle`
  重连。用户听到的就是"播几秒、卡几秒"。
- 所有只订阅 summary（或只订阅单个线程）的 CBDP 设备都会被无关事件打扰。

## 建议修复（Bridge 侧）

1. **首选**：`_deliver_cbdp_event()` 改为"只在设备**已订阅**该 topic/scope 时才发"：

   ```python
   if gateway is None or not gateway.has_subscription(
       int(event.topic_id), event_scope(int(event.event_type))
   ):
       return          # 未订阅就不发；订阅路径会负责已订阅的投递
   session.send_event(event)
   ```

   注意与 `deliver_event()` 的去重语义对齐：要么由 `deliver_event()` 统一投递，
   要么这里补投，**两者不能都发，也不能让未订阅的漏出去**。

2. **强烈建议**：在 `session.send_event()` 里也加一道订阅校验（或断言），
   避免其它调用点再次绕过过滤器。协议上"只投递已订阅 topic"是设备端的前提
   （见 `PROTOCOL.md` §15.4），设备端目前是**信任**主机不会发未订阅事件的。

3. 顺带：`_broadcast_messages()` 里对 CBDP 设备建议按"该会话已订阅的 topic"
   预过滤一次，避免为每台设备都构造/编码一遍再丢弃。

## 设备端已做的配套（本次提交，不改 Bridge）

- **回退了上一轮我加的"summary 订阅重试"**：`codex_state` 是事件驱动的，计数不变时
  本来就不会推送，"15 秒没收到"并不代表订阅失败；而每次重订阅都会让主机重放整段
  事件积压（实测一次 32 条），反而**放大**了这个洪泛。真机对照：移除后连续 6 个窗口
  满帧，但事件突发时仍会卡（即本问题）。
- 对 CBDP 控制 socket 设 `SO_RCVBUF = 2048`，让主机侧被 TCP 窗口限速、把突发摊平。
  这是**临时缓解，不能根治**（根因仍在 Bridge 侧）。实测 250 秒窗口统计：

  | 配置 | 满帧窗口 | 卡顿窗口 |
  |---|---|---|
  | 未设 `SO_RCVBUF` | 10 / 16 | 6 |
  | `SO_RCVBUF=2048` | 16 / 19 | 3 |

  仍会偶发卡顿，因为桌面端持续产生新消息，Bridge 就持续把**其它线程**的事件
  推下来；只要这个错误投递还在，突发就会周期性出现。
- 设备端在 `wireless_handle_delta()` 里已经会丢弃未订阅 topic 的事件（不会污染 UI），
  但**帧已经在网络上传输并占用过内存**，所以这一层过滤救不了卡顿。

## 验收标准（Bridge 侧）

1. 设备只订阅 summary topic 时，串口**不再出现** `sub=0` 的 `delta` 日志。
2. 首页"运行中/已完成/失败"仍能正常刷新（`0x0704` 事件照常到达）。
3. 开着 CodexBridge 的同时播放网络电台，`netdiag` 的 `eagain` 保持 0、
   `audio stats` 稳定在 ~9936ms/10000ms，不再出现 8~20 秒静音。
4. 选中某个任务后，该线程的事件仍能正常到达（订阅 topic 的投递不受影响）。
