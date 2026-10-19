# CodexBridge 侧问题清单（设备端 CBDP v1 联调实测）

- 日期：2026-10-19
- 设备端：Dotii ESP32-C3（MAC `98:c3:77:f5:52:70`，IP `192.168.0.39`），CBDP v1 panel profile
- Bridge：本机已安装运行的 `CodexBridge.exe` v2.1.2（本文档只做观测记录，未修改 Bridge、未改动其运行实例）
- 说明：以下均为设备端在真机联调中抓到的**对端行为**与协议文档不一致之处，供 Bridge 侧判断与修复。

## 摘要

设备端已完成音频上行闭环（`AUDIO_BEGIN` → `AUDIO_CHUNK` × N → `AUDIO_END`），Bridge 侧确实落盘为 WAV（`recordings/aipassport-98c377f55270-000003.wav`、`..._000004.wav`，16 kHz/mono/16-bit，约 6.4s / 6.8s，波形非空），说明传输与拼接链路可用。但仍有两个 Bridge 侧行为导致失败或留下残余状态。

---

## 一、`thread.read` 恒定返回 `ERROR(INTERNAL)`，`threads.list` 却正常

**现象**

- `projects.list`（`0x000D`）、`threads.list`（`0x0002`）均正常返回 `RESULT` 与 `objects`。
- 对同一台设备，发出 `thread.read`（`0x0003`）后，Bridge 立即回 `ERROR`：

```
RX type=0x0F code=0x0015            # INTERNAL
  tlv 0x002A request_id = <该命令 request_id>
  tlv 0x0062 "INTERNAL"
```

- 相同的 `tx_id`/`thread_ref` 语义在 `AUDIO_BEGIN` 中可用，说明 ref 本身有效。
- 实测请求参数（TLV 解出结果，均为协议定义的编码）：

```
thread_ref   0x0021 u32  = 84        # 来自 threads.list 的 object(0x0021)
history_depth 0x0014 u16 = 2         # 协议 §8.2 定义为 u16
request_id   0x002A u32
idem         0x002B 16B
method_id    0x002C u16 = 0x0003
```

- 帧长 80 字节，`header_len=0x0050`、`payload_len=0`，`cbdp_frame_validate` 通过；`objects` 等可选字段均未越界。
- `history_depth` 早期设备端误发为 `u8`（框架长度 79），Bridge 也回 `INTERNAL`；设备端按 §8.2 改为 `u16` 后（长度 80）仍是 `INTERNAL`，说明**不是 TLV 类型问题**。

**影响**

- `thread.read` 是协议 §15.1/§15.2 中获取线程元数据与最近消息的正式方法。设备端已改为不再依赖它的返回值即可完成选线程（用 `threads.list` 里已有的 `thread_ref` + `topic_id` 直接绑定并 `SUBSCRIBE`），但 Bridge 侧恒定失败会持续占用一次命令往返并产生 `ERROR`。
- 建议 Bridge 侧检查 `thread.read` 的实现路径（尤其是按 `thread_ref` 查 projection 时；本机 `cbdp-projection.sqlite3` 的 `threads` 表为 0 行，可能相关）。

**证据**

- 设备串口日志（`RX type=0x0F code=0x0015`，`request_id` 与 `thread.read` 一致）。
- Bridge 数据目录 `%LOCALAPPDATA%\CodexBridge\cbdp-projection.sqlite3`：`threads` 表 `count=0`，`messages` 表 143 行。

---

## 二、`message.read.update`（`0x00A0`）返回 `ERROR(INTERNAL)`

**现象**

- 设备在收到线程消息、进入完成态后按 §15.8/§17 发送 `message.read.update`：

```
method_id 0x00A0
params: 0x0026 message_id | 0x0021 thread_ref | 0x0054 read_at_ms(i64)
```

- Bridge 返回 `ERROR(INTERNAL)`（`code=0x0015`，`0x0062 "INTERNAL"`），与 `thread.read` 同样的信封形态与 `request_id` 回带。

**影响**

- 已读状态无法上报，Bridge 侧 checkpoint / unread 计数不会推进。
- 设备端当前按 §21.3 把它降级为“仅提示、不断开”，所以不再拖垮会话，但该能力实际不可用。

**证据**

- 设备串口日志：`message.read.update` 的 `request_id` 与随后 `ERROR(INTERNAL)` 的 `0x002A` 一致。

---

## 三、`AUDIO_END` 后 Bridge 回 `ERROR(UPSTREAM_UNAVAILABLE)`

**现象**

- `AUDIO_BEGIN/CHUNK/END` 传输成功，Bridge 也写出了完整 WAV；随后回：

```
RX type=0x0F code=0x000B
  tlv 0x0062 "UPSTREAM_UNAVAILABLE"
```

- 设备端日志：`capture bytes=216640`、`AUDIO_END ok=1`，紧接 `bridge error code=0x000B: UPSTREAM_UNAVAILABLE`。

**影响**

- 音频已落盘但未转写，设备侧只能显示“识别失败”。若 Bridge 的 STT 依赖未配置/未就绪，建议在 `ERROR` 里带上可操作的原因（例如 `error_detail`），或按 §19.5 的约定明确这是 STT 侧不可用而非设备参数错误。

**证据**

- Bridge 落盘 `recordings/aipassport-98c377f55270-000004.wav`（216684 字节，6.77s，16 kHz/mono，peak 928）。
- 设备串口日志中的 `ERROR(UPSTREAM_UNAVAILABLE)` 与 `request_id`。

