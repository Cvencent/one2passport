# 小智 AI 语音接入：问题定位与当前状态

日期：2026-10-07 ~ 2026-10-08
设备：ESP32-C3（400KB SRAM，无 PSRAM）
结论：**WebSocket 连接链路已打通并稳定；语音上行受硬件内存限制未完成。**

---

## 一、已解决（均经实测验证）

### 1. TLS 证书验证失败
- 现象：`esp-x509-crt-bundle: No matching trusted root certificate found`
- 原因：`api.tenclass.net` 用 **GeoTrust G2 TLS CN RSA4096 SHA256 2022 CA1** 签发，
  根为 **DigiCert Global Root G2**。当时 sdkconfig 用的是 CMN 精简 CA 库。
- 解法：确认 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y` + `cfg.crt_bundle_attach = esp_crt_bundle_attach`
  （CMN 库实际已含 DigiCert Global Root G2）。
- ⚠️ 教训：中途误加 `CONFIG_ESP_TLS_INSECURE` + `SKIP_SERVER_CERT_VERIFY` 会**破坏** mbedtls 的
  in_buf 分配，导致 `requesting more data than fits`。不要开这两个选项。

### 2. taskLVGL 看门狗（UI 卡死）
- 原因：ws 任务优先级设为 9，高于 LVGL 任务，绘制中文时被饿死。
- 解法：`cfg.task_prio = 5`。

### 3. WS 连接握手失败（认证头未发出）
- 原因：`esp_websocket_client_set_headers()` 在 `start()` 之前调用无效
  （该 API 要求 state==CONNECTED，否则返回 INVALID_ARG）。
- 解法：改用 **config 的 `cfg.headers` 字段**传入 Device-Id / Client-Id / Authorization / Protocol-Version。

### 4. LVGL 独占内存池导致堆碎片
- 现象：`largest free block` 仅 7.6KB，mbedtls/TLS 分配失败。
- 解法：`CONFIG_LV_USE_CLIB_MALLOC=y`（改用系统 malloc），
  largest 从 32KB 提升到 **45~47KB**。

### 5. opus 编码器无法初始化（部分改善）
- 原因：需要 **24864 B 连续内存**，进小智后 largest 不足。
- 改善：靠上面的 clib malloc 改造，opus 预热**能够成功**（曾实测 `opus encoder pre-warmed`）。

### 6. 两个代码 bug
- `xz_capture_run(int16_t *pcm)` 内 `sizeof(pcm)` 取的是**指针大小(4)**，应显式用 `XZ_CHUNK_SAMPLES*2`。
- 常驻任务内误用 `vTaskDelete(NULL)` 自杀，应改为 `return`。

---

## 二、未解决：opus 与 TLS 无法共存（硬件边界）

### 实测数据

| 项目 | 需求 |
|---|---|
| opus 编码器 | **24864 B 连续** |
| TLS 握手 | 需 largest **>20KB**（无 opus 时 27648 可成功；opus 预热后 11~13KB 即失败） |
| ws 任务栈 | **8192** 才稳定（3072/4096 崩溃；5120 时 ClientHello 发出但收不到响应） |
| mic 任务栈 | >7KB（opus 编码调用栈很深） |

进小智后可用连续内存上限约 **27KB**，而 opus + TLS 需 **>45KB**。

### 已尝试但不奏效的方案
- 调整 LVGL 堆大小（24/26/28/32KB）
- 缩小 WiFi RX 缓冲、TCP 窗口、lwip 栈
- 把 opus 预热放在不同时机（连接前/后/录音时）
- mic 任务改静态栈（.bss，反而减少总堆量）
- 降低 opus 帧长（10ms/20ms）——内存几乎不变（大头是 libopus 状态）

---

## 三、后续可行方向（未实施）

1. **换 ESP32-S3 + PSRAM**：把 LVGL / opus 放 PSRAM，彻底解决。这是最干净的方案。
2. **分时复用**：录音时 `lv_deinit()` 释放 LVGL，录完 `lv_init()` 重建 UI（改动大、易出问题）。
3. **核验服务器能力**：已确认服务器接受 `format=pcm` / `g711` 的 hello（但不改回显），
   需进一步确认上行是否真有非 opus 解码路径。
4. **简化 UI**：若小智页可去掉大字体/图形，或能把 LVGL 堆降到 8KB 以下。

---

## 四、当前代码状态（稳定可用）

- 小智 AI WebSocket 连接：**稳定成功**，可拿到 session_id
- mic 常驻任务：静态栈创建成功
- 无崩溃、无看门狗
- sdkconfig 关键项：`LV_USE_CLIB_MALLOC=y`、`SSL_IN_CONTENT_LEN=4096`、`SSL_OUT_CONTENT_LEN=1024`、
  `VARIABLE_BUFFER_LENGTH=y`、CMN CA 库
- 诊断开关 `RELAY_MODE=0` / `PLAIN_WS_TEST=0`（均关闭）
