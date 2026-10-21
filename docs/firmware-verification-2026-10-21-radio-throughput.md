# 首页收音机"一卡一卡 / 没声音"——排查记录与剩余方案

- 日期：2026-10-21
- 设备：AI Passport（ESP32-C3，8MB Flash，**无 PSRAM**），MAC `98:c3:77:f5:52:70`
- 串口：COM4，ESP-IDF 5.5.3
- Bridge：本机已运行的 CodexBridge `2.1.21`（未修改）
- 结论：**问题未修复**。本文记录已确认的根因方向、以及为什么"加深缓冲 / 调大 TCP 窗口"这类改法全部被硬件内存上限挡住。

---

## 1. 现象（区分成两个独立问题）

1. **"说在播放，但没声音"**
   真机日志抓到 `pcm=10008ms`（满负荷在播）同时 `rms=0 peak=5` —— 解出来的是**纯数字静音**。
   设备没有任何静音检测，所以会一直显示"正在播放"。
2. **"一卡一卡"**
   连续窗口 `pcm` 只占现实时长的 **1%~40%**，`gap_max` 常达 6~30 秒，`eagain` 11~26 次/窗口。

## 2. 已排除的原因（都有实测数据）

| 假设 | 实测 | 结论 |
|---|---|---|
| 电台源头卡 | PC 裸 socket 直连同 URL：中国之声 60s 547KB/**72.9kbps**，最长停顿 1.18s；重庆都市广播 25s 237KB、2007 个 MP3 帧同步字 | **源头正常** |
| DNS 解析到不同 CDN 节点 | 路由器 / 223.5.5.5 / 114.114.114.114 三个 DNS 都解析到同一 IP | 排除 |
| WiFi 省电导致 | 播放期日志为 `Set ps type: 0` = `PERFORMANCE (NONE)` | 排除 |
| 设备是我的改动引入的回归 | **回退到不含本轮任何改动的已提交固件（`git stash` 后重烧）重测，表现完全相同**（13% 与 2% 占空比、13 次 rebuffer） | **非回归，是既有问题** |
| 解码跟不上 | 播放期 `avail`（环形缓冲剩余）常有 1~6KB 空闲 | 排除 |

## 3. 根因方向：DMA 内存被榨干，WiFi 收包缓冲申请不出来

在 `radio_player.cc` 加内存探针后，真机实测播放期：

```text
memprobe before_open      free=10512 dma=2784 largest=7680
memprobe after_open       free=8284  dma=556  largest=7680   <-- DMA 只剩 556B
memprobe after_feed_start free=9252  dma=1524 largest=7680
tcp close                 free=3960->12428 dma=1364->8036
idle5s                    free=1416~2188  dma=1084~1856  largest=544
```

- WiFi 每个动态接收缓冲约 **1600 字节，且必须是 DMA 可寻址内存**。
- 播放期 DMA 空闲只有 **196~1524 字节** → 收包缓冲申请失败 → 网卡丢包。
- TCP 按 `CONFIG_LWIP_TCP_RTO_TIME=1500` 退避重传 → 正好对应每 1~2 秒一次 `eagain`。
- 即便有数据时也只收到 **441 B/s**，而源头是 73 kbps —— 差 20~160 倍。

### 开机时的内存账目（真机 `heapdiag`，静态 RX=3 的基线）

| 阶段 | free | dma | 本阶段消耗 |
|---|---|---|---|
| post_framework_pre_wifi | 99476 | 91748 | — |
| post_wifi_init | 67052 | 59324 | WiFi 32424 |
| post_audio_init | 56364 | 48636 | ES8311/I2S 10688 |
| post_radio_prewarm | 20024 | 12296 | **收音机解码池 36340** |
| post_coding_bridge | 10148 | 2420 | **最小化 Codex 桥接 9876** |

播放期再掉约 12KB（HTTP + TCP + WiFi RX），最终只剩 1.4~2.2KB 空闲、最大连续块 **544B**。

## 4. 试过但失败的改法（都已回退，仓库保持干净）

| 改动 | 结果 |
|---|---|
| 抖动环 `kNetworkBlocks` 20 → 120 | 无效。实测可借静态区只有 20~21 个 1KB 块（`StackType_t` 为 1 字节，实际 9KB+10KB），环没变大 |
| `LWIP_TCP_WND_DEFAULT` 5760 → 16384 | **崩溃**：3× `wifi:mem fail`、1× Guru、8× `Open failed` |
| `LWIP_TCP_OOSEQ_MAX_PBUFS` 4→12 + 收件箱 6→16 | 无任何改善 |
| `ESP_WIFI_STATIC_RX_BUFFER_NUM` 3 → 8 | **崩溃**：9× `wifi:mem fail`、12× `Open failed`（开机 `post_coding_bridge free` 掉到 1680、`dma=12`） |

**结论：开机链条已经贴着上限，任何"多加几 KB"的改法都会直接把设备打崩。**
（`DYNAMIC_RX_BUFFER_NUM=0` 在 IDF 里的语义是"无限制按需分配"，不是"关闭"，且 `RX_BA_WIN=4` 要求它 ≥4，所以也不能简单关掉。）

## 5. 真正可行的方向（需要设计决策，未实施）

根因是**同一时刻两条 TCP 连接（收音机流 + CBDP 桥接）各自要 ~8.6KB DMA 内存**，而 C3 的 DMA 可用内存不够同时供给二者 + WiFi 收包缓冲。

候选方案，按推荐顺序：

### A. 给 CBDP 控制连接用更小的接收窗口（推荐）
桥接只传控制消息（几百字节），根本不需要 TCP 默认的 5760 窗口。
- 启用 `CONFIG_LWIP_SO_RCVBUF`，在 `wireless_bridge.c` 建连后对 CBDP socket 设
  `SO_RCVBUF`（例如 1024~2048）；
- 省下的约 4~5KB DMA 用来把 `ESP_WIFI_STATIC_RX_BUFFER_NUM` 提到 5~6；
- 收音机流继续用全局大窗口（它才需要吞吐）。
- 改动集中在设备侧 `wireless_bridge.c`，不涉及 Bridge 仓库。

### B. 压缩收音机解码池
`kOutputSize = kPcmCapacity(4608) + 2×1024 = 6656`。MP3 满帧确实要 4608，但共享网络块那 2KB 可评估。
收益有限（≤2KB），且触碰解码器路径风险高，优先级低于 A。

### C. 产品级降级
首页收音机只保留低码率台（≤64kbps）时表现明显更好；可考虑在弱网/内存紧张时优先选低码率源。
属于缓解而非修复。

## 6. 当前状态

- 仓库：`main` 与 `origin/main` 同步，工作区干净，HEAD = `8d0e1f8`
- 设备：已重新烧回该提交的干净固件，功能正常、无崩溃
- `firmware/sdkconfig` 已逐项还原为原始值（`STATIC_RX_BUFFER_NUM=3`、`DYNAMIC=4`、`TCP_WND=5760`、`OOSEQ_MAX_PBUFS=4`、`OOSEQ_TIMEOUT=6`、收件箱 6/6）

## 7. 附：本次用到的复现脚本（Windows 临时目录）

- `_o2p_home_watch.py` —— 复位 → 首页播放 → 抓 `audio stats / eagain / DMA`
- `stream_probe.py <host> <path> <secs>` —— PC 裸 socket 对照测同一 URL
- `save_stream.py` —— 存下流的音频并统计 MP3 帧同步字（判断是否静音源）

---

## 8. 追加调查（2026-10-21 晚）：CBDP 窗口方向已被否决

用户提问："首页只用接收 Codex 的消息数量，一个就是几个数字，为什么要开这么大窗口？"

结论：**方向正确，但设备端做不到**，原因如下。

### 8.1 那行 `SO_RCVBUF` 一直是无效的

`wireless_bridge.c` 里对 CBDP socket 设了 `SO_RCVBUF = 2048`，但 `sdkconfig` 中
**没有 `CONFIG_LWIP_SO_RCVBUF`**（Kconfig 默认 `n`）。lwIP 的 `sockets.c` 把该
case 包在 `#if LWIP_SO_RCVBUF` 内，未开启时 `setsockopt` 直接返回失败、静默忽略。
→ CBDP 连接实际仍使用全局默认 **5760** 接收窗口。

### 8.2 即使开启开关，对 TCP 也无效

`lwip/src/api/api_msg.c` 中 `recv_bufsize` 只在 **raw / UDP** 路径被检查
（第 173、244 行）；**TCP 路径完全不读它**。`conn->recv_bufsize` 对 tcp_pcb 的
`rcv_wnd` 没有任何作用。→ `SO_RCVBUF` 不可能缩小 CBDP 的 TCP 接收窗口。

### 8.3 lwIP 没有"按 socket 改 TCP 接收窗口"的接口

```
grep TCP_WINDOW  ->  只出现在 Kconfig 的说明文字里，代码中无实现
grep SO_RCVBUF   ->  raw/UDP 限制 + netconn 层缓冲，与 tcp_pcb.rcv_wnd 无关
```
ESP-IDF 这份 lwIP 未提供 `TCP_WINDOW` sockopt。唯一杠杆是**全局**
`CONFIG_LWIP_TCP_WND_DEFAULT`，无法只对 CBDP 生效。

### 8.4 实测：反向也不行

- 全局 `TCP_WND` 5760 → 16384：**崩**（3× `wifi:mem fail`、Guru、8× `Open failed`）
- `STATIC_RX_BUFFER_NUM` 3 → 8：**崩**（9× `wifi:mem fail`、12× `Open failed`），
  崩溃时 `dma=12~152`、`largest=1280` —— socket 都建不起来（`fd=-1`），
  所有电台 `unreachable`。

### 8.5 由此得到的新结论

`TCP_SND_BUF` + `TCP_WND` 是**每连接**预留的。首页驻留期间同时存在：
**收音机流 + CBDP 控制连接**，两条连接各按全局默认值占用，合计约 17KB，
而 C3 的 DMA 可用内存无法同时满足它 + WiFi 收包缓冲。

因此**真正有效且不改 Bridge 的改法**是：**别让两条连接同时存在**。

## 9. 推荐方案（替代 A）：首页态不保留 CBDP 数据连接

首页只需要"运行中 / 已完成 / 失败"三个数字，而且 Bridge 已经在
**UDP 8764** 上周期广播 presence、并在 summary 主题推送 `0x0704` 计数事件。

可选实现（择一）：

### 9.1 首页态只收 UDP，不进 TCP 会话（推荐）
- 首页/菜单层级：`wireless_bridge` **不建立 CBDP TCP 会话**，
  仅监听 UDP presence + 由 Bridge 的轻量摘要通道取计数；
- 进入 Codex 页：才 `coding_bridge_start_wireless()` 建 TCP 会话；
- 退出 Codex 页：回到"仅 UDP"状态。
- 收益：首页播放期间**只剩一条 TCP 连接**（电台流），DMA 预算翻倍富余。
- 代价：首页数字刷新依赖 Bridge 的 UDP/摘要通道；若 Bridge 目前只在 TCP
  会话里推 `0x0704`，**需要 Bridge 侧配合**把计数也放到 UDP 广播里
  （这属于对端改动，需另出文档）。

### 9.2 首页数字改用已有 UDP presence 里附带计数
若 presence 报文可以扩展一个计数字段，设备连 TCP 都不必开。
同样需要 Bridge 侧加字段。

### 9.3 若不愿动 Bridge：降低电台码率上限
首页播放优先选 ≤64kbps 的源（实测此类源 `pcm` 可长期接近 100%），
属于缓解手段，不解决并发连接的内存冲突。

---

## 10. 待决策

设备侧已无可挖的余量；方案 9.1/9.2 都需要 Bridge 侧配合。
请确认是否要：
1. 先出 Bridge 侧对接文档（把聚合计数放进 UDP presence 或独立轻量通道），或
2. 先按 9.3 做设备侧缓解，让首页播放至少稳定可用。

---

## 11. 已实施：首页不再占用 CBDP TCP（2026-10-21 晚）

Bridge v2.1.25 按第 9 节实现了 UDP 计数通道
（`codexbridge/devices/home_counters.py`，帧类型 `TYPE_HOME_COUNTERS = 0x1E`，
计数放在 **header TLVs**，只向主动 announce 过的设备单播，2 秒一拍、变化即推）。
设备端已完成对应的一半：

### 11.1 设备改动

| 文件 | 改动 |
|---|---|
| `wireless_bridge.c/.h` | 新增 `wireless_bridge_set_home_only()`；首页/菜单态**拒绝 CBDP TCP 连接**，只保留 UDP；UDP 分支新增 `0x1E` 解析 |
| `coding_bridge.c/.h` | 把 `0x0704` 内联的计数处理抽成 `apply_codex_counts()`，新增 `coding_bridge_apply_udp_home_counters()` 由 UDP 路径调用。两条通道共用同一实现，不会重复计数 |
| `app_registry.cc` | 开机即 home-only；进入任意全屏应用时清除，Codex 页照常建 TCP |
| `codex_bridge_protocol.h` | 新增 `CBDP_MT_HOME_COUNTERS = 0x1E` |

### 11.2 实测（COM4，Bridge v2.1.25 在跑）

| 指标 | 改前 | 改后 |
|---|---|---|
| 满速窗口占比 | 2~3 / 21 | **10 / 19** |
| 单窗口播放时长 | 多为 1~4s | 多为 **10008ms / 10005ms** |
| `wifi:mem fail` | 每轮都有 | 250s 长测 **0 次** |
| 首页 TCP 会话 | 常开 | **0**（日志 4~11 次 `home-only: refused`） |
| 首页计数刷新 | 靠 TCP `0x0704` | 纯 UDP，`codex_state` 每 ~2s 一次 |
| Codex 项目/任务列表 | 正常 | **正常**（进应用后建 TCP，截图为证） |

首页截屏可见宠物右侧徽标显示 `6`/`1`，同时收音机在播。

### 11.3 残余问题（未根除）

播放期 `dma` 空闲仍常掉到 **300~2000 字节**，而 WiFi 每个动态接收缓冲需要
约 **1600 字节连续 DMA 内存**。低于该值时驱动仍会丢帧，TCP 退避重传，于是
仍有 9/19 的坏窗口。

根因是硬件限制：**ESP32-C3 无 PSRAM，内部 RAM 全部 DMA 可寻址**，所以
"内部堆"和"DMA 堆"是同一块。开机链条已贴顶：

```text
post_framework_pre_wifi  free=99476 dma=91748
post_wifi_init           free=67052 dma=59324   (-32.4K)
post_audio_init          free=56364 dma=48636   (-10.7K)
post_radio_prewarm       free=20024 dma=12296   (-36.3K)  <- 解码器池
post_coding_bridge       free=10148 dma=2420    (-9.9K)   <- 最小化桥接
播放期                     free=3.6~4.7K dma=0.9~2.0K
```

### 11.4 下一步可选项（按收益排序）

1. **把解码器池改成非 DMA 需求**：不可行 —— C3 内部 RAM 全 DMA 可寻址。
2. **缩解码器池**：`kOutputSize = 4608 + 2×1024`。MP3 满帧 4608 不能动，
   但共享的 2 个 1KB 块可评估。收益 ≤2KB，风险中等。
3. **降低首页播放码率上限**：优先选 ≤64kbps 源，缓解但非根治。
4. **接受现状**：目前 10/19 满速 + 偶发停顿，已是无 PSRAM 硬件下的实际可行水平。

---

## 12. 解码池调查结论（2026-10-21 深夜）

用户建议："减少一点内存，给收音机多一点缓冲，从解码池入手。"

### 12.1 解码池的真实构成（真机实测，不是估算）

在 `radio_player_prewarm()` 加了分配探针：

```text
poolsize entry=56364 afterOpen=56096 afterBuffers=48408 (open=268 buffers=7688)
poolsize afterWarm=20024 (warm alloc=28384)
```

| 组成 | 字节 | 可调 |
|---|---|---|
| 解码器句柄本体 | 268 | 否 |
| **我们的收发缓冲** (`kOutputSize` 6656 + `kInputSize` 1024) | **7688** | 部分 |
| **解码器内部一次性分配** | **28384 (27.7KB)** | **否** |
| 合计 | ~36.3K | |

关键发现：注释里写的"约 20KB 内部缓冲"**低估了**，实测是 **27.7KB**，
而且这部分在 `esp_audio_codec` 的**预编译库里**
（`lib/esp32c3/libesp_audio_codec.a`，源码未随组件提供），
没有任何 Kconfig 开关可以调小它。**这是解码池里最大且完全不可动的一块。**

### 12.2 我们自己的 7688B 里能省多少

- `kPcmCapacity = 4608`：MP3 MPEG-1 Layer III 满帧 1152 样本 × 2ch × 2B，
  **不能再小**（小于它预热就失败，实测 `result=-8, decoded=0`）。
- `kSharedNetworkBlocks = 2`（+2048B）：这是与输出缓冲共享的尾部网络块。
  可以从 2 降到 1，省 **1024B**。收益很小。

### 12.3 试过并失败的"加缓冲"

| 尝试 | 结果 |
|---|---|
| `STATIC_RX_BUFFER_NUM` 3 → 5 | **崩**：`post_coding_bridge dma=12`、23× `wifi:mem fail`、17× `Open failed`，所有台 `unreachable` |

原因：静态 RX 缓冲（每块约 1600B）是**从 DMA 池直接扣的**，而 C3 的内部堆与
DMA 池是同一片物理 RAM，开机链条已经没有 3.2KB 的余量。

### 12.4 试过并失败/无效的"省内存"

| 尝试 | 结果 |
|---|---|
| USB 驱动缓冲 2048→1024 | **死代码**：console 已用 USB-Serial-JTAG，驱动早已装好，`coding_bridge_init` 的 `if (!usb_serial_jtag_is_driver_installed())` 分支从不执行（开机堆 10144→10148 无变化） |
| console USB 缓冲调小 | Kconfig 无此选项，由 console 驱动固定 |
| 压缩解码器内部 27.7KB | 预编译库，无源码、无开关 |

### 12.5 最终结论

**"减内存 + 加缓冲"这条路在 ESP32-C3 上已经走到尽头。** 具体地：

1. 解码池 36.3KB 里 **27.7KB 是不可动的库内部占用**，我们能控的只有 7688B，
   其中 4608B 是 MP3 满帧硬需求，真正可省的只有 1024B。
2. 加任何静态 WiFi 收包缓冲都会把 `dma` 打到接近 0 并导致所有电台打不开。
3. 省下的内存若不足 1600B（一个收包缓冲），就无法换来一个可用的缓冲块。

**已完成的实质改善**（见第 11 节）：首页不再占 CBDP TCP + 计数走 UDP，
使 `wifi:mem fail` 归零、满速窗口从 2~3/21 提升到 10/19。
剩余 9/19 的停顿源于 DMA 余量偶尔低于 1600B，属于该硬件的天花板。

**若必须彻底解决**，只有换带 PSRAM 的模组（可把 WiFi/lwIP 缓冲移到外部
RAM，内部 DMA 池留给 I2S 与收包）。
