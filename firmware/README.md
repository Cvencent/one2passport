# LEO RADIO 城市网络收音机

把一台 FoloToy AI Passport 变成随身城市电台：联网后自动定位你所在的城市，拉取当地正在热播的电台，用三颗按键就能换台、播放、设定时。

> 中文详细说明见 [README.zh_CN.md](README.zh_CN.md)。当前版本 **1.4.4**。

## 特性

- **城市电台**：自动按网络位置定位，也可手动切换北京、上海、广州、深圳、长沙、杭州、成都。
- **调谐指针**：换台时指针沿 88–108 MHz 刻度扫到目标位置；能从电台名解析出真实频率的台会显示频率读数。
- **设备端配网**：直接在设备上搜索 Wi-Fi、输入密码，无需手机；也保留 `192.168.4.1` 网页配网。
- **定时播放**：15 / 30 / 60 / 90 分钟到点停止并进入深度睡眠。
- **屏幕设置**：亮度四档，自动熄屏可选，熄屏不打断播放。
- **网络管理**：查看已保存网络、设为首选、逐个删除。
- **全中文界面**：界面文字使用项目内置的原生字库渲染。

## 快速开始

需要 [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) 5.5.3 或更新版本。

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p <串口> flash
```

首次开机会自行打开名为 `LEO Radio` 的配网热点，按屏幕提示完成配网即可。

## 调谐指针是怎么工作的

公开电台目录里只有一小部分电台在名称中写明了 FM 频率（实测约 12–15%）。为了让指针既诚实又始终有反馈，它分两种情况：

| 情况 | 指针位置 | 频率读数 |
| --- | --- | --- |
| 电台名含真实频率，如 `上海经典音乐广播 FM94.7` | 按真实频率落在刻度上 | 显示 `94.7` |
| 电台名无频率，如 `北京新闻广播` | 按电台在列表中的序号均匀分布 | 留空 |

指针任何时候都会动，但**读数只在有据可依时才出现**——不会编造一个设备实际收不到的频率。

频率解析见 [`main/radio_catalog.cc`](main/radio_catalog.cc) 的 `parse_frequency()`，它按置信度依次尝试 `FM94.7`、`94.7`、`971` 三种写法，并处理裸数字的歧义（`971` 是 97.1 MHz，而 `FM 101` 是 101.0 MHz），只接受落在 87.0–108.0 MHz 广播频段内的结果。

## 目录结构

```
main/           应用代码：界面、播放器、电台目录、设置、字库
components/bsp  板级支持：屏幕、音频、按键、电池、I2C
components/esp-wifi-connect
                Wi-Fi 连接与配网门户
tools/          彩蛋资源打包脚本
partitions.csv  分区表
```

## 关于蓝牙彩蛋

设置里的「蓝牙」是一个趣味彩蛋，会播放一段短视频，**并不提供真正的蓝牙耳机或音箱连接功能**（硬件不具备该能力）。

彩蛋资源是独立的数据包，因所用视频素材属第三方内容，**未包含在本仓库中**。未安装资源包时进入该菜单会提示「彩蛋未安装」，不影响收音机的任何其他功能。如需自制，可用 [`tools/build_easter_egg_resource.py`](tools/build_easter_egg_resource.py) 把你自己的视频打包后写入 `0x360000`。

## 隐私

本仓库不包含任何 Wi-Fi 名称、密码、设备 MAC 或本机路径。配网热点使用通用名称 `LEO Radio`。

若要把已配过网的实体设备转交他人，请先清除已保存的网络记录：

```bash
esptool.py erase_region 0x9000 0x6000
```

## 致谢与许可

本项目以 [MIT License](LICENSE) 开源。

构建时会自动拉取 Espressif 官方组件（`esp_audio_codec`、`esp_codec_dev`、`button`、`esp_lvgl_port`）与 LVGL，它们各自遵循其原有许可。界面字库由 [Noto Sans SC](https://fonts.google.com/noto/specimen/Noto+Sans+SC)（SIL Open Font License 1.1）生成子集。电台目录数据来自公开的 [Radio Browser](https://www.radio-browser.info/) API。
