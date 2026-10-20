# 设备端修复与真机验证记录（2026-10-20）

- 设备：AI Passport（ESP32-C3，8MB Flash，无 PSRAM），MAC `98:c3:77:f5:52:70`，IP `192.168.0.39`
- 串口：COM4 @115200
- 固件：本仓库 `main`，ESP-IDF 5.5.3，`idf.py build` 通过后 `idf.py -p COM4 flash`
- 验证方式：`tools/fap_device.py`（FAP_SCREENSHOT_V1 截屏 + 单字符按键注入）

## 修复项

1. **收音机“内存不足 稍后重试”**
   退出完整 Codex 时先停无线桥接、并在菜单/首页树重建前立刻重新抢占 20KB 连续解码池，
   避免“先建树把堆切碎、再抢池失败”的长退避。

2. **Codecs 新建任务卡住**
   `thread.start` 的 RESULT 字段在帧头（不在嵌套 `result` TLV），固件现在两种布局都读；
   拿到 `thread_ref` 后再发一次 `thread.read` 取得 `topic_id` 并完成选中。
   若对端确实没有返回任何线程选择器，则走新增的 `app_coding_catalog_abort()` 明确报错，
   不再永久停在“加载中”。

3. **任务完成提醒**
   `turn.completed / turn.failed / turn.interrupted` 无论完整 Codex 页是否在前台，
   都会更新后台计数并触发提示（按用户语音档位：语音 / 提示音 / 静音），重放事件不重复提醒。

4. **录音转写结果未处理**
   Bridge 对 `AUDIO_*` 事务回的是 `RESULT(method=0x0013, transcript=...)`；
   固件原来只处理 legacy JSON 的 `transcript`，导致录音结束直接报错。
   现在解析该 RESULT 并进入“确认语音文字”流程。

5. **后台状态机**
   `codex_background_apply()` 原来忽略 `status` 字段（`working` 永远算不出运行中）；
   现按 status 词条维护 running，并新增 `codex_background_apply_notice()` 作为
   state/task/status/result 的统一收敛入口。

## 真机验证结果

| 项目 | 结果 | 证据 |
|---|---|---|
| 固件构建 | 通过 | `Project build complete` / `BUILD_EXIT=0` |
| 烧录 | 通过 | `Hash of data verified` ×N，`Hard resetting` |
| 启动 | 通过 | `app_fw: framework ready: 4 apps` |
| 收音机播放 | 通过 | `PCM playback active (2304 bytes)`，串口无“内存不足” |
| 首页宠物 | 通过 | 截屏可见卡通形象（idle 态 `z Z`） |
| 进入 Codex | 通过 | 截屏显示“等待指令 / 开始录音 / 切换任务” |
| 项目/任务列表 | 通过 | 截屏显示项目列表与任务列表 |
| 新建任务 | 通过 | 截屏显示“当前任务：未命名任务”，日志出现新线程 `SUBSCRIBE` |
| 录音 | 通过 | 截屏显示“正在聆听”、计时与电平条 |
| 录音结束转写 | 通过 | 截屏进入“任务完成 / 任务已完成” |
| 任务完成提醒 UI | 通过 | 截屏“完成啦！”与“任务完成”页 |
| 退出 Codex 回收音机 | 部分 | 解码池能重新抢占（堆 40KB→11KB），但自动恢复播放不稳定，按 OK 可正常开始播放 |
| 首页“运行中/完成/失败”数量 | 依赖 Bridge | Bridge 未在 CBDP 下投递 `codex_state` 计数，见 `docs/CodexBridge-CBDP-status-push-issue-2026-10-19.md` |

## 已知限制

- 串口每次打开都会复位 ESP32-C3（DTR/RTS），因此真机脚本每次都要等约 20–35 秒完成
  启动、联网与 CBDP 会话建立后再注入按键。
- 首页运行计数为 0 时，界面按设计隐藏该行（`refresh_home_codex()` 在所有计数为 0 时置空），
  因此“看不到运行中数量”在当前 Bridge 版本下属于预期表现，待 Bridge 侧补齐计数后自动显示。
