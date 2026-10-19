# CodexBridge CBDP 上行互通问题

日期：2026-10-18

## 环境

- 设备：ESP32-C3，`aipassport-98c377f55270`，`192.168.0.39`
- 固件：本仓库当前工作区版本，CBDP v1
- 桥接器：本机运行中的托盘版
  `D:\codexBrigexx\CodexBridge\CodexBridge.exe`
- 测试方式：只使用正在运行的桥接器；未重启、未改配置、未改桥接器数据

## 结论摘要

桥接器与设备的 CBDP 握手、鉴权、心跳和能力协商都正常，设备持续在线。
本次测试没有先选中活动会话，因此“文本上行没有生成 turn”不能单独判定为
桥接器缺陷；确定的问题是没有返回任何 `RESULT` / `ERROR`，设备无法知道
上行被丢弃。音频上行约 10 KB 后失败，需要桥接器侧确认会话线程是否在上行
期间及时排空 socket。

## 现象一：文本上行没有转成 Codex turn

设备发送完整的一轮文本上行后（本次未选中活动会话）：

- 桥接器管理接口仍显示设备 `online = true`，`protocol = cbdp`；
- `queues.json` 没有新增条目；
- 没有出现新的 `turn.started` / `turn.completed` / `turn.failed` 事件；
- 设备端没有收到任何 `RESULT` 或 `ERROR`。

由于没有活动会话，桥接器不创建 turn 可能是正确的；但按协议要求，它至少
应该回一个结构化错误，而不是让设备停在“正在发送到电脑端”。

复核命令：

```powershell
Invoke-RestMethod http://127.0.0.1:8788/api/admin/devices
Get-Content "$env:LOCALAPPDATA\CodexBridge\queues.json" -Raw
```

## 现象二：音频上行约 10 KB 后失败

设备开始录音后，采集任务实际运行并发出约 9920 字节 PCM，随后
`wireless_bridge_send_frame()` 返回失败，设备侧显示：

```text
coding_bridge: capture bytes=9920 stack_free=1004
{"type":"capture_error","message":"音频发送失败，请检查桥接连接"}
```

同一时刻桥接器没有向设备回发错误，设备仍被桥接器标记为在线。约 10 KB
正好接近一次 TCP 写窗口被填满后的背压阈值，符合“桥接器会话线程暂时
停止读取、设备 `send()` 超时失败”的模式。

## 协议要求

`CBDP-V1-PROTOCOL.md` 要求：

1. `SEND_BEGIN` / `SEND_CHUNK` / `SEND_END` 是客户端到 Bridge 的文本回复
   事务，Bridge 在 `SEND_END` 后把完整文本交给上游。
2. 文本回复链路应为 `SEND_* -> turn.start / turn.steer`。
3. 设备会话线程不能因为上游 Codex 调用而停止读取该设备的音频分片；否则会
   出现录音截断、上行失败和连接假在线。

对应章节：消息类型表 `0x10..0x15`、第 18 节、第 19 节、第 26 节验收表
“文本回复 / 音频与语音回复”两行。

## 建议排查

1. 确认 CBDP 解码层是否把 `0x10/0x11/0x12` 正确转成内部文本事务。
2. 确认 `SEND_END` 后是否调用上游 `turn.start` / `turn.steer`。
3. 确认设备 socket 读取循环没有被同步的 Codex 调用阻塞；上行处理应放到
   每设备工作线程，保持读循环持续排空。
4. 确认音频 `AUDIO_*` 分片不被丢弃，并且桥接器按 `tx_id` 正确组装。
5. 给设备返回明确的 `RESULT` 或 `ERROR`，不要让一次失败的上行既无 turn
   也无错误反馈。

## 设备侧已完成

- CBDP v1 固定头、TLV、鉴权、能力协商、事件、SEND / AUDIO、审批和读回执
  已按文档实现。
- 修复了 7 KB 网络任务栈溢出导致握手后崩溃的问题。
- 录音期间挂起界面提示音，避免与采集任务并发重配 codec。
- 主机协议单测全部通过，完整固件构建和烧录通过。

## 需要桥接器侧确认的点

以上现象只能定位到“桥接器没有完成上行事务处理或读取背压失控”。由于运行
版是打包后的 `CodexBridge.exe`，其内部实现无法在本仓库内继续确认，需桥接器
侧按上面的排查项核对。
