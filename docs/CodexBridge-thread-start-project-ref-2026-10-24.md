# CodexBridge：`thread.start` 需要支持 `project_ref`

日期：2026-10-24  
状态：待 Bridge 开发者修改  
影响：AI Passport 设备端「选择项目 → 新建任务」无法把任务创建到所选项目目录，任务会落到 Bridge 默认工作目录。设备端旧的“收到用户消息就显示完成”问题已在固件里修复，但这一条必须由 Bridge 配合。

## 结论

按 CBDP-V1 协议，`thread.start` 的可选参数应包含 `project_ref`：

| 方法 | 可选参数 | 结果 |
|---|---|---|
| `thread.start` | `title`, `cwd`, `model_name`, `effort`, `project_ref` | `thread_ref`, `owner_state`, `revision` |

但当前 Bridge 实现没有把 `project_ref` 真正接到 `thread.start` 的创建逻辑里。设备目前为了兼容现网 Bridge，仍然只能发空参数；所以任务创建目录不受设备上选择的项目控制。

## 当前 Bridge 的卡点

1. `cbdp/gateway.py`
   - `METHOD_IDS` 已把 `0x0004` 映射成 `thread.start`。
   - `PARAM_TAGS` 已有 `project_ref = 0x0122`。
   - `_normalize_params()` 会把 `project_ref` 解析成 `project_id`。

2. `protocol/commands.py`
   - 当前定义是：
     ```python
     'thread.start': ('control', (), ('cwd', 'model'))
     ```
   - 它没有接受 `project_id`，所以 `_normalize_params()` 翻译出来的 `project_id` 会在参数校验时被当成非法参数拒绝。
   - 如果不传 `project_ref`，`service.start_thread()` 的 `cwd` 是空字符串，最终落到 Bridge 默认工作目录。

3. `companion/service.py`
   - `start_thread(cwd: str = "", model: str = "", ephemeral: bool = False)` 只认识 `cwd` 和 `model`，没有项目选择语义。
   - 设备端选中的是 `projects.list` 返回的 `project_ref`，不是完整 `cwd`。设备端没有可靠的方法自行把 `project_ref` 翻译成 `cwd`，这一步应该在 Bridge 内完成。

## 建议修复

在 Bridge 侧把 `project_ref` 翻译成 `cwd`，然后把 `cwd` 传给现有的 `service.start_thread()`。不要把 `project_id` 直接透传给 Codex app-server。

建议路径：

1. `gateway._normalize_params()`：
   - 只在 `method == "thread.start"` 时处理 `project_ref`。
   - 通过 `ReferenceRegistry` 把 `project_ref` 解析成项目 ID。
   - 从 Bridge 的项目状态里查这个项目对应的 `cwd`。
   - 把结果放到 `params["cwd"]`。
   - `project_ref` 消耗掉，不要把 `project_id` 传给命令层。

2. 失败语义：
   - `project_ref` 无法解析：返回 `NOT_FOUND`。
   - 项目存在但没有可用 `cwd`：返回 `INVALID_PARAM`。
   - 不要静默忽略 `project_ref`，否则设备会继续在错误目录建任务。

3. 保持现有行为：
   - `service.start_thread()` 仍然使用 `threadSource = "user"`。
   - `use_desktop_handoff` 与共享 app-server 两条创建路径都要使用同一个解析出的 `cwd`。
   - 已有对话、`turn.start`、队列、审批逻辑不要受影响。

## 验收用例

1. 设备选中项目 `D:\aipassport\one2passport-publish`。
2. 设备发起 `thread.start`，携带 `project_ref = <该项目的 ref>`。
3. Bridge 创建的新任务 `cwd` 必须是 `D:\aipassport\one2passport-publish`。
4. 电脑端 Codex Desktop 能在该项目下看到新任务。
5. 换选另一个项目后新建任务，新任务 `cwd` 也跟着变。
6. 发送不存在的 `project_ref`，Bridge 返回 `ERROR(NOT_FOUND, "project_ref")`，不能创建默认目录任务。
7. 不带 `project_ref` 的旧客户端仍然按原逻辑工作。

## 设备端现状

设备端已经定位并修复了另一个 UI 误判：

- CBDP `MESSAGE_APPENDED / MESSAGE_UPDATED` 是消息事件，不是任务终态。
- 设备端旧逻辑把任何消息都调用成 `app_coding_remote_result(..., "completed", ...)`。
- 现在已改为只在 `kind = commentary/task(2)` 且 `status = working(1)` 时更新运行状态。
- 真正完成/失败仍然由 `TURN_COMPLETED / TURN_FAILED / TURN_INTERRUPTED` 驱动。

因此，这条 Bridge 修复完成后，设备端「新任务」才会同时满足两个条件：

- 不再提前显示“已完成”。
- 任务能创建到用户选择的 Codex 项目目录。
