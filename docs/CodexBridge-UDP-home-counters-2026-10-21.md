# CodexBridge 侧改动请求：把首页计数放到 UDP，让设备不必常开 TCP

- 日期：2026-10-21
- 提出方：设备端（AI Passport 固件）
- 接收方：CodexBridge 开发者
- 当前 Bridge 版本：`2.1.21`（本机实跑，未改动）
- 设备固件：本仓库 `main` @ `1af11f5`

> **设备端已经不需要再挖内存了。** 结论是 C3 的 DMA 内存供不起"收音机流 + CBDP 控制连接"两条 TCP 同时存在。要解决，只能让**首页不要开那条 TCP**。

---

## 1. 背景：为什么设备想要这个

AI Passport 首页（plays/438 风格）在后台播网络电台，同时显示 Codex 的
"运行中 / 已完成 / 失败"三个数字。真机实测（COM4）：

- 播放期 **DMA 可寻址内存只剩 196~1524 字节**，最大连续块仅 **544 字节**；
- WiFi 每个接收缓冲约 **1600 字节且必须是连续 DMA 内存** → 申请失败 → 驱动丢帧；
- TCP 按 `CONFIG_LWIP_TCP_RTO_TIME=1500` 退避重传 → 正好对应每 1~2 秒一次 `eagain`；
- 结果：同一 URL 在 PC 上是 **73 kbps**，设备只有 **1~4 kbps**，K 卡到无法收听。

### 为什么不能只把 CBDP 的窗口调小

```
TCP_SND_BUF_DEFAULT=2880 + TCP_WND_DEFAULT=5760  ≈ 8.6KB / 每连接
```

这三个数是**每条连接各自预留**的。首页驻留时同时存在：

1. 收音机流（1 条 TCP）
2. CBDP 控制连接（1 条 TCP）

合计 ≈ **17KB**，C3 的 DMA 内存给不起。

设备侧已尝试并排除：

| 尝试 | 结果 |
|---|---|
| CBDP socket 设 `SO_RCVBUF=2048` | **一直无效**：`CONFIG_LWIP_SO_RCVBUF` 未开启，且 lwIP 的 `recv_bufsize` 只对 raw/UDP 生效，TCP 路径根本不读 |
| 按 socket 调 TCP 窗口 | **lwIP 无此接口**：`TCP_WINDOW` 只存在于 Kconfig 说明文字，代码零实现 |
| 全局 `TCP_WND` 5760→16384 | **崩**：3× `wifi:mem fail`、Guru、8× `Open failed` |
| 静态 RX 缓冲 3→8 | **崩**：9× `wifi:mem fail`、12× `Open failed`，`dma=12` 时连 `socket()` 都失败 |

**结论：设备侧无余量可挖，必须减少同时存在的 TCP 连接数。**

---

## 2. 请求：把聚合计数放进 UDP 通道

设备首页只需要**三个数字**：

| 字段 | 含义 |
|---|---|
| running | 运行中的任务数 |
| completed_unread | 未读的完成结果数 |
| failed_unread | 未读的失败结果数 |

这三个数字**已经在 Bridge 的 `hub.counts()` 里算好了**，也已经在
TCP 的 summary 主题上以事件 `0x0704`（`system.codex_state`）推送。
现在只需要让它**也走 UDP**。

### 2.1 复用现有 UDP 8764 通道（推荐）

Bridge 与设备之间**已经**在 UDP 8764 上有对称的 CBDP 发现往返：

- 设备启动后主动广播 **presence**（`TYPE_DISCOVERY_REPLY = 0x1D`，
  见设备 `wireless_bridge.c: announce_presence()`）；
- Bridge 的 `PresenceListener` 监听该端口并解析出设备身份
  （`codexbridge/devices/discovery.py`）；
- Bridge 也会周期广播 `TYPE_DISCOVERY = 0x1C` 做发现
  （`discover()`，`DISCOVERY_TIMEOUT_SECONDS = 2.5`）。

**最简做法：Bridge 在收到设备 presence 后，回一帧带计数的 UDP 报文。**

