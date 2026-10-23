# CodexBridge 侧请求：CBDP-only 设备无法在管理页完成“批准”

- 日期：2026-10-24
- 提出方：设备端（AI Passport 固件，仓库 `one2passport-publish`）
- 接收方：CodexBridge 开发者
- 本机实跑 Bridge 版本：`2.1.38`（`GET /api/admin/status`），设备端未改动 Bridge 代码
- 设备：AI Passport / ESP32-C3，device_id `aipassport-98c377f55270`

> **设备端结论（已改完并真机验证）：设备只走 CBDP，不再碰 JSON。**\
> 8764 只发 CBDP `DISCOVERY_REPLY`，8765 只做 CBDP 会话。\
> 下面这条是**设备侧无法自己解决**、必须 Bridge 侧决定的问题。

---

## 1. 现象

设备重新烧录（NVS 里没有 token）后，按用户描述的预期流程走：

1. 设备上电 → 发 CBDP 发现/入网广播；
2. Bridge 发现它，管理页出现该设备；
3. 用户在管理页点“批准”；
4. 设备拿到 token，和 Bridge 连上，Codex 可用。

**第 3 步现在必然失败。** 直接打本机管理接口：

```
POST /api/admin/devices/approve  {"device_id":"aipassport-98c377f55270"}
→ 409  {"ok":false,"error":"registration_nonce_missing"}
```

Bridge 界面上的“批准”按钮走的就是这个接口。

## 2. 根因：纯 CBDP 设备拿不到 registration nonce

设备的 CBDP 发现响应由 `codexbridge/devices/discovery.py` 的
`parse_cbdp_reply()` 解析。该函数返回的 `DiscoveryResult` **固定使用**
`registration_state="approved"`（dataclass 默认值），并且**从不携带
`registration_nonce`**。

只有旧的 JSON 行协议响应（`parse_reply()`）才解析
`registration_state` / `registration_nonce`：

```python
# discovery.py
registration_state = value.get("registration_state", "approved")
registration_nonce = value.get("registration_nonce")
if registration_state == "pending":
    ...要求 nonce 长度 16..64...
```

随后 `DeviceManager._sync_one()` 里，只有
`result.registration_state == "pending"` 时才会把 nonce 落到记录上：

```python
# manager.py
if result.registration_state == "pending" and not record.token:
    ...
    record.registration_nonce = result.registration_nonce
    record.registration_address = result.host
    record.registration_discovered_at = time.time()
```

而 `approve()` 又**强制要求** nonce：

```python
# manager.py approve()
if not record.registration_nonce:
    raise DeviceRegistrationError("registration_nonce_missing")
```

于是对于纯 CBDP 设备：设备会被发现，记录也会落成
`registration_state="pending"`（`registry.upsert()` 在 `token` 为空时默认 pending），
但 `registration_nonce` 是 `None`，`approve()` 直接抛错。
**管理页的批准按钮对 CBDP 设备是死路。**

用一个最小复现（不碰真机配置，临时 registry）：

```
[1] 纯 CBDP 发现之后:  state=pending  nonce=None  token=None
[2] POST /api/admin/devices/approve -> 409 registration_nonce_missing
[3] GET  /api/admin/pairing         -> 能铸出 token 并把状态置为 approved
```

## 3. 为什么 `register_offer` 帮不上忙

Bridge 里确实有一条 `register_offer` 通道
（`devices/session.py` 的 `send_register_offer()`，由 `manager.approve()` 调用），
Bridge README 也写着“ESP 硬件使用一次性 registration nonce 和 `register_offer`”。

但它的**触发条件**是 `approve()`，而 `approve()` 对 CBDP 设备永远抛
`registration_nonce_missing`（见上）。也就是说：

> 这条 path 对 CBDP-only 设备**不可达**。

设备端上一轮曾尝试在 8765 上按首字节区分、接收这条 JSON
`register_offer`。这是错的，已回退：

- 纯 CBDP 设备不会被标成 `pending`，这条连接**根本不会发生**；
- 更糟的是，为了区分协议而在 8765 上先做一次阻塞 `recv(MSG_PEEK)`，
  会把 CBDP 握手卡死。按协议 3 / 11.2（Device Listener Binding），
  **是设备先发 HELLO**，Bridge 在 HELLO 之前一个字节都不发；
  设备侧的阻塞 peek 会一直等不到第一个字节，HELLO 也就发不出去，
  Bridge 10s 后按 `hello_timeout` 断开。这条改动**已删除**，
  当前 `wireless_bridge.c` 的 accept 分支是无条件先发 HELLO。

## 4. 现在唯一可用的路（以及它为什么不能当产品流程）

