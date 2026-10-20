# CodexBridge 待修复：CBDP 下 codex_state 计数未投递（设备端首页无“运行中”）

- 日期：2026-10-19
- 设备端：AI Passport（ESP32-C3，MAC `98:c3:77:f5:52:70`，IP `192.168.0.39`），CBDP v1 panel profile
- Bridge：本机运行中的 `CodexBridge.exe` v2.1.8
- 说明：本文档只记录设备端在真机联调中观测到的**对端行为**与证据，供 Bridge 侧开发者判断与修复。设备端仓库未修改 Bridge 代码。

## 摘要

设备端首页的“卡通形象 + 运行中/完成/失败数量”依赖 Bridge 下发全局聚合计数 `codex_state`。Bridge 在 CBDP 会话上**静默丢弃**了这条消息，因此设备端拿不到任何运行计数，首页宠物永远停在离线/空闲态。这是 Bridge 侧缺口，不是设备端解析问题。

## 现象

- 设备端已完成 CBDP 握手（HELLO/WELCOME/AUTH/AUTH_OK），`status` 能力已在 `enabled` 列表中。
- 设备端订阅了 summary 主题（`topic_id = 0x50000003`，`scope = 0`），并能正常收到 `message` / `read_state` 事件。
- 但设备端**从未收到任何**等价于 legacy `{"type":"codex_state","running":N,...}` 的 CBDP 事件；首页宠物与“运行中”计数始终为 0/离线。

## 根因（Bridge 侧）

1. `BridgeHub._send_codex_state()`（`codexbridge/hub.py`）构造的是 legacy dict：

```python
payload = {"type": "codex_state", **self.counts()}
# -> {"type": "codex_state", "running": ..., "completed_unread": ...,
#     "failed_unread": ..., "done": ..., "failed": ...}
self._send(device_id, payload)
```

2. `CbdpDeviceSession.send()`（`codexbridge/devices/session.py`）会把 legacy dict 交给 `compat.to_cbdp()` 翻译；而 `to_cbdp()`（`codexbridge/cbdp/compat.py`）**没有 `codex_state` 分支**，只能落到末尾的通用 notice 分支。

```python
for frame in to_cbdp(message, context):
    if frame.message_type == TYPE_DELTA:
        # CBDP DELTA frames are committed Event Spine events...
        continue
    self._send_frame(frame)
```

   结论：`codex_state` 在 CBDP 会话上既没有正确的翻译分支，构造出的 DELTA 也被 `send()` 丢弃 → 设备端收不到计数。

3. `BridgeHub._broadcast_codex_state()` 同样只走 `_send()`（legacy 路径），对 CBDP 会话无效。

## 影响

- 设备端首页“运行中数量 / 完成 / 失败”始终为 0，卡通形象停在 idle/offline，与电脑端实际是否有任务在跑无关。
- 设备端 `codex_background_state`（按 `message_id` 去重、按 `read_state` 消未读）本可直接消费聚合计数，但因收不到事件而无法工作。
- 这是“首页看不到 Codec 运行状态 / 运行中数量”的直接根因。

## 建议修复方向（供 Bridge 侧判断，不限定实现）

两种都可，任选其一并保持协议文档同步（`PROTOCOL.md` 属于设备通用协议文档）：

1. **首选**：把聚合计数作为 committed summary-scope 事件投递。在 `BridgeHub` 内新增一条在 Event Spine 上 append 的事件（summary 主题、state QoS），payload 携带 `running / completed_unread / failed_unread` 等字段，通过 `session.send_event(event)` / `gateway._emit(...)` 路径下发，而不是走 legacy `_send()`。需要设备端与 Bridge 就“哪个 event_type + 哪些 TLV 承载计数”达成一致，并写入 `PROTOCOL.md`（当前 CBDP 事件表里没有聚合计数事件）。
2. **次选**：为 `status.read` 的 RESULT 增补计数字段，设备端定时/按需拉取。但设备端当前只在 AUTH_OK 后订阅 summary 主题，不主动轮询；若走这条路，需要设备端与 Bridge 同时加“定时 status.read”逻辑。

> 无论选哪种，都必须让**已连接的 CBDP 会话**在任务进入/离开运行集合时收到更新（`BridgeHub.publish()` 已在 `record.kind in {"task","result"}` 时调用 `_broadcast_codex_state()`，把该调用改成向 CBDP 事件路径投递即可复用现成触发点）。

## 证据

- 设备端串口：CBDP 握手成功后 status 能力生效，但无任何 `codex_state` 等价事件落地。
- `%LOCALAPPDATA%\CodexBridge\devices.json` 中该设备 `"enabled"` 含 `"status"`；`"supported"` 亦含 `"status"`。
- 代码位置：`codexbridge/hub.py`（`_send_codex_state()` / `_broadcast_codex_state()` / `counts()`）、`codexbridge/devices/session.py`（`CbdpDeviceSession.send()` 丢弃 DELTA）、`codexbridge/cbdp/compat.py`（`to_cbdp()` 无 `codex_state` 分支）。

## 设备端已完成的配套工作（供对照）

- `codex_background_apply()` 修复：原来忽略 `status`，`working` 永远算不出运行中；现已按 status 词条正确维护 running。
- 新增 `codex_background_apply_notice()`：统一 state/task/status/result 四类通知的收敛入口，供收到 Bridge 计数/结果事件时调用。
- `turn.completed/failed` 事件现在无论是否在前台都会更新首页计数并触发完成提醒（按用户语音档位）。

即：**设备端已准备好消费该计数事件**，只等 Bridge 把 `codex_state` 真正投递到 CBDP 会话。

## 验收标准（Bridge 侧）

1. 制造一个正在运行的 Codex 任务；已连接的 CBDP 设备在数秒内收到携带 `running >= 1` 的聚合事件。
2. 任务结束后该设备收到 `running = 0`（且未读完成/失败计数按已读状态变化）。
3. 断开重连后，设备端能在 attach/handshake 后重新拿到一次当前计数快照。
4. `PROTOCOL.md` 中记录该事件的 event_type、scope、payload 字段与 QoS。
