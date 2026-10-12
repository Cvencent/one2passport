# 口袋百宝箱 · Codex 版（LEO RADIO 基座）

在 LEO RADIO 收音机固件上，新增一个 **Codex 监控与语音对话页**：
设备屏幕显示 Codex 的小动画形象与正在运行的会话，可切进对话看内容，
并用**语音回复**，由电脑端的 Codex 执行。

保留：收音机、麦克风、配网页、设置页、小智 AI
移除：**你在狗叫什么**（word_bear）、**俄罗斯方块**

> PC 端桥接器已经独立到
> [Cvencent/CodexBridge](https://github.com/Cvencent/CodexBridge)。本仓库不再包含
> `windows-client/`、`pair_device.py`、`CodexBridge.exe` 或其运行脚本。

---

## 一、产物与仓库边界

| 产物 | 位置 |
| --- | --- |
| 设备固件源码 | 本仓库 `firmware/` |
| 固件构建产物 | `firmware/build/Internet-Radio.bin`（本地构建） |
| PC 端托盘与协议宿主 | [Cvencent/CodexBridge](https://github.com/Cvencent/CodexBridge) |
| PC 端可执行文件 | [CodexBridge Releases](https://github.com/Cvencent/CodexBridge/releases/latest) |

固件大小与分区余量会随构建变化，以实际 `idf.py build` 输出为准。

---

## 二、烧录

```bash
cd firmware
idf.py -p <COM口> flash monitor
```

或直接用 esptool（`<COM口>` 换成实际端口）：

```bash
python -m esptool --chip esp32c3 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size detect --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/Internet-Radio.bin
```

> **注意**：`flash` 时不要勾选擦除 NVS，否则已保存的 Wi-Fi 与配对 token 会丢失。

---

## 三、按键操作

### 首页 / 菜单：只跑最小化服务

首页与菜单不是完整应用页，只保留两个轻量服务：

- **最小化收音机**：继续播放当前频道，短按 OK 暂停/恢复，不加载完整收音机页。
- **最小化 Codex**：后台接收任务状态、结果和已读同步，更新首页宠物与未读数字，
  不创建完整 Codex 页面。

进入完整应用时，另一个最小化服务会让出资源：进完整收音机时最小化 Codex 退出，
进完整 Codex 时最小化收音机退出，进小智 AI 时两者都退出。退出完整应用后才回到
首页/菜单的最小化层，并按用户在最小化层最后一次选择的播放/暂停状态恢复。

### 完整收音机

| 操作 | 效果 |
| --- | --- |
| 点击 UP / DOWN | 切换电台 |
| 点击 OK | 播放 / 暂停 |
| 长按 OK | 进入设置 |
| **长按 UP** | **进入 Codex 页** |

### 完整 Codex

| 操作 | 效果 |
| --- | --- |
| **长按 UP** | **退出，回到菜单** |
| 点击 UP / DOWN | 上移 / 下移一格（选项、决策、列表） |
| 点击 OK | 确认；在“开始录音”上是开始/停止录音 |
| 录音中再点 OK | 结束录音 → 等待识别 → 确认后发送 |

进入完整 Codex 时会暂停最小化收音机、释放解码池，并启动完整 Codex 页面和无线桥接。
退出完整 Codex 时销毁页面，只保留后台最小化 Codex，不恢复完整收音机页面。

---

## 四、PC 端配置

### 前置条件

1. 电脑已安装 Codex，并且 **Codex App Server 使用 ChatGPT 账户登录**。
   API Key 登录可以用于 Codex CLI 本身，但不能替代桥接器需要的 ChatGPT
   App Server 登录状态。
2. 从 [CodexBridge Releases](https://github.com/Cvencent/CodexBridge/releases/latest)
   下载最新的 `CodexBridge.exe`，或从源码运行。
3. 电脑与设备在同一 **2.4GHz** 局域网。
4. Windows 已装中文语音识别与语音包（用于语音转文字）。

### 第 1 步：设备先联上 Wi-Fi

在设备上长按进入 **设置 → Wi-Fi**，设备会起一个配置热点。
用手机或电脑连该热点，在打开的网页里填家里 **2.4GHz** 的 SSID 与密码。

### 第 2 步：启动 CodexBridge

推荐直接运行 `CodexBridge.exe`，它会以托盘程序运行并提供管理接口。源码模式：

```powershell
cd <CodexBridge 仓库目录>
python -m pip install -e ".[tray]"
python -m codexbridge
```

托盘菜单会列出已发现设备，并可分别配置每个设备启用的能力。
CodexBridge 的本机管理接口默认为 `http://127.0.0.1:8788`：

```powershell
Invoke-RestMethod http://127.0.0.1:8788/api/admin/status
Invoke-RestMethod http://127.0.0.1:8788/api/admin/devices
```

开发或排障时可以只启动协议部分：

```powershell
python -m codexbridge --no-tray
```

---

## 五、统一协议与迁移状态

CodexBridge 的目标协议只有一套，不区分 V1/V2：

```text
发现请求：CODEXBRIDGE_DISCOVER
发现应答：{"device": "...", "device_id": "...", "name": "...", "port": 8765, "capabilities": [...]}
握手顺序：bridge_auth -> bridge_auth_ok -> bridge_ready
消息类型：message、capabilities_update、read_state、read_ack、error
```

能力清单包括 `status`、`text`、`tasks`、`results`、`approval`、`audio`、
`read_ack`、`voice`、`word_bear`。每台设备声明自己支持的能力，CodexBridge
再按设备配置计算实际生效的能力；未启用的能力不会下发。

### 当前固件状态

本分支固件已经完成统一协议迁移，只保留这一套握手：

```text
CODEXBRIDGE_DISCOVER
bridge_auth -> bridge_auth_ok -> bridge_ready
```

- 发现请求精确匹配 `CODEXBRIDGE_DISCOVER`，应答带稳定的
  `device_id`（`aipassport-<mac>`）、`name`、`port` 与 `capabilities`。
- `bridge_auth_ok` 上报 `device_id`、`model`、`name`、`firmware` 与同一份
  `capabilities`，与发现应答一致。
- 设备声明支持 `status`、`text`、`tasks`、`results`、`read_ack`；实际启用项由
  CodexBridge 按设备配置，默认只开最小集合。
- 接收首次及心跳 `bridge_ready.capabilities` 与 `capabilities_update`，据此决定显示
  哪些消息；15 秒未收到 PC 消息判定离线。
- 解析统一 `message` 与 `read_state`；结果页停留满 2 秒上报一次 `read_ack`，其他
  设备已读后同步刷新“已读人数”。
- 旧 `FOLOOS_DISCOVER_V1` 与旧握手代码已删除，不存在 V1/V2 分支。

---

## 六、相对原版 LEO RADIO 的改动

### 新增文件

| 文件 | 说明 |
| --- | --- |
| `main/app_coding.c` | Codex 页面：动画形象、会话列表、对话渲染、按键状态机 |
| `main/coding_flow.c` | 对话流程状态机（录音/识别/确认/排队/运行/审批） |
| `main/coding_bridge.c` | PC 桥接协议：录音上行、语音下行、USB + 无线双通道 |
| `main/wireless_bridge.c` | 统一协议 TCP/UDP 传输层：发现、token 配对与鉴权回调 |
| `main/codex_bridge_protocol.c/.h` | 统一发现、握手、能力、消息分片与 read_ack 编解码 |
| `main/codex_background_state.h` | 最小化 Codex 的状态、未读结果去重与已读清零 |
| `main/codex_read_tracker.c` | 结果页停留 2 秒的已读计时器 |
| `main/ui_sfx.c` | 按键音效 |
| `main/ui_pixel.c` / `.h` | 像素风 UI 基元（面板、标签、选中态） |
| `main/lv_font_cn_16.c` | 中文点阵字体（GB2312 全集 6763 字） |
| `main/audio/*.wav` | 提示音（approval / done） |

### 修改文件

| 文件 | 改动 |
| --- | --- |
| `main/main.cc` | 长按 UP 切换 Codex 页；Host 钩子；WiFi 事件转发桥接层 |
| `components/bsp/src/bsp_audio.c` | 加递归互斥锁，串行化 codec 访问 |
| `components/bsp/*` | 合并 LEO 与 FoloOS 两版 BSP 至 API 并集 |

### 移除

- `ui_menu_chrome.c`、`ui_icons.c` 未纳入本工程，界面改为自绘。
- `word_bear`（你在狗叫什么）全部代码与 `WordBear` 数据目录引用。

---

## 七、排障

| 现象 | 处理 |
| --- | --- |
| CodexBridge 提示未登录 ChatGPT | 在 Codex App Server 使用 ChatGPT 账户登录；API Key 登录不等于可用 |
| 托盘里出现 `legacy:<IP>` 设备 | 那是旧 `bridge.json` 迁移记录；新固件会另以 `aipassport-<mac>` 上线 |
| CodexBridge 发现不到设备 | 确认设备已联网、与电脑同网段，且 UDP 8764 未被防火墙拦截 |
| 设备端提示“麦克风不可用” | 检查 `bsp_audio` 初始化；重启设备 |
| 构建报 `lvgl/lvgl.h not found` | 确认 `main/CMakeLists.txt` 里有 `LV_LVGL_H_INCLUDE_SIMPLE` 定义 |
| 构建报 `undefined reference to wireless_bridge_*` | 确认相关头文件有 `extern "C"` 守卫 |
