# 口袋百宝箱 · Codex 版（LEO RADIO 基座）

在 LEO RADIO 收音机固件上，新增一个 **Codex 监控与语音对话页**：
设备屏幕显示 Codex 的小动画形象与正在运行的会话，可切进对话看内容，
并用**语音回复**，由电脑端的 Codex 桌面端执行。

保留：收音机、麦克风、配网页、设置页、小智 AI
移除：**你在狗叫什么**（word_bear）、**俄罗斯方块**

---

## 一、成品位置

| 产物 | 路径 |
| --- | --- |
| 设备固件 | `firmware/build/Internet-Radio.bin`（构建产物） |
| 工程源码 | `firmware/` |
| PC 桥接包 | `windows-client/` |

固件大小 **2,223,712 字节（2.12 MB）**，factory 分区余量 **900.4 KB**。

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
> 构建日志里的 `recovery 分区 too small` 警告是**预期的**——1MB 的 recovery
> 分区本来就装不下收音机应用，LEO RADIO 原版构建时同样如此。

---

## 三、按键操作

### 收音机主界面

| 操作 | 效果 |
| --- | --- |
| 点击 UP / DOWN | 切换电台（原有） |
| 点击 OK | 播放 / 暂停（原有） |
| 长按 OK | 进入设置（原有） |
| **长按 UP** | **进入 Codex 页**（新增） |

### Codex 页

| 操作 | 效果 |
| --- | --- |
| **长按 UP** | **退出，回到收音机主界面**（全局返回，优先级最高） |
| 点击 UP / DOWN | 上移 / 下移一格（选项、决策、列表） |
| 点击 OK | 确认；在"开始录音"上是**开始/停止录音** |
| 录音中再点 OK | 结束录音 → 等待识别 → 确认后发送 |

进入 Codex 页时会自动**暂停收音机播放**并等播放器空闲，避免与语音链路抢音频。

---

## 四、PC 端配置（两步）

### 前置条件

1. 电脑已装 **Codex 桌面端**并登录（本工程会自动定位
   `%LOCALAPPDATA%\OpenAI\Codex\bin\<版本哈希>\codex.exe`，无需加 PATH）。
2. 装 **Python 3.10+**。
3. 电脑与设备在同一 **2.4GHz** 局域网。
4. Windows 已装中文语音识别与语音包（用于语音转文字）。

### 第 1 步：设备先联上 Wi-Fi

在设备上长按进入 **设置 → Wi-Fi**，设备会起一个配置热点。
用手机或电脑连该热点，在打开的网页里填家里 **2.4GHz** 的 SSID 与密码。

> 这是 LEO RADIO 原有的配网页，本工程未改动它。

### 第 2 步：PC 端配对并启动

```bash
cd windows-client/runtime
python pair_device.py          # 自动发现设备并完成配对
python windows_bridge.py       # 启动桥接(保持运行)
```

`pair_device.py` 会把地址与密钥写入
`%LOCALAPPDATA%\FoloOS\bridge.json`，与 `windows_bridge.py` 共用同一份配置。
配对只需做一次；以后直接启动桥接即可。

也可用图形入口：双击 `windows-client/安装与管理编程伴侣.bat`，
选 `1. 一键安装或修复`。

---

## 五、配对机制说明（TOFU）

**本工程的配对方式与 FoloOS 原版不同，这是必须了解的一点。**

FoloOS 原版走 **USB 串口**下发 token：PC 端随机生成 32 位 token，写进设备，
两边用同一个 token 做鉴权。

本工程基于 **LEO RADIO**，配网页由 `esp-wifi-connect` 提供——那套网页是**固定表单**
（只有 SSID 和密码两个输入框），无法像 FoloOS 那样在表单里附加一个自定义 token 字段。

因此设备端改为 **TOFU（首次信任配对）**：

- 设备本地 token 为**空**时 → 接受电脑端带来的**第一个**合法 token（≥16 字符）并永久落盘；
- 配对**之后** → 必须严格匹配，不再接受新 token。

所以：