---

## 四、建议 Bridge 侧确认的两点协议歧义

1. **`SUBSCRIBE`（`0x07`）省略 `cursor`（`0x0033`）时的语义**
   设备端实测：不携带 `cursor` 时 Bridge 会回放该 topic 的完整历史（一次订阅产生上百条 `DELTA`）。协议 §15.4 把 `cursor` 列为可选，未明确“省略”是等价于 `0` 还是“只推新事件”。建议在文档里写明，或在 `RESULT` 里回带实际使用的 `cursor` 让设备端可判断。

2. **`ERROR` 的 `error_message` 目前只有粗粒度枚举名**
   例如 `thread.read` / `message.read.update` 只拿到 `"INTERNAL"`。若能在 `0x0062` 或 `0x0063` 里带更具体的原因（如具体失败的表/字段），设备端与联调都会更容易定位。

---

## 附：设备端在本次联调中修掉的相关缺陷（供对照，不属于 Bridge 问题）

1. `HELLO/WELCOME` 后每 15s 被应用层固定静默计时器强制断链 → 改为传输层心跳（PING + 12s×3 判活）。
2. `cbdp_parse_auth_ok()` 覆盖 session、丢失 WELCOME 协商的 `max_audio_bytes` → 合并传输层快照。
3. `history_depth` 误按 `u8` 编码 → 按 §8.2 改 `u16`。
4. `threads.list` 结果缺少 `catalog_begin/end` 包裹导致选择器卡在加载态 → 补齐。
5. 收到的 `DELTA` 未按订阅过滤 → 只应用已订阅 topic。
6. `socket_send_all` 把 `EAGAIN` 当致命错误 → 改为在期限内重试；并让 PING/PONG 走同一把 socket 锁，避免与音频帧交错。
7. 每条 `DELTA` 各回一个 `ACK` 造成 ACK 风暴 → 合并为按窗口只回最新 cursor。

---

## 五、v2.1.3 复测结论（2026-10-19 晚）

Bridge 已升级到 **v2.1.3** 并重启。设备端按最新协议重新对齐后再测，结果如下。

### 5.1 已修复，确认可用

| 项 | 状态 |
|---|---|
| `thread.read`（`0x0003`） | **已修复**。`RESULT` 正常返回 `thread_ref`/`status`/`objects`，不再回 `ERROR(INTERNAL)` |
| `message.read.update`（`0x00A0`） | 不再回 `ERROR(INTERNAL)` |
| `UPSTREAM_UNAVAILABLE` | 音频上行后不再无条件出现（Bridge 能落盘 WAV） |

### 5.2 新增：`IDEMPOTENCY_CONFLICT`（`0x0008`）会跨连接复现

- 设备端按 §17.4 把 `idem` 改为 `first_16_bytes(SHA-256(device_id || request_id || method_id || params))`。
- 但 `request_id` 在**同一连接内**从 1 开始计数（协议 §5.1 也只要求"同一连接内唯一"）。
- Bridge 侧幂等记录保留 24 小时，**跨连接/跨重启仍然生效**。因此设备重启后重放 `request_id=8 + 同一 method + 同一 params` 时，Bridge 立即回 `ERROR(IDEMPOTENCY_CONFLICT)`，即使是全新的 TCP 会话。
- 设备端已用启动随机数打散 `request_id`/`tx_id` 起点规避。
- **建议 Bridge 侧确认**：幂等键是否应加入 `session_id` 或连接标识。按当前语义，任何设备只要重启就会与自己的历史记录冲突。

### 5.3 订阅后回放历史会打断正在进行的录音

- 设备选中线程后发 `SUBSCRIBE(topic, scope=thread/summary)`（不带 `cursor`，即 §15.4 的可选省略）。
- Bridge 会把这**整个线程的历史事件**一次性回放给设备。
- 复现：选线程 → 立即开始录音。回放期间 Bridge 停止读取设备上传的音频，设备的 `send()` 持续 `EAGAIN`，`AUDIO_END` 最终以 `ECONNRESET`/`ENOTCONN` 失败，录音报废。
- 设备端日志：`send frame type=0x15 failed (auth=0 fd=-1 errno=104/128)`，`capture bytes` 停在断开时刻。
- 设备侧已做的缓解：音频帧改为短超时发送、背压时丢帧但保持 `offset` 连续，避免设备端自己卡死；但**只要 Bridge 在回放期间不读 socket，录音仍会被截断**。
- **建议 Bridge 侧确认**：订阅回放与设备上行是否共用同一个 socket 读线程。若是，回放期间应继续消费设备上行，或在回放前先给设备一个可恢复的 cursor 让设备选择"只收新事件"。

### 5.4 `AUDIO_BEGIN` 在会话刚结束时被拒

- 复现：选线程 → 等待该线程的历史完成事件到达（UI 进入"完成"态）→ 按录音。
- Bridge 回 `ERROR(UNSUPPORTED_FEATURE)`（`0x000E`），随后连接被重置。
- 设备端此时 `thread_ref` 来自 `threads.list`/`thread.read`，非空；`audio` 能力也在 `enabled_caps` 内。
- **建议 Bridge 侧确认**：`AUDIO_BEGIN` 对 `thread_ref` 的校验条件，以及是否要求在录音前先 `thread.claim`（writer lease）。
