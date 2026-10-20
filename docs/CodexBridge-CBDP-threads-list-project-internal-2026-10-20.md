# CodexBridge：CBDP `threads.list` 按项目过滤返回 INTERNAL（2026-10-20，**已修复**）

> **状态：已解决（2026-10-20）** —— Bridge 已升级到 v2.1.17 并修复此问题：
> `cbdp/gateway.py` 增加 `"project_ref": 0x0122` 映射并在 `_normalize_params()`
> 中解析成 `project_id`，`protocol/commands.py` 的 `threads.list` 可选参数加入
> `project_id`。设备端无需改动，真机已验证项目列表 → 任务列表 → 新建任务 →
> 回复任务全部可用（详见 `firmware-verification-2026-10-20.md` 第五轮验证）。
> 以下保留原始问题记录，仅供回溯。

- 日期：2026-10-20
- 设备端：AI Passport（ESP32-C3，MAC `98:c3:77:f5:52:70`，IP `192.168.0.39`），CBDP **text** profile
- Bridge：本机运行中的 `CodexBridge.exe` **v2.1.16**
- 说明：本文档只记录设备端在真机联调中观测到的**对端行为**与证据，供 Bridge 侧开发者判断与修复。**设备端仓库没有修改任何 Bridge 代码。**

## 摘要

在 Codex 页面里"选择一个项目 → 查看该项目下的任务"这一步，设备端发送
`threads.list`（携带 `project_ref = 0x0122`），Bridge 回
`ERROR code=0x0015 (INTERNAL)`，任务列表永远拿不到。设备端没有可用的替代路径
（当前协议里 `threads.list` 就是按项目取任务列表的唯一方法）。

另外，聚合计数 `codex_state` 仍未投递到 CBDP 会话（见
`CodexBridge-CBDP-status-push-issue-2026-10-19.md`），首页"运行中/完成/失败"数量仍恒为 0。

## 现象

真机串口（设备端已按项目正确取到 `projects.list` 的 8 个项目）：

```
I  coding_bridge: SUBSCRIBE topic=0x50000003 scope=0 cursor=0
W  coding_bridge: bridge error code=0x0015: INTERNAL   <- 对 threads.list 的回复
```

设备端因此从"正在读取这个项目的任务…"切到错误页，显示 `INTERNAL`。

## 根因（Bridge 侧，静态代码分析）

1. 设备端把项目选择编码进 `threads.list` 的参数：

   ```
   FW  CBDP_TLV_PROJECT_REF = 0x0122
   ```

   这是协议表里 project 对象携带的 `project_ref` (见
   `cbdp/gateway.py: `TAG_PROJECT_REF: u32(self._ref("project", ...))`)，
   设备端按同一 tag 回传，语义一致。

2. 但 `gateway.PARAM_TAGS`（`cbdp/gateway.py:352`）**没有 `project_ref` 映射**，
   因此 `0x0122` 不会被翻译成上游命令层的参数名。`PARAM_TAGS` 里只有
   `thread_id / thread_ref / turn_id / message_id / ... `，没有 project 相关项。

3. `protocol/commands.py` 中：

   ```python
   'threads.list': ('tasks', (), ('limit', 'search', 'cursor')),
   ...
   if method == 'threads.list':
       return service.list_threads(**params)
   ```

   `service.list_threads()`（`companion/service.py:1980`）只接受
   `limit / search / cursor / refresh`，**不支持按项目过滤**。
   因此即便 `project_ref` 被翻译成参数，也会以未知参数抛出异常，
   最终被 `gateway._handle_command` 的兜底 `except Exception` 映射为
   `INTERNAL`（`gateway.py:1145-1146`）。

## 影响

- 设备端 Codex 页面**无法进入任何项目的任务列表**，因此也无法"新建任务"、
  "选择已有任务继续"或"回复任务"。用户看到的是一条永久错误。
- 这是"Codecs 里面新建任务建不了/卡住"的直接根因（前半段 `projects.list` 已可用，
  卡点在 `threads.list`）。

## 建议修复方向（供 Bridge 侧判断，不限定实现）

1. **首选**：让 `threads.list` 支持按项目过滤。
   - 在 `gateway.PARAM_TAGS` 增加 `"project_ref": 0x0122`；
   - 在 `_normalize_params()` 里把 `project_ref` 解析回字符串 `project_id`
     （与 `thread_ref` 的处理一致：查 `references.external_id("project", ref)`，
     解析失败应回 `NOT_FOUND` 而不是 `INTERNAL`）；
   - 在 `commands.py` 的 `METHODS` 表里把 `project_id` 加进 `threads.list`
     的可选参数，并在 `service.list_threads()` 里按项目过滤（或明确回
     `UNSUPPORTED_FEATURE`，让设备端能区分"不支持"与"内部错误"）。

2. **次选**：如果上游无法按项目过滤，请明确返回
   `ERROR UNSUPPORTED_FEATURE`（而不是 `INTERNAL`），并给出可用的替代方法
   （例如 `thread.search` 携带项目范围），由设备端改走该路径。

> 无论选哪种，请在 `PROTOCOL.md` 里记录 `threads.list` 是否接受
> `project_ref(0x0122)`，避免两端再次各自理解。

## 证据

- 设备端串口：`coding_bridge: bridge error code=0x0015: INTERNAL`，紧跟在
  `threads.list` 命令之后；同时 `projects.list` 已成功返回 8 个项目
  （截图显示 `选择项目 1-8/8`，含 aiNovelWrite / one2passport / CodexBridge 等）。
- 设备端 tag 定义：`firmware/main/codex_bridge_protocol.h`
  （`CBDP_TLV_PROJECT_REF = 0x0122`）。
- Bridge 代码位置：`codexbridge/cbdp/gateway.py`（`PARAM_TAGS`:352、
  `_normalize_params`:1173、`_handle_command` 兜底:1145）、
  `codexbridge/protocol/commands.py`（METHODS:16、dispatch:170）、
  `codexbridge/companion/service.py`（`list_threads`:1980）。

## 设备端已完成的配套工作（供对照）

- `projects.list` 的结果现在能在前台正确落地；进入 Codex 后会自动重新请求一次，
  避免回复在前台标志置位前到达而被丢弃（这是设备端自身的 bug，已修）。
- 目录类请求现在会跟踪 `request_id`：Bridge 返回 `ERROR` 或 20 秒无响应时，
  设备端**不再永久停在加载中**，而是显示明确错误并允许返回重试。
- 因此 `threads.list` 一旦在 Bridge 侧修好，设备端无需再改即可正常列任务、建任务、
  继续任务与回复任务。