```
GET /api/admin/pairing?device_id=aipassport-98c377f55270
→ {"ok":true,"device_id":"...","token":"<64字符>","port":8765}
```

这个接口会 `registry.mint_token()`：铸 token、置
`registration_state="approved"`。但 **token 只通过 HTTP 返回给调用方**，
设备侧拿不到 —— 必须由人把它写进设备 NVS（本设备目前只能通过 USB 串口
`wifi_setup` 命令，见 `firmware/main/coding_bridge.c` →
`wireless_bridge_configure()`）。这正是用户抱怨“不该有配对码 / 应该像
Dotii 那样点一下就通”的地方。

参考：Dotii 也不是靠这条，而是 Bridge 侧
`claim_codex_bridge_token()` 铸 token 后经 **BLE 配网**写进设备
（`D:\\dotii\\bridge\\codex_bridge.py` → `bluetooth_bridge.start_configure`）。
Dotii 固件**完全没有** `register_offer` / `registration_nonce`
（全仓 grep 为空）。本设备没有 BLE（设置页的蓝牙入口已删除），
所以照搬 Dotii 这条也不行。

## 5. 请 Bridge 侧二选一

### 方案 A（推荐，改动最小）：让 CBDP 设备也能进入 `pending` 并收到 token

1. `parse_cbdp_reply()` 允许 CBDP 发现响应带
   `registration_state` / `registration_nonce`（两个新 TLV），
   或者：Bridge 对**未知** CBDP 设备自行生成一个 nonce 并置 `pending`；
2. 这样 `manager.approve()` 就能走到现有的 `send_register_offer()`；
3. 但 token 的**投递**必须重新定义：`send_register_offer()` 现在发的是
   JSON 行协议。若要让 CBDP 设备也能收，请在协议里定义一条
   **CBDP 原生的注册/令牌下发消息**（例如复用 `0x1D DISCOVERY_REPLY`
   的注册语义，或新增一个一次性 `register` 消息 + TLV），
   而不是复用 JSON 通道。

   设备端可以按这个新定义实现；在定义落地前，设备不会在 8765 上
   解析任何 JSON。

### 方案 B（不动协议）：管理页为 CBDP 设备提供“铸 token”动作

把 `GET /api/admin/pairing` 已有的 `mint_token()` 暴露成管理页上一个
明确的“为该设备生成令牌”按钮，并在 UI 上直接展示 token，让用户
按设备自己的配网方式写入。最小改动、当天可做，但仍然是“手动搬运令牌”，
体验和用户期望的“点批准就连上”有差距。

## 6. 设备侧当前事实（供 Bridge 对齐）

| 项 | 现状 |
|---|---|
| 8764 | 只回 CBDP `DISCOVERY_REPLY`（`0x1D`），不响应旧的 `CODEXBRIDGE_DISCOVER` ASCII 探针 |
| 8765 | 只做 CBDP：accept 后**立即发 HELLO**，不再检查 token |
| JSON | 只在 **USB 串口**上解析（`wifi_setup` 等），网络上完全不解析 JSON |
| token 来源 | NVS `folo_bridge/token`，由 `wireless_bridge_configure()` 写入 |
| 无 token 时 | 仍然发 HELLO，AUTH 会失败并打日志；不再直接断链 |
| 真机验证 | `CBDP transport ready, token=loaded`；进 Codex `CBDP session ready`，能拉到真实项目列表 |

## 7. 附：设备端已修、无需 Bridge 配合的问题

进 Codex 报“请启动电脑端桥接”与 Bridge 无关，是设备内含内存竞态：
`radio_player_release_pool()` 只释放解码器池，不等收音机网络喂流任务退出，
紧接着申请桥接的 12KB 静态栈会拿到 `size=0` → `ESP_ERR_NO_MEM`。
设备端已改为先轮询等喂流任务真正退出（上限 2s）再起桥接。
真机复测：`largest=24576`，`CBDP session ready`，无 `NO_MEM`。

进小智"一直转、最后连接失败"同样是设备端问题，与 Bridge 无关：
Opus 编解码 arena 之前按上限 `XZ_OPUS_STATE_CAPACITY`(24576) 申请，
而小智前台只有 20480 的最大连续块，于是永远申请不到：
`E xiaozhi: opus arena alloc failed: need=24576 largest=20480`。
改成按 `opus_encoder_get_size(1)/opus_decoder_get_size(1)` 的真实需求
（向上对齐 16）申请后，真机 `opus shared arena ready: 24528 bytes`、
`WS connected`、`server hello ok`。同时把预热顺序改为"先 OTA/TLS、再预热
Opus"（OTA 走 HTTPS，TLS 握手峰值与 24KB arena 互斥）。整套流程（首页放
电台 → 进小智连上 → 退出回首页 → 电台恢复）真机跑通。
