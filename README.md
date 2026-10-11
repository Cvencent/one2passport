# one2passport

把一台 FoloToy **AI Passport**（ESP32-C3 小设备）改造成「城市网络收音机 + Codex 编程伴侣」：
联网后自动定位所在城市播放当地电台，也可以把语音指令送到电脑上的 Codex 执行，
任务进度、审批和结果回传到设备屏幕。

> 这是社区项目，与 FoloToy 官方无关，也未获其背书。详见 [NOTICE](windows-client/NOTICE)。

## 下载

固件与 Windows 客户端见 **[Releases](https://github.com/Cvencent/one2passport/releases/latest)**。

| 文件 | 说明 |
| --- | --- |
| `AI-Passport-MERGED.bin` | 完整固件镜像，从 `0x0` 刷入。推荐，适配官方 web flasher |
| `Internet-Radio.bin` | 仅应用分区，从 `0x10000` 刷入 |
| `CodexBridge.exe` | Windows 10/11 托盘客户端，单文件，免装 Python |

## 仓库结构

- `firmware/` —— 设备端固件（ESP-IDF 5.5.3 / ESP32-C3）
- `windows-client/` —— Windows 托盘应用与桥接运行时

## 固件

- 目标硬件：ESP32-C3、8 MB Flash、无 PSRAM；ES8311 音频 codec；UP / DOWN / OK 三键 + 屏幕
- 城市电台：按网络位置自动定位，也可手动切换城市；FM 指针只在能解析出真实频率时才显示读数
- 设备端配网：直接在设备上搜索 Wi-Fi、输入密码，无需手机
- 定时播放、亮度/熄屏、已存网络管理
- Codex 页（长按 UP 进入）：显示会话状态，支持语音对话
- 全中文界面，内置点阵字库

按键操作、烧录参数、PC 端配对机制（TOFU）等细节见
[`firmware/CODEX-README.md`](firmware/CODEX-README.md)；纯收音机部分见
[`firmware/README.md`](firmware/README.md) 与 [`firmware/README.zh_CN.md`](firmware/README.zh_CN.md)。

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

## Windows 客户端

把设备接到电脑上的 Codex。

- 免安装：下载 Releases 里的 `CodexBridge.exe`，双击运行即可
- 从源码运行：

```powershell
cd windows-client
python -m pip install -r runtime/requirements.txt
python runtime/tray_app.py
```

首次使用三步：双击 `CodexBridge.exe` → 确认设备已连上 Wi-Fi → 右键托盘图标选「配对设备…」。

细节见 [`windows-client/README-Windows.md`](windows-client/README-Windows.md)
与 [`windows-client/CodexBridge-托盘程序说明.md`](windows-client/CodexBridge-托盘程序说明.md)。

### 打包 EXE

```powershell
cd windows-client/runtime
python -m pip install pyinstaller pystray pillow
python -m PyInstaller --noconfirm --clean CodexBridge.spec
# 产物：runtime/dist/CodexBridge.exe
```

## 测试

主机侧桥接运行时的测试（Python 3.10+）：

```sh
cd windows-client
python tests/run_runtime_tests.py
```

## 隐私

本仓库只包含源码与文档：不含 Wi-Fi 名称/密码、设备 MAC、配对 token，也不含开发者本机的绝对路径。
构建产物（`build/`、`managed_components/`、`sdkconfig`、日志、EXE 与 BIN）一律不入库，只随 Releases 发布。

## 许可

MIT，详见 [`firmware/LICENSE`](firmware/LICENSE) 与 [`windows-client/LICENSE`](windows-client/LICENSE)。
内置字库、Espressif 组件与第三方素材各自遵循其原始许可。

---

## English

`one2passport` is an unofficial community project for the FoloToy **AI Passport**
(ESP32-C3). It combines a city internet radio with a Codex companion page: the
device locates your city and streams local stations, and can send voice commands
to Codex running on a paired Windows PC, with progress, approvals and results
mirrored back to the device screen.

Everything is source-only in this repository. Prebuilt `CodexBridge.exe` and the
firmware `.bin` files ship through the Releases page. Firmware targets ESP-IDF
5.5.3 on ESP32-C3 (8 MB flash). The Windows client is a Python tray application
packaged with PyInstaller. See `firmware/CODEX-README.md` and
`windows-client/README-Windows.md` for details. Licensed under MIT.