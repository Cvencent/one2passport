# one2passport

把一台 FoloToy **AI Passport**（ESP32-C3 小设备）改造成「城市网络收音机 + Codex 编程伴侣」：
联网后自动定位所在城市播放当地电台，也可以把语音指令送到电脑上的 Codex 执行，
任务进度、审批和结果回传到设备屏幕。

> 这是社区项目，与 FoloToy 官方无关，也未获其背书。详见 [NOTICE](NOTICE)。

## 仓库边界

- 本仓库只维护 AI Passport 设备端固件。
- PC 端桥接器已经独立到 [Cvencent/CodexBridge](https://github.com/Cvencent/CodexBridge)。
- 旧版内置 Windows 客户端 `windows-client/` 已从本仓库移除，不再随本仓库发布或维护。

CodexBridge 现在是一套面向多设备、可配置能力的通用协议宿主，不再只服务 AI
Passport。设备端支持的能力由桥接器按设备配置，未开启的能力不会下发到设备。

## 下载

| 产物 | 下载位置 | 说明 |
| --- | --- | --- |
| AI Passport 固件 | [one2passport Releases](https://github.com/Cvencent/one2passport/releases/latest) | `AI-Passport-MERGED.bin`，从 `0x0` 刷入，适配官方 web flasher |
| 仅应用分区固件 | [one2passport Releases](https://github.com/Cvencent/one2passport/releases/latest) | `Internet-Radio.bin`，从 `0x10000` 刷入 |
| CodexBridge.exe | [CodexBridge Releases](https://github.com/Cvencent/CodexBridge/releases/latest) | Windows 10/11 托盘宿主，单实例服务多个设备 |
| CodexBridge 源码 | [Cvencent/CodexBridge](https://github.com/Cvencent/CodexBridge) | 协议、设备管理、能力开关与托盘界面 |

## 仓库结构

- `firmware/` —— 设备端固件（ESP-IDF 5.5.3 / ESP32-C3）
- `NOTICE` —— 项目声明与外部桥接器入口

## 固件

- 目标硬件：ESP32-C3、8 MB Flash、无 PSRAM；ES8311 音频 codec；UP / DOWN / OK 三键 + 屏幕
- 城市电台：按网络位置自动定位，也可手动切换城市
- 设备端配网：直接在设备上搜索 Wi-Fi、输入密码，无需手机
- 定时播放、亮度/熄屏、已存网络管理
- Codex 页（长按 UP 进入）：显示会话状态，支持语音对话
- 全中文界面，内置点阵字库

按键操作、烧录参数与 Codex 页行为见
[`firmware/CODEX-README.md`](firmware/CODEX-README.md)；纯收音机部分见
[`firmware/README.md`](firmware/README.md) 与
[`firmware/README.zh_CN.md`](firmware/README.zh_CN.md)。

### 构建

```sh
. $IDF_PATH/export.sh
cd firmware
idf.py set-target esp32c3
idf.py build
idf.py -p <PORT> flash monitor
```

生成可从 `0x0` 刷入的合并镜像：

```sh
python firmware/merge_bin.py --build-dir firmware/build --out AI-Passport-MERGED.bin
```

## Codex 接入状态

这条 `main` 分支里的固件仍然使用旧的 `FOLOOS_DISCOVER_V1` 发现请求和旧握手：

```text
FOLOOS_DISCOVER_V1
bridge_auth -> bridge_auth_ok
```

新的通用 CodexBridge 使用统一协议：

```text
CODEXBRIDGE_DISCOVER
bridge_auth -> bridge_auth_ok -> bridge_ready
```

两者目前**不能直接完成新协议配对**。因此：

- 启动 CodexBridge 后，旧的 `%LOCALAPPDATA%\FoloOS\bridge.json` 只会被迁入
  `%LOCALAPPDATA%\CodexBridge\devices.json`，在托盘里显示为待迁移设备。
- AI Passport 要真正接入通用协议，需要把固件发现应答补充 `device_id`、`name`、
  `capabilities`，把握手升级为 `bridge_auth_ok -> bridge_ready`，并实现
  `message`、`read_state`、`read_ack` 等统一消息。
- 在固件完成迁移前，不要把旧固件描述成已经兼容新版 CodexBridge。

## 测试

固件测试位于 `firmware/tests/`。PC 端协议、设备管理、已读同步与托盘测试位于
[CodexBridge 仓库](https://github.com/Cvencent/CodexBridge)。

```sh
cd firmware
python tools/tests/run_tests.sh
```

## 隐私

本仓库只包含源码与文档：不含 Wi-Fi 名称/密码、设备 MAC、配对 token，也不含开发者本机的绝对路径。
构建产物（`build/`、`managed_components/`、`sdkconfig`、日志与 BIN）一律不入库，只随 Releases 发布。

## 许可

MIT，详见 [`firmware/LICENSE`](firmware/LICENSE)。内置字库、Espressif 组件与第三方素材各自遵循其原始许可。

---

## English

`one2passport` is an unofficial community project for the FoloToy **AI Passport**
(ESP32-C3). It combines a city internet radio with a Codex companion page: the
device locates your city and streams local stations, and can send voice commands
to Codex running on a paired Windows PC, with progress, approvals and results
mirrored back to the device screen.

This repository is now firmware-only. The PC-side bridge lives at
[Cvencent/CodexBridge](https://github.com/Cvencent/CodexBridge). The old bundled
`windows-client/` implementation has been removed and is no longer maintained
here. Firmware binaries ship through this repository's Releases page; the
CodexBridge installer ships through the CodexBridge repository.

The current `main` firmware still speaks the legacy `FOLOOS_DISCOVER_V1`
discovery and handshake protocol. The unified `CODEXBRIDGE_DISCOVER` protocol
requires a firmware migration before direct pairing is available. Licensed
under MIT; see `firmware/LICENSE`.
