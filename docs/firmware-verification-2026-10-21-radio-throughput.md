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
