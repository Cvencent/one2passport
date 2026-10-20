# CodexBridge 故障：cbdp-events.sqlite3 损坏导致 SUMMARY 订阅返回 INTERNAL（2026-10-20）

- 日期：2026-10-20
- 设备端：AI Passport（ESP32-C3，MAC `98:c3:77:f5:52:70`，IP `192.168.0.39`）
- Bridge：本机运行中的 `CodexBridge.exe` **v2.1.17**（commit `085953a`）
- 说明：本文档只记录设备端观测到的**对端行为**与只读取证，供 Bridge 侧开发者修复。
  **设备端仓库没有修改任何 Bridge 代码。**

## 现象

设备每次 AUTH_OK 后都会订阅 summary topic，主机随即回 ERROR：

```
I  wireless_bridge: CBDP session ready
I  coding_bridge: SUBSCRIBE topic=0x50000003 scope=0 cursor=0
W  coding_bridge: bridge error code=0x0015: INTERNAL
```

因为订阅失败，设备**收不到任何 `codex_state` 聚合计数**，首页"运行中/已完成/失败"
徽标为空；任务/项目流程本身不受影响（那些是 COMMAND/RESULT，不走订阅重放）。

## 根因（Bridge 侧）

`CbdpGateway._handle_subscribe()` 的重放路径会查事件库，而
**`%LOCALAPPDATA%\CodexBridge\cbdp-events.sqlite3` 已损坏**：

- 只读打开（`mode=ro`）执行订阅实际使用的那条查询即失败：

```sql
SELECT event_id, global_seq, topic_id, ... FROM events
WHERE topic_id = ? ORDER BY global_seq DESC LIMIT ?;
-- sqlite3.DatabaseError: database disk image is malformed
```

- 同一库的其他查询正常，说明是**索引/页结构损坏**而非整库不可读：

| 查询 | 结果 |
|---|---|
| `WHERE topic_id=? ORDER BY global_seq DESC`（订阅用） | **FAIL malformed** |
| `ORDER BY global_seq DESC`（不带 topic 过滤） | OK 32 行 |
| `WHERE global_seq > ?` | OK 32 行 |
| `WHERE topic_id=? AND topic_seq>0 ORDER BY topic_seq DESC` | OK 8 行 |

- `PRAGMA quick_check` 明确报错：

```
*** in database main ***
Tree 2 page 2 cell 5: Rowid 169 out of order
Tree 2 page 2 cell 4: Rowid 139 out of order
Tree 2 page 2 cell 2: Rowid 75 out of order
...
```

- 对应索引：`idx_events_topic_seq ON events(topic_id, topic_seq)`（`cbdp/events.py`）。
  损坏使按 `topic_id` 过滤的查询抛 `sqlite3.DatabaseError`；
  `_handle_subscribe()` 只捕获 `SubscriptionLimitError/ValueError`，
  其余异常被 `handle_frame()` 的兜底 `except Exception` 映射成 `INTERNAL`。

> 时间线上也吻合：本次订阅路径新增的"订阅时补发最新 codex_state"
> （commit `085953a` / v2.1.17）正是走 `events_latest(topic_id=...)` /
> `latest_event(...)` 这类按 topic 过滤的查询，所以更容易踩到坏索引。

## 为什么曾经能看到计数

设备刚上线时主机会在 attach 阶段**主动推送**一次聚合计数
（`hub.py` 的 `_send_codex_state_event()`，`session.send_event(event)`），
那条路径不查事件库，所以能到达设备（实测曾显示 `运行 6 完成 5 失败 0`）。
之后所有增量更新都走订阅 topic 下发，而订阅已失败 -> 计数再也不会刷新。

## 建议修复（Bridge 侧）

1. **修复/重建损坏的数据库**：启动时执行 `PRAGMA quick_check`；失败则对
   `events` 重建索引（`REINDEX`）或把库转存到新文件（`VACUUM INTO`）后替换。
   推荐在启动时自动做一次 `VACUUM INTO` + 原子替换，并保留旧库备份。
2. **对齐异常映射**：`_handle_subscribe()` 中的 `sqlite3`/存储异常应返回可诊断的
   错误码（如 `INTERNAL` 至少带 `reason=event_store_unavailable`），
   而不是无信息的 `INTERNAL`。
3. **订阅失败不应静默**：设备侧已加入有界重试（见下），但如果 Bridge 能
   在订阅失败后主动补发一次最新 `codex_state`，体验会更好。

## 设备端已做的配套（本次提交）

- 订阅失败不再"整场静默"：会话就绪但 **15 秒内未收到 `codex_state`** 时，
  低频重发 summary 订阅，最多 8 次；一旦收到计数立即永久停止。
  真机日志：
  ```
  W  coding_bridge: no codex_state yet; re-subscribing summary (attempt 1/8)
  I  coding_bridge: SUBSCRIBE topic=0x50000003 scope=0 cursor=0
  ```
- 首页计数改为宠物右侧的彩色数字徽标（进行中/已完成/失败），不再用一整行文字，
  避免被下方播放卡遮挡。

## 验收标准（Bridge 侧）

1. `PRAGMA quick_check` 返回 `ok`。
2. 设备 AUTH_OK 后订阅 summary topic 返回 RESULT（不再是 `INTERNAL`）。
3. 设备在数秒内收到带 `running` 的 `codex_state` 事件，首页徽标随任务增减刷新。