- 复用 `0x1D`（reply）即可，无需新类型：在 header 里追加三个计数 TLV；
- 或新增 `0x1E`（`RESERVED_MESSAGE_TYPES` 已预留 `{0x1E, 0x1F}`），
  语义更清晰、不与发现语义混淆。

**建议节奏**：Bridge 每 **1~2 秒**向该设备单播一次（`source` 地址 + 8764 端口），
计数变化时立即补发一次。设备侧首页刷新本来就是 1 秒级，1~2 秒足够。
不要用广播——那会打醒同网段所有设备。

### 2.2 建议的报文定义

```text
message_type : 0x1E（新增）或 0x1D（复用 reply）
topic_id     : SYSTEM_THREAD_SUMMARIES_TOPIC（与 0x0704 一致，仅作标记）
qos          : QOS_STATE
header TLVs  : 无（UDP 无需会话/序列）
payload TLVs :  TLV_RUNNING_COUNT     = 0x00BC  u16
                TLV_COMPLETED_UNREAD  = 0x00BD  u16
                TLV_FAILED_UNREAD     = 0x00BE  u16
```

> 这三个 tag 号**已在设备端定义**（`codex_bridge_protocol.h`：
> `CBDP_TLV_RUNNING_COUNT=0x00BC` / `CBDP_TLV_COMPLETED_UNREAD=0x00BD` /
> `CBDP_TLV_FAILED_UNREAD=0x00BE`），Bridge 侧对应
> `cbdp/constants.py: TAG_RUNNING_COUNT / TAG_COMPLETED_UNREAD / TAG_FAILED_UNREAD`。
> **无需新增任何 tag。**

### 2.3 不要把 CBDP TCP 会话变成"可选"以外的语义变更

- TCP 路径**保持现状不动**：进 Codex 页仍然照常握手、订阅、收发任务。
- 本请求只是**增加**一条轻量 UDP 计数通道，供"首页/菜单"这种不需要任务
  交互的场景使用。
- 如果同一个数字同时从 `0x0704`（TCP）和 UDP 到达，设备会按"取较大值 +
  4 秒落地窗口去重"处理（现有逻辑），不会双计。

---

## 3. 设备侧会怎么配合

设备端（本仓库，等 Bridge 就绪后实施）：

1. 首页/菜单层级**不建立 CBDP TCP 会话**，只保留 UDP presence + 监听
   §2.2 的计数字段；
2. 进入 Codex 页：才 `coding_bridge_start_wireless()` 建 TCP；
3. 退出 Codex 页：断开 TCP，回到"仅 UDP"；
4. 这样首页播放期只剩电台 1 条 TCP，DMA 预算翻倍。

设备端的 UDP 接收路径现在**只接受 `0x1C`(DISCOVERY) 并回帧**
（`wireless_bridge.c` 里 `FD_ISSET(udp_fd, ...)` 分支），需要增加对
新报文的解析——这部分**设备端来做**，不在本次请求范围。

---

## 4. 验收标准

1. 设备停在首页播放收音机，**不建立 TCP 8765 会话**，Bridge 日志显示
   `/api/admin/devices` 中该设备仍在（依靠 UDP presence）。
2. 首页"运行中/已完成/失败"三个数字能随 Codex 状态在 1~2 秒内更新。
3. 真机播放窗口 `pcm` 占空比显著提升（目标 >90%），`eagain` 降到个位数。
4. 设备**未**出现 `wifi:mem fail` / Guru / `Open failed`。
5. 进入 Codex 页后任务列表、新建、回复、结果推送全部照常（TCP 路径未回归）。

---

## 5. 可选：如果 Bridge 不想动 UDP

备选是保持现状，由设备端做缓解（首页优先选 ≤64kbps 的源）。但这只是
规避并发连接的内存冲突，弱网/高码率台仍会卡，**不推荐**作为最终方案。

---

## 6. 参考（设备端证据）

- 完整排查记录：`docs/firmware-verification-2026-10-21-radio-throughput.md`
  （含 DMA 探针原始数字、崩溃复现、lwIP 源码行号）
- 现有 TCP 计数事件：`0x0704 system.codex_state`
- Bridge 计数来源：`codexbridge/hub.py: counts()`
