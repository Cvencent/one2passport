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

## 追加修复：设置里切换城市电台（2026-10-20 第二轮）

用户复现：设置 → 城市电台，把福建切到重庆，退出设置时卡住，过一会自己回到首页，
再打开仍是泉州。

串口抓到两条独立根因：

1. **崩溃**：`Guru Meditation Error: Core 0 panic'ed (Stack protection fault)`，
   `Detected in task "radio_network"`，`MEPC=_vfprintf_r`（`esp_log` ←
   `esp_http_client_read` → `lwip_select` 的日志路径）。该任务栈只有 2048B，
   还带 `bytes[1024]` 局部数组，切城市触发重连时越界。
   → 栈提升到 3072B（只 +1KB 静态 DRAM）。
2. **城市没生效**：`catalog task create failed (free=3788 largest=1280)`。
   搜索任务要 6KB 动态栈 + HTTP 客户端连续内存，而最小化收音机的 20KB
   解码池占着堆，创建必然失败；失败路径当时静默保留旧列表。
   → 搜索前释放解码池、等到足够连续内存、搜索完再抢回；显式选城市时不再
   沿用旧缓存（auto 定位仍保留原保护逻辑）。

真机验证（Bridge 为本地运行中的 v2.1.11，未启动第二个实例）：

| 项目 | 结果 | 证据 |
|---|---|---|
| 切城市不再崩溃 | 通过 | 全程无 `Guru Meditation` / `Stack protection` |
| 搜索任务创建 | 通过 | `catalog task started for city '重庆' (free=42896 largest=29696)` |
| 城市搜索生效 | 通过 | `Manual city search '重庆' returned 2 playable stations` / `Manual province state 'Chungking' now totals 8 stations` / `Installed local catalog with 14 stations` |
| 播放新城市电台 | 通过 | `Playing 重庆…FM88.7` |
| 退出设置 | 通过 | 返回菜单，无卡死/重启 |
| 重启后保持 | 通过 | 重启后 `Restored cached radio catalog: 14 stations`（重庆目录） |
| 启动堆余量 | 通过 | `post_radio_prewarm free=18640 largest=10752`（仅比基线少约 1KB） |

### 第二轮补充（同一天，真机复测）

除上面的崩溃与任务创建问题外，还修掉两个会让"切了城市又变回泉州"的次要原因：

3. **NVS 写缓存失败**：`catalog cache save failed: ESP_ERR_NVS_NOT_ENOUGH_SPACE`。
   覆盖写 ~6KB 的 `catalog_v3` 时旧条目仍占槽位，导致新城市目录没落盘、重启后
   恢复旧列表。→ 写入前先 `nvs_erase_key` 再写。
4. **缓存未与城市绑定**：新增 `catalog_city` 记录缓存对应的城市；启动时若与
   `city_name` 不一致就作废缓存并强制重新搜索（自动定位的旧保护逻辑保留）。

最终真机结果：切重庆不再崩溃、搜索任务正常创建并返回重庆电台、退出设置回到菜单、
重启后仍恢复重庆目录（`Restored cached radio catalog: 14 stations`）。
