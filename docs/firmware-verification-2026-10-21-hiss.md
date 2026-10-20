# 设备端修复与真机验证记录（2026-10-21：首页收音机电流声 / 完成提示音）

- 设备：AI Passport（ESP32-C3，8MB Flash，无 PSRAM），MAC `98:c3:77:f5:52:70`，IP `192.168.0.39`
- 串口：COM4，ESP-IDF 5.5.3，`idf.py build` + `idf.py -p COM4 flash`
- 验证工具：`tools/fap_verify.py`（按键注入 + 串口采集）
- Bridge：本机已在运行的 CodexBridge `2.1.21`，本轮未启动第二个实例、未修改 Bridge 仓库

## 1. 首页播放时的轻微电流声

### 根因（真机证据）

稳态连续 10 秒窗口一直是 `clipped=0 / gap_max=12~20ms / pcm≈9980ms`，
说明电流声不是持续削波，也不是网络断流。真正的偶发噪声来自音频所有权切换：

1. **格式反复重开 codec**：提示音固定 16kHz，而电台是 44.1/48kHz。
   `ui_sfx` 播完撒手后，收音机下一帧再次发现格式不同，
   `bsp_audio_set_format()` 又走一次 `close → enable → open → prime`。
   在 44.1kHz 与 16kHz 之间来回，每次开合都可能在模拟输出上留下"滋"的一声。
   真机日志可见同一段时间里出现两条 `codec 打开`。
2. **让位窗口内 DAC 仍在放音**：收音机是"不写 I2S 但继续解码"，
   它最后交给 DMA 的那一帧在让位后仍会播完；此时提示音任务已经在写新的
   16kHz 数据，两段不同内容的 PCM 交叠，听感就是短促电流声/咔哒。

### 修复（仅设备端）

- `bsp_audio` 新增 `bsp_audio_get_format()` / `bsp_audio_set_muted()`：
  不关 codec 就能读取当前采样率、静音/解除静音。
- `ui_sfx` **不再改采样率**：让位前用 `bsp_audio_get_format()` 读到电台当前
  速率，提示音（含 16kHz WAV 与合成音）按该速率重采样/合成，
  `bsp_audio_set_format()` 变成同格式 no-op，codec 全程不 close/open。
- 让位期间先静音，播完先写 30ms 数字静音把已进 DMA 的最后一帧冲掉，
  再解除静音播提示音；播完仍保持静音，直到收音机恢复格式并写新帧，
  由收音机侧 `bsp_audio_set_muted(false)` 解除。
- 首页不再做用不到的频谱计算：`radio_ui_leo_page_visible()` 为 false 时
  跳过 `calculate_levels()` 与 `radio_ui_set_audio_levels()`。
  这次改动前 `ui_sfx` 的日志是 `rate=16000`，改动后是 `rate=44100/48000`，
  且真机日志里 **codec opens 保持 1**。

### 真机验证

| 项目 | 结果 | 证据 |
|---|---|---|
| 稳态无持续电流声 | 通过 | 连续窗口 `clipped=0`、`gap_max=11~24ms`、`pcm≈10008ms` |
| 提示音不再重开 codec | 通过 | 一次完整运行 `codec opens: 1`，`ui_sfx: play ... rate=44100` |
| 提示音与电台共采样率 | 通过 | `ui_sfx: play effect=3 yield_radio=1 rate=44100` / `rate=48000` |
| 首页频谱计算已跳过 | 通过 | 首页窗口无 16 段 RMS 计算，回收音机页仍正常刷新电平条 |
| 无崩溃 / 无内存耗尽 | 通过 | 全程 0 `Guru`、0 `Stack protection`、0 `wifi:mem fail` |

## 2. 任务完成提醒（附带修复）

### 根因

Bridge 在 CBDP 上只推送**聚合计数**事件 `system.codex_state`（`0x0704`，
含 running / completed_unread / failed_unread），并不会把每条
`turn.completed` 转成 CBDP 推给设备。设备端原来只在收到
`turn.completed` 时才响铃，所以真实环境里"任务完成"一直不会响。

### 修复

`coding_bridge.c` 的 `0x0704` 分支现在把上一次的 completed/failed 计数
与本次比较：

- 首次收到只当作基线（历史重放不响）；
- 计数上升时先"待定"，**4 秒落地窗口**内计数若回落（说明是别的客户端已读，
  而不是新完成）就取消；
- 计数保持 4 秒不回落才算真实完成：failed → `UI_SFX_FAIL`，
  completed → 按语音档位播 `UI_SFX_DONE_SPEECH` / `UI_SFX_CONFIRM`。

> 直接"计数变大就响"在真机上一次误响了 7 声：Bridge 的未读数会随其它客户端
> 已读在 2↔3 之间抖动。4 秒落地窗口就是针对这个实测量出来的抖动特征。

### 真机验证

| 项目 | 结果 | 证据 |
|---|---|---|
| 首页收音机播放中收到完成提醒 | 通过 | `codex_state: running=2 done=3 failed=0` 后 4 秒内出现 `ui_sfx: play effect=3 yield_radio=1 rate=48000` |
| 其它客户端已读导致的计数回落不误报 | 通过 | `done` 在 2↔3 间反复震荡时不再连续响铃（加 4 秒落地窗口） |
| 提醒不再打断收音机 | 通过 | 提示音期间不出现 `codec 打开`，播完 `pcm≈9978ms/10s` |
| 运行中/完成数量与首页宠物 | 通过 | `codex_state: running=2 done=2 failed=0` 时首页宠物为忙碌态 |
