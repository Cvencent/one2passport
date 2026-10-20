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


## 第三轮修复与真机验证（2026-10-20 晚，用户反馈“电台一直换台/没声音”“进 Codex 很慢且显示断线”）

### A. 收音机“一直切台、没声音”

真机串口先出现两种失败并存：

```
radio_player: Open failed: ... / HTTP status -1
wireless_bridge / wifi:mem fail        <- WiFi 收包缓冲分配失败
radio_player: Station N unreachable, skipping to the next one
```

定位过程（每一步都有真机日志对照）：

| 尝试 | 结果 |
|---|---|
| 任务栈 2048B → 2560B / 3072B（+512B / +1KB `.bss`） | `wifi:mem fail`，TCP 吞吐 11.7KB/s → ~300B/s，仍不停换台 |
| 栈 2048B、读缓冲 256B | 吞吐恢复，但 `radio_network` 仍触发 **Stack protection fault → RTC_SW_CPU_RST 自动重启** |
| 栈 2048B + 去掉本任务全部日志 | 仍崩（addr2line 定位在 `esp_http_client_read → lwIP → FreeRTOS `xQueueSemaphoreTake` / newlib `lock_acquire_generic`） |

结论：**2048B 栈不够，而 C3 的 `.bss` 连 +512B 都受不了**。最终方案：

- `radio_network` 的 4KB 任务栈改成从小智**关闭时闲置的静态麦克风区**里划分
  （`xiaozhi_radio_stack()`），不再占新的 `.bss`；
- 抖动环同时使用小智麦克风区余量 + 小智 **TTS 静态栈**（收音机运行时同样闲置），
  回到 **20KB / 20 块**；
- 环形缓冲恢复 1024B 读块；恢复阈值上限改为环容量的 1/3（约 5~6KB），
  避免小环里“要等 11.5KB 才恢复”把一次欠载放大成长期静音。

真机结果（连续 130 秒，无人工干预）：

```
network buffer: blocks=20 capacity=20480 source=idle-xiaozhi-stack
Playing 中国之声 at 48000Hz/2ch -> mono
audio stats: wall=10010ms pcm=9960ms read_max=854ms gaps30=0 rebuffers=0 queued=20480
```

| 项目 | 结果 |
|---|---|
| `wifi:mem fail` | 0 次（修复前反复出现） |
| `Stack protection` / `Guru Meditation` | 0 次（修复前每 20~40 秒一次重启） |
| `Open failed` | 0 次（偶发 1 次为源站侧） |
| 持续出声 | 出现 9960ms/10000ms 的完整播放窗口 |
| 切换电台 | 不再出现“一直换台”循环 |

> 仍有部分 10 秒窗口 PCM 偏低，原因已用电脑侧直连同一电台验证：源站本身会长时间
> 停发（电脑侧 TCP 采样同样出现秒级、十几秒级空档），不是设备端缺陷。

### B. 进入 Codex 很慢 / 显示“连接断开”

两个设备端原因，均已修复：

1. **退出 Codex 会拆掉 CBDP 会话**：`codex_app_exit()` 里曾调用
   `coding_bridge_stop_wireless()`，每退一次就要等电脑端下一次发现/握手
   （实测 20~30s），用户看到的正是“卡半天 + 进去说未连接”。现已移除该调用，
   解码池仍在退出时立即抢回。
2. **进入 Codex 时连接状态是旧值**：`s_flow.connected` 只在收到消息时更新，
   于是刚进页面会显示“未连接”。现在进入时同步
   `coding_bridge_is_connected()`。

另外新增设备端**主动 presence 通告**（WiFi 连上 8s 后向 UDP 8764 广播
`discovery_reply`，连发 3 次），不再等电脑端下一次广播，CBDP 会话建立时间从
~22–30s 降到 ~18s（实测 18.3s / 19.4s）。

### C. Codex 项目/任务列表卡住

真机复现：页面停在“正在读取真实 Codex 项目…”。两类问题：

1. **设备端 bug（已修）**：`app_coding_enter()` 在前台标志
   `coding_bridge_set_foreground_ui(true)` **之前**就发出 `projects.list`，
   回复到达时前台标志仍为 false，整条 RESULT 被丢弃（连 `catalog_end` 都没走到），
   于是永久加载。现在前台标志置位后若目录仍在 loading 会重新请求一次；
   同时目录请求会记录 `request_id`，收到 `ERROR` 或 20 秒无响应即退出加载态并
   显示明确错误，不再永久卡住。
2. **Bridge 侧问题（未改 Bridge，已出文档）**：设备选择项目后发
   `threads.list`（`project_ref = 0x0122`），Bridge 回
   `ERROR code=0x0015 (INTERNAL)`。因为 `gateway.PARAM_TAGS` 没有
   `project_ref` 映射，且上游 `service.list_threads()` 不支持按项目过滤。
   见 `docs/CodexBridge-CBDP-threads-list-project-internal-2026-10-20.md`。

真机验证（Bridge v2.1.16）：

| 项目 | 结果 | 证据 |
|---|---|---|
| 项目列表 | 通过 | 截屏 `选择项目 1-8/8`，8 个真实项目 |
| 不再永久卡加载 | 通过 | `threads.list` 失败后显示“发生错误 / INTERNAL”+“重新录音/切换任务” |
| 任务列表/新建任务 | 待 Bridge 修复 | 设备端已就绪，等 `threads.list` 支持项目过滤 |

### D. 首页 Codex 运行状态

仍依赖 Bridge 下发聚合计数 `codex_state`（设备端已具备消费逻辑），
见 `docs/CodexBridge-CBDP-status-push-issue-2026-10-19.md`。