| 情况 | 该怎么做 |
| --- | --- |
| 全新设备、从未配对 | 直接跑 `pair_device.py` |
| 已配对过，想重新配 | 用 `--token <旧token>` 复用设备里存的那个 |
| 旧 token 已丢失 | 需要先清掉设备端配对（擦 NVS / 恢复出厂）再重新配对 |

> 也可继续使用官方 `windows_wifi_setup.ps1` 走 USB 串口配网路径：
> 设备端 `coding_bridge` 完整保留了 `wifi_setup` 事件处理，
> 它会保存脚本下发的 token 并转发换网请求。此时**不需要**再跑 `pair_device.py`，
> 直接复用 `bridge.json` 里脚本写入的 token 即可。

---

## 六、相对原版 LEO RADIO 的改动

### 新增文件

| 文件 | 说明 |
| --- | --- |
| `main/app_coding.c` | Codex 页面：动画形象、会话列表、对话渲染、按键状态机 |
| `main/coding_flow.c` | 对话流程状态机（录音/识别/确认/排队/运行/审批） |
| `main/coding_bridge.c` | PC 桥接协议：录音上行、语音下行、USB + 无线双通道 |
| `main/wireless_bridge.c` | 纯 TCP/UDP 传输层（TOFU 配对、设备发现） |
| `main/ui_sfx.c` | 按键音效 |
| `main/ui_pixel.c` / `.h` | 像素风 UI 基元（面板、标签、选中态） |
| `main/lv_font_cn_16.c` | 中文点阵字体（GB2312 全集 6763 字） |
| `main/audio/*.wav` | 提示音（approval / done） |

### 修改文件

| 文件 | 改动 |
| --- | --- |
| `main/main.cc` | 长按 UP 切换 Codex 页；5 个 Host 钩子；WiFi 事件转发桥接层 |
| `components/bsp/src/bsp_audio.c` | 加**递归互斥锁**，串行化 codec 访问 |
| `components/bsp/*`（7 个文件） | 合并 LEO 与 FoloOS 两版 BSP 至 **API 并集** |

### 移除

- `ui_menu_chrome.c`（970 KB）、`ui_icons.c`（415 KB）——本工程自绘 UI，未参与链接；
  原文件未纳入本工程。
- `word_bear`（你在狗叫什么）全部代码与 `WordBear` 数据目录引用。

---

## 七、已知限制

1. **未做真机验证。** 本轮只完成到「编译通过、零警告、Flash 核算」；
   未在 AI Passport 实机上烧录运行过，实际显示效果与语音链路需上机确认。
2. `input_task` 栈为 4096 字节。已通过对照 **FoloOS 原版**验证：
   其按键回调走完全相同的 `app_coding_*` 调用链，跑在 **3584** 字节
   （ESP-IDF 默认 `esp_timer` 栈）上且已发布，故 4096 足够。
   若实机出现栈溢出告警，再加 1024 即可。
3. `wireless_bridge` 任务栈保持与 FoloOS 一致的 **7168** 字节——
   该任务收到 JSON 后会一路走到 LVGL 渲染，且自带约 1.8 KB 的局部缓冲。
4. 音频 codec（ES8311）只有一路。收音机、按键音、录音/播报三条链路已用
   递归互斥锁串行化，但**同时**播放与录音在物理上仍不可能。

---

## 八、排障

| 现象 | 处理 |
| --- | --- |
| PC 端提示"未发现设备" | 确认设备已联网、与电脑同网段；关掉路由器的访客网络隔离；放行 Python 的 UDP 8764 |
| 配对时"连上了但没回包" | 设备已配对过，token 不匹配。用 `--token <旧token>` 重跑，见第五节 |
| 设备端提示"麦克风不可用" | 检查 `bsp_audio` 初始化；重启设备 |
| 构建报 `lvgl/lvgl.h not found` | 确认 `main/CMakeLists.txt` 里有 `LV_LVGL_H_INCLUDE_SIMPLE` 定义 |
| 构建报 `undefined reference to wireless_bridge_*` | 确认相关头文件有 `extern "C"` 守卫（C 实现的头文件被 C++ 调用时必须加） |
