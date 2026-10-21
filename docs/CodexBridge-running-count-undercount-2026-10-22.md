# CodexBridge 侧问题：`counts().running` 少算并发任务（实测 5 个在跑只报 1）

- 日期：2026-10-22
- 提出方：设备端（AI Passport 固件）
- 接收方：CodexBridge 开发者
- Bridge 版本：`2.1.26`（用户本机实跑，设备端**未改动** Bridge）
- 设备固件：本仓库 `main`（本次设备端修复见文末 §5）

> 一句话：**设备只是把 Bridge 推来的 `running` 原样显示**。
> 现在 Codex 里同时有 5 个 turn 处于 `inProgress`，Bridge 只报 `running=1`，
> 所以首页角标显示 1。这个数字要在 Bridge 侧修。

---

## 1. 现象

首页（AI Passport，设备 `aipassport-98c377f55270`）机器人旁边的"运行中"角标
长期显示 **1**，而实际上同时有 **5 个会话在跑**。

设备串口每 ~2s 打印一条（Bridge 的 UDP 0x1E 首页计数帧，`coding_bridge.c:
apply_codex_counts()`）：

```
I (8819) coding_bridge: codex_state: running=1 done=0 failed=0
I (10721) coding_bridge: codex_state: running=1 done=0 failed=0
I (12740) coding_bridge: codex_state: running=1 done=0 failed=0
...
I (64841) coding_bridge: codex_state: running=1 done=0 failed=0
```

70 秒内 25 帧，**全部是 `running=1`**，没有任何一帧变成 2 或更多。

## 2. 同一时刻 Codex 的真实状态

用 Codex 的 `wait_threads`（timeoutMs=0）取快照，同一时间段内
**5 个线程的 `latestTurn.status` 都是 `inProgress`**：

| threadId | latestTurn.status | startedAt |
|---|---|---|
| `01a1548d-…`（本会话） | inProgress | — |
| `01a156c0-…` | **inProgress** | 1792605399 |
| `01a14a66-…` | **inProgress** | 1792605384 |
| `01a15ef7-…` | **inProgress** | 1792594662 |
| `01a15341-…` | **inProgress** | 1792593141 |

（同一时刻 `/api/admin/devices` 显示该设备 `online:true`，UDP 通道正常。）

→ **真实 running = 5，Bridge 报 1。**

## 3. Bridge 侧代码路径（只读分析，未改动）

```
hub.counts()                        codexbridge/hub.py:367
  └─ for record in store.records():
       if record.kind == "task"
          and record.status in IN_FLIGHT_STATUSES
          and is_in_flight(record) and is_fresh(record, now):
             running += 1

collector.records_from_snapshot()   codexbridge/codex/collector.py:50
  └─ tasks = snapshot.get("tasks")
     → 每个 task 生成一条 kind="task" 记录

AppServer.snapshot()                codexbridge/codex/app_server.py:1186
  └─ "tasks": self.tasks

AppServer.refresh_thread()          codexbridge/codex/app_server.py:1088
  └─ if snapshot["status"] in {"working", "waiting_user"}:
         active.append(snapshot)
     ...
     self.tasks = active          # :1107
```

### 三个可疑点（按怀疑程度排序）

**(a) `active` 只收 `working` / `waiting_user`，漏掉 `queued` / `starting`**

`app_server.py:1088` 只把这两种状态放进 `active`，但
`messages_store.py:45` 的 `IN_FLIGHT_STATUSES` 是
`{"working","waiting_user","queued","starting"}`。
两边口径不一致：一个 turn 处于 `queued` / `starting` 时，Bridge 自己认为它是
"在飞"，但 `self.tasks` 里没有它 → `counts().running` 少数。

**(b) `counts()` 只数 `kind == "task"`，但 `IN_FLIGHT_KINDS` 同时包含 `status`**

`messages_store.py:44`：`IN_FLIGHT_KINDS = {"status", "task"}`
`hub.py:383`：`if record.kind == "task"`

如果某条 live 线程只产出了 `status` 记录（collector 对 primary 线程会**同时**发
`status` 和 `task` 两条，但"其它运行中线程"分支的过滤条件不同），它就不会被计入
`running`。

**(c) `candidates` 的入选条件可能漏掉线程**

`app_server.py:1029-1037`：

```python
if (self._is_active_hint(selected) or thread_id in self.active_thread_ids or
        liveness.get(thread_id) == "working" or
        len(candidates) < self.MAX_DISPLAY_TASKS):
```

`MAX_DISPLAY_TASKS = 6`（:913），所以 5 个线程理论上都进得去。
但 `liveness` 依赖 `self.activity.activity(listed)` 读 rollout 日志，
而 `thread_snapshot()` 对"第二个 app-server 看不到的 live turn"会误报
`interrupted`（代码注释 :1076-1082 自己写了这个坑）。
如果某线程既没有 active hint、`liveness` 也没判出 `working`、
且排在 `MAX_DISPLAY_TASKS` 之外，它就不会进 `active`。

## 4. 建议的验收口径

`counts()["running"]` 应等于：

> **所有 `latestTurn.status == inProgress` 的会话数**
> （含 `queued` / `starting` 这类"已排队但还没真正开跑"的，按产品口径都算"运行中"）

最直接的做法：在 `refresh_thread()` 里，把 `active` 的入选条件从
`{"working","waiting_user"}` 扩到 `IN_FLIGHT_STATUSES` 全集，并确认
`counts()` 对 `kind in {"task","status"}` 都能计数（同一 thread 去重，别双计）。

### 验收步骤

1. 在 Codex 里同时开 3 个会话并各发一条会跑 >10s 的任务；
2. `GET /api/admin/devices` 确认设备在线；
3. 设备串口应打印 `codex_state: running=3`；
4. 首页机器人右侧角标应显示 **3**，且机器人处于"运行中"表情；
5. 三个任务全部结束后，下一帧应为 `running=0`，角标消失。

## 5. 设备端本次已修的（不需要 Bridge 配合）

同一轮里设备端修了 3 个自己的问题（提交在设备仓库，**未触碰 Bridge**）：

1. **状态优先级**：原来 `failed` 判在 `running` 之前 —— 只要历史上还剩一个未读失败，
   哪怕此刻正在跑任务，首页机器人也一直显示"失败"。现改为
   `运行中 > 已完成 > 失败 > 空闲`。
2. **一条结果不再清零全局 running**：`codex_background_apply_result()` 里原来无条件
   `state->running = 0U;`，多任务并发时一条终态结果会把"运行中"清零。
3. **已完成/失败计数不再只增不减**：`apply_codex_counts()` 原来写的是
   `if (done > state.done) state.done = done;`，导致角标一旦亮起永不消失；
   现在直接跟随 Bridge 的权威值（这样"在手机上读掉一条"也能让设备角标降下来）。

> 注：第 3 条修好之后，**Bridge 把 `completed_unread` / `failed_unread` 降下来时设备才会跟着降**。
> 如果 Bridge 侧这两个值本身不清零，设备仍会显示旧数字 —— 届时请按 §4 一并核对。
