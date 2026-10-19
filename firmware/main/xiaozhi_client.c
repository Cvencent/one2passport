#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"

#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include <time.h>
#include "esp_websocket_client.h"
#include "esp_mac.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "xiaozhi_connect_guard.h"
#include "xiaozhi_client.h"

#include "bsp_audio.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"   // IDF_VER(OTA 请求体的 application.idf_version)

static const char *TAG = "xiaozhi";


// ---- heap 诊断(临时) ----
#include "esp_heap_caps.h"
void xz_heap_report(const char *tag) {
    size_t all = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ESP_LOGW("heapdiag", "[%s] free_all=%u internal=%u dma=%u largest=%u",
             tag, (unsigned)all, (unsigned)internal, (unsigned)dma, (unsigned)largest);
}

#define XZ_WS_URL_DEFAULT "wss://api.tenclass.net/xiaozhi/v1/"
// ---- 诊断开关(默认全 0; 排查时按需置 1) ----
#define RELAY_MODE 0               // 1=经电脑中继 TCP1443 转发 443
#define PLAIN_WS_TEST 0            // 1=本机假服务器联调; 发布固件必须为 0
#define RELAY_MODE_OTA_BIGTEST 0   // 1=OTA 走电脑本地 8080 大响应

#if RELAY_MODE_OTA_BIGTEST
#define XZ_OTA_URL      "http://192.168.0.35:8080/xiaozhi/ota/"   // 大响应接收实验
#else
// ⚠ 必须用**正式服** xiaozhi.me, 不能用公共测试服 api.tenclass.net:
//   测试服对任何 MAC 都直接返回 websocket 配置, **永不返回 activation**,
//   因此设备码永远出不来(用户"打开什么都没有"的根因)。
//   正式服 https://xiaozhi.me/api/ota/ 在设备未绑定时返回:
//     {"activation":{"code":"961572","message":"xiaozhi.me
//                    "challenge":"<uuid>"}}
//   用户拿 code 去 xiaozhi.me 绑定后, 后续请求即返回 websocket 配置。
//   用 https(会 301 到 https, disable_auto_redirect=false 自动跟随); OTA 发生
//   在 opus 预热之前, largest≈13KB, TLS 握手可过。
#define XZ_OTA_URL      "https://xiaozhi.me/api/ota/"
#endif
// ---- runtime state ----
static esp_websocket_client_handle_t s_ws;
static volatile bool s_ws_connected;  // TCP/WS 层已连接(未握手)
static volatile bool s_ws_open;
static volatile bool s_listening;
static char s_session_id[64];
static char s_client_id[40];    // UUIDv4, NVS 持久化(参考固件 xiaozhi_store 做法)
static int  s_proto_version = 3; // OTA 返回的协议版本
// 重组后的单个 WS 帧最大字节数。60ms@16k Opus 远小于此, 留足余量。
// 放在 .bss: 必须在 WS 回调里做分片拼装, 但绝不能在栈上开这么大缓冲。
#define XZ_WS_FRAME_MAX 512
static char s_url[256];
static char s_token[192];
static char s_stt_text[256];     // last recognized text
static char s_tts_text[256];     // last tts sentence
// 设备激活码: OTA 返回 activation.code 时填入。用户在 https://xiaozhi.me
// 输入此码完成设备绑定(官方固件同款流程)。
static char s_activation_code[32];
static char s_activation_msg[192];
static bool s_activated;         // 已激活(OTA 未返回 activation 段)
static bool s_config_cached;     // OTA 成功过: 已缓存 ws url/token, 重连时可跳过 TLS OTA
static int  s_cached_ver = 3;   // 缓存的协议版本
static QueueHandle_t s_event_queue;   // ui events: enum xz_ui_event + text ptr

// The UI task can request exit while the blocking connect task is inside OTA
// or WebSocket setup. Keep the ownership decision in one tiny state machine:
// only the task that owns the connection is allowed to close it.
static xz_connect_guard_t s_connect_guard;
static portMUX_TYPE s_connect_guard_mux = portMUX_INITIALIZER_UNLOCKED;

static bool xz_cancel_requested(void)
{
    portENTER_CRITICAL(&s_connect_guard_mux);
    const bool requested = xz_connect_guard_is_cancelled(&s_connect_guard);
    portEXIT_CRITICAL(&s_connect_guard_mux);
    return requested;
}

static bool xz_connect_begin(void)
{
    portENTER_CRITICAL(&s_connect_guard_mux);
    const bool begun = xz_connect_guard_begin(&s_connect_guard);
    portEXIT_CRITICAL(&s_connect_guard_mux);
    return begun;
}

static bool xz_connect_finish(void)
{
    portENTER_CRITICAL(&s_connect_guard_mux);
    const bool cancelled = xz_connect_guard_finish(&s_connect_guard);
    portEXIT_CRITICAL(&s_connect_guard_mux);
    return cancelled;
}


bool xiaozhi_client_reset_cancel(void)
{
    portENTER_CRITICAL(&s_connect_guard_mux);
    const bool idle = !xz_connect_guard_is_active(&s_connect_guard);
    if (idle) xz_connect_guard_reset(&s_connect_guard);
    portEXIT_CRITICAL(&s_connect_guard_mux);
    return idle;
}

bool xiaozhi_client_cancel(void)
{
    portENTER_CRITICAL(&s_connect_guard_mux);
    const bool close_now = xz_connect_guard_request_cancel(&s_connect_guard);
    portEXIT_CRITICAL(&s_connect_guard_mux);

    // If a connect task owns the handle, it must perform the close itself.
    // Closing from here while OTA/WebSocket is using the handle is a UAF race.
    if (close_now) xiaozhi_client_close();
    return close_now;
}

bool xiaozhi_client_is_cancelled(void)
{
    return xz_cancel_requested();
}

bool xiaozhi_client_is_open(void) { return s_ws_open; }
const char *xiaozhi_client_stt(void) { return s_stt_text; }
const char *xiaozhi_client_tts(void) { return s_tts_text; }
const char *xiaozhi_client_activation_code(void) { return s_activation_code; }
const char *xiaozhi_client_activation_message(void) { return s_activation_msg; }

// ---- Client-Id: UUIDv4, NVS 持久化 ----
static void load_or_create_client_id(void)
{
    if (s_client_id[0]) return;
    nvs_handle_t h;
    if (nvs_open("xiaozhi", NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof(s_client_id);
        if (nvs_get_str(h, "client_id", s_client_id, &len) != ESP_OK || !s_client_id[0]) {
            uint8_t r[16];
            esp_fill_random(r, sizeof(r));
            r[6] = (r[6] & 0x0F) | 0x40;   // v4
            r[8] = (r[8] & 0x3F) | 0x80;
            snprintf(s_client_id, sizeof(s_client_id),
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7],
                     r[8],r[9],r[10],r[11],r[12],r[13],r[14],r[15]);
            nvs_set_str(h, "client_id", s_client_id);
            nvs_commit(h);
            ESP_LOGI(TAG, "Generated new Client-Id UUID: %s", s_client_id);
        }
        nvs_close(h);
    }
}

// ---- cached OTA result (NVS) ----
// The OTA endpoint hands back a WebSocket URL and a bearer token. That pair is
// stable across reboots until the binding/token is revoked, so paying a full
// HTTPS/TLS round trip on every re-entry made "enter XiaoZhi" take ~5s longer
// than necessary. Cache it, try WS first, and fall back to OTA on any failure.
static void xz_cache_load(void)
{
    if (s_config_cached && s_url[0]) return;
    s_url[0] = 0;
    s_token[0] = 0;
    nvs_handle_t h;
    if (nvs_open("xiaozhi", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_url);
    if (nvs_get_str(h, "ws_url", s_url, &len) != ESP_OK || !s_url[0]) {
        s_url[0] = 0;
        nvs_close(h);
        return;
    }
    len = sizeof(s_token);
    if (nvs_get_str(h, "ws_token", s_token, &len) != ESP_OK) s_token[0] = 0;
    uint8_t proto = 3;
    if (nvs_get_u8(h, "proto_ver", &proto) == ESP_OK && proto > 0) {
        s_proto_version = proto;
    }
    nvs_close(h);
    s_config_cached = true;
    ESP_LOGI(TAG, "cached OTA config: url=%.60s token=%d chars proto=%d",
             s_url, (int)strlen(s_token), s_proto_version);
}

static void xz_cache_save(void)
{
    if (!s_url[0]) return;
    nvs_handle_t h;
    if (nvs_open("xiaozhi", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = nvs_set_str(h, "ws_url", s_url);
    if (e == ESP_OK) e = nvs_set_str(h, "ws_token", s_token);
    if (e == ESP_OK) e = nvs_set_u8(h, "proto_ver", (uint8_t)s_proto_version);
    if (e == ESP_OK) e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "cached OTA save failed: %s", esp_err_to_name(e));
    nvs_close(h);
    s_config_cached = true;
}

static void xz_cache_clear(void)
{
    s_config_cached = false;
    s_url[0] = 0;
    s_token[0] = 0;
    nvs_handle_t h;
    if (nvs_open("xiaozhi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ws_url");
    nvs_erase_key(h, "ws_token");
    nvs_erase_key(h, "proto_ver");
    nvs_commit(h);
    nvs_close(h);
}

// ---- OTA: fetch websocket url+token ----
static bool ota_fetch_config(void)
{
    char mac[18] = {0};
    uint8_t mac_bin[6] = {0};
    esp_read_mac(mac_bin, ESP_MAC_WIFI_STA);
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac_bin[0], mac_bin[1], mac_bin[2], mac_bin[3], mac_bin[4], mac_bin[5]);

    load_or_create_client_id();
    // 使用官方 xiaozhi OTA 激活协议的 application + board 请求结构。
    // 服务端按这个结构生成 activation.code；旧的扁平结构会被降级成
    // 公共 test-token，设备绑定后再次请求仍会回到“待激活/公共配置”。
    char post[640];
    snprintf(post, sizeof(post),
             "{\"application\":{\"version\":\"1.0.0\",\"elf_sha256\":\"\"},"
             "\"board\":{\"type\":\"esp32c3\",\"name\":\"passport-os\","
             "\"ip\":\"\",\"mac\":\"%s\"}}",
             mac);

    esp_http_client_config_t cfg = {};
    cfg.url = XZ_OTA_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.user_agent = "passport-os/1.0.0";
    cfg.keep_alive_enable = false;
    // ⚠ HTTPS 必须挂证书包, 否则 esp-tls 报
    //   "No server verification option set in esp_tls_cfg_t structure"
    //   (早期 OTA 走 http 明文, 不需要; 改 https://xiaozhi.me 后必需)。
    //   xiaozhi.me 由 DigiCert RapidSSL 签发, CMN CA 库含其根。
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // http 会 301 到 https, 自动跟随
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 4;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    if (xz_cancel_requested()) {
        esp_http_client_cleanup(c);
        return false;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_http_client_set_header(c, "Accept-Language", "zh-CN");
    esp_http_client_set_header(c, "Activation-Version", "1");
    esp_http_client_set_header(c, "Accept-Encoding", "identity");
    esp_http_client_set_header(c, "Device-Id", mac);
    esp_http_client_set_header(c, "Client-Id", s_client_id);
    esp_err_t open_err = esp_http_client_open(c, strlen(post));
    if (open_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA http open failed: %s", esp_err_to_name(open_err));
        esp_http_client_cleanup(c);
        return false;
    }
    if (xz_cancel_requested()) {
        esp_http_client_cleanup(c);
        return false;
    }
    esp_http_client_write(c, post, strlen(post));
    esp_http_client_fetch_headers(c);
    const int status = esp_http_client_get_status_code(c);
    ESP_LOGI(TAG, "OTA http status=%d", status);
    static char buf[4096];
    memset(buf, 0, sizeof(buf));
    int read_total = 0;
    while (read_total < (int)sizeof(buf) - 1) {
        if (xz_cancel_requested()) break;
        int n = esp_http_client_read(c, buf + read_total, sizeof(buf) - 1 - read_total);
        if (n <= 0) break;
        read_total += n;
    }
    buf[read_total] = 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (xz_cancel_requested()) return false;
    ESP_LOGW(TAG, "OTA_BIGTEST: body %d bytes", read_total);
    if (read_total <= 0) { ESP_LOGE(TAG, "OTA no response"); return false; }

    cJSON *root = cJSON_Parse(buf);
    if (!root) { ESP_LOGE(TAG, "OTA bad json"); return false; }
    bool ok = false;
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    int proto_ver = cJSON_IsNumber(ver) ? ver->valueint : 3;   // 参考固件: version 动态取, 默认按 v3

    // ---- 服务端错误分支 ----
    // 非 2xx / {"success":false,"error":"...","message":"..."} 都要显式暴露。
    // 早期版本忽略状态码与 success 字段 -> 服务端拒绝时界面无任何提示, 用户
    // 只看到"什么都没有"。典型如 Client-Id 非合法 UUIDv4 返回 400。
    cJSON *succ = cJSON_GetObjectItem(root, "success");
    const bool server_rejected =
        (status < 200 || status >= 300) ||
        (cJSON_IsBool(succ) && !cJSON_IsTrue(succ));
    if (server_rejected) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        cJSON *emsg = cJSON_GetObjectItem(root, "message");
        snprintf(s_activation_msg, sizeof(s_activation_msg), "%s%s%s",
                 cJSON_IsString(err) ? err->valuestring : "",
                 (cJSON_IsString(err) && cJSON_IsString(emsg)) ? " / " : "",
                 cJSON_IsString(emsg) ? emsg->valuestring : "");
        s_activation_code[0] = 0;
        ESP_LOGE(TAG, "OTA rejected: HTTP %d body=%.200s", status, buf);
        if (s_event_queue) {
            xz_ui_event_t ev = XZ_EV_ACTIVATION;
            xQueueSend(s_event_queue, &ev, 0);
        }
        cJSON_Delete(root);
        return false;
    }

    // ---- 激活分支(官方固件同款) ----
    // 未激活时 OTA 返回 {"activation":{"code":"XXXXXX","message":"..."}},
    // 用户需在 https://xiaozhi.me 输入该 6 位码完成设备绑定。
    // 早期版本完全没有处理这个分支 -> 设备未激活时界面空白, 用户无所适从。
    cJSON *act = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(act)) {
        cJSON *code = cJSON_GetObjectItem(act, "code");
        cJSON *msg = cJSON_GetObjectItem(act, "message");
        s_activated = false;
        if (cJSON_IsString(code)) {
            snprintf(s_activation_code, sizeof(s_activation_code), "%s", code->valuestring);
        }
        if (cJSON_IsString(msg)) {
            snprintf(s_activation_msg, sizeof(s_activation_msg), "%s", msg->valuestring);
        }
        ESP_LOGW(TAG, "Device needs activation. code='%s' message='%s'",
                 s_activation_code, s_activation_msg);
        if (s_event_queue) {
            xz_ui_event_t ev = XZ_EV_ACTIVATION;
            xQueueSend(s_event_queue, &ev, 0);
        }
        cJSON_Delete(root);
        return false;   // 未激活: 不进入 WS 连接(服务端会拒绝)
    }
    s_activated = true;

    cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(ws)) {
        cJSON *u = cJSON_GetObjectItem(ws, "url");
        cJSON *t = cJSON_GetObjectItem(ws, "token");
        if (cJSON_IsString(u)) snprintf(s_url, sizeof(s_url), "%s", u->valuestring);
        if (cJSON_IsString(t)) snprintf(s_token, sizeof(s_token), "%s", t->valuestring);
        cJSON *v = cJSON_GetObjectItem(ws, "version");
        if (cJSON_IsNumber(v)) proto_ver = v->valueint;
        s_proto_version = proto_ver;
        ok = s_url[0];
    }
    cJSON_Delete(root);

    // 绑定成功后的 v1 响应可能仍使用公共 test-token。
    // 激活状态只由响应是否包含 activation 段决定；不能把 test-token
    // 当成未激活，否则用户输入设备码后会被固件再次打回待激活页面。
    if (!ok) ESP_LOGW(TAG, "OTA gave no websocket config, using default url");
    if (!s_url[0]) snprintf(s_url, sizeof(s_url), "%s", XZ_WS_URL_DEFAULT);
    // C3 内存墙: TLS 握手需 ~35KB 连续块, 与 opus 编码器互斥(实测 largest
    // 7680 时握手超时)。api.tenclass.net 的 80 端口同路径明文 WS 实测可用
    // (nginx 同时监听 80/443, 101 Switching Protocols)。将 wss:// 降级为
    // ws://, 省 TLS ~35KB, 保住 opus 编码器与 mic 任务。
    if (strncmp(s_url, "wss://", 6) == 0) {
        char plain[256];
        snprintf(plain, sizeof(plain), "ws://%s", s_url + 6);
        memcpy(s_url, plain, sizeof(s_url));
        ESP_LOGW(TAG, "TLS-free mode: ws url=%.60s", s_url);
    }
#if RELAY_MODE
#endif
#if RELAY_MODE
    snprintf(s_url, sizeof(s_url), "wss://192.168.0.35:1443/xiaozhi/v1/");
    ESP_LOGW(TAG, "RELAY_MODE: url=%s", s_url);
#endif
#if PLAIN_WS_TEST
    snprintf(s_url, sizeof(s_url), "ws://192.168.0.33:8799/xiaozhi/v1/");
    ESP_LOGW(TAG, "PLAIN_WS_TEST url=%s", s_url);
#endif
    ESP_LOGI(TAG, "OTA ok: url=%.60s token=%d chars", s_url, (int)strlen(s_token));
    xz_cache_save();
    return true;
}

// ---- WS events ----

static void xiaozhi_send_client_hello(void)
{
    if (!s_ws) return;
    char hello[384];
    snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"version\":%d,\"features\":{\"mcp\":false},"
             "\"transport\":\"websocket\","
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
             "\"channels\":1,\"frame_duration\":60}}", s_proto_version);
    int sent = esp_websocket_client_send_text(s_ws, hello, strlen(hello), pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "client hello sent: %d bytes", sent);
}

/* ---- TTS 下行: Opus 解码 ----
 * 服务器的语音回复是**二进制** websocket 帧(op_code 0x2), 载荷是 Opus。
 * 原实现只处理文本帧(0x1), 因此设备能听懂、能显示文字, 但永远不出声。
 * libopus 的编码器/解码器同处一个库(esp_audio_codec), 解码器函数此前
 * 因无人调用被链接器裁掉, 补上声明即可。
 * 与编码器同样用静态 arena: 解码器 state 在 C3 上也要 ~20KB, 堆碎了拿不到。
 */
extern int opus_decoder_get_size(int channels);
extern int opus_decoder_init(void *st, int Fs, int channels, int *error);
extern int opus_decode(void *st, const unsigned char *data, int len,
                       short *pcm, int frame_size, int decode_fec);
// 解码器与编码器共用同一块 Opus arena。两者在正常轮次里是分时使用的:
// 录音结束才可能收到 TTS, 播报时麦克风采集任务已经退出。这样不再要求
// 碎片化堆里额外找出一块 17784B 连续内存。
static bool s_decoder_ready;
static bool xz_opus_arena_ensure(void);
static bool xz_opus_encoder_yield_to_decoder(void);
static void *s_opus_state;
static int s_opus_state_size;

/* ---- TTS 下行: Opus 解码 + 播放 ----
 * 一帧服务器 Opus -> 16kHz 单声道 PCM -> I2S。frame_duration 在 hello 里
 * 协商为 60ms, 即每帧 960 采样(与上行 XZ_CHUNK_SAMPLES 对称)。
 *
 * ⚠⚠ 为什么必须有独立播放任务(这是"小智不出声"的真正原因, 2026-10-12 定位):
 *   opus_decode() 内部走 silk_decode_frame(), 其栈需求远超 3KB。之前直接
 *   在 websocket 事件回调里解码, 实测必崩:
 *     Guru Meditation: Stack protection fault
 *     Detected in task "websocket_task" ... Stack bounds: 0x3fcd9d50-0x3fcda940
 *     MEPC: 0x42096926 (silk_decode_frame)
 *   websocket 任务的栈只有 3072 字节(cfg.task_stack), 第一帧就踩穿栈保护区。
 *   把栈调大到 20KB+ 会重演开机 RAM 墙(WiFi ESP_ERR_NO_MEM), 因此改为:
 *     websocket 回调只做"收包 + 拷贝", 解码/写 I2S 交给下面这个**静态栈**
 *     任务, 它的栈足够 silk/celt 跑, 且不跟 WiFi 抢大块连续堆。
 */
#define OPUS_DEC_OK 0
// 60ms@16k = 960 采样; 给到 1152(2.3KB) 足够容纳服务端偶发的 120ms 帧。
#define XZ_TTS_PCM_CAPACITY 1152
#define XZ_TTS_QUEUE_DEPTH 8          // ~0.5s 缓冲; 4 太浅, 网络抖动就丢帧
#define XZ_TTS_MAX_OPUS 512          // 单帧上限(见下方说明)
// 2026-10-12: 单帧上限 256 -> 512, 队列深度 4 -> 8。实测云端下发的 TTS
// 帧有 257 字节, 超过原来的 256 上限被整帧丢弃(日志 "tts opus frame too
// large"), 表现为回答只播一半就没了。
// ⚠ 单位是"字节": 本平台 portSTACK_TYPE == uint8_t(见 RISC-V portmacro.h),
//   所以 12288 就是 12KB 栈, 不是 12K words。
//   实测阶梯: 3KB(websocket_task 原值)必崩; 8KB 仍在 silk_decode_frame 崩;
//   故给到 12KB, 再由下面的 high water 日志实测校准到"够用且不浪费"。
// 2026-10-12: 12288 -> 10240. 实测 high water 4212(=用了 8076), 10240 留 2164 余量; // 省下的 2KB 还给开机 DMA(RF 校准差 ~100B 就会 abort 死循环)。
#define XZ_TTS_STACK_WORDS 10240
//   2026-10-12: 原为 6("高于 WS 保证下行不积压"), 这是错的。TTS 任务在
//   i2s_channel_write(阻塞, 超时1000ms) 里连续写, 优先级一旦高于 websocket 任务,
//   持 client->lock 做阻塞读的 ws 任务就彻底让不出 CPU —— 锁被扣到读超时
//   (实测 26 秒), 期间 mic 任务的 listen start/stop 全发不出去, 于是"第一轮能答,"
//   之后再也识别不到"。降回同级 5: I2S DMA 自身有缓冲, 同级轮转下 60ms 一帧的
//   写入不会丢; 真要靠队列兜住抖动, 应该调大 XZ_TTS_QUEUE_DEPTH 而不是抬优先级。
#define XZ_TTS_PRIO 5                 // 必须【不高于】ws 任务(prio 5)

typedef struct {
    int len;                 // 有效字节数 (<= 0 表示停止标记)
    uint8_t data[XZ_TTS_MAX_OPUS];
} xz_tts_item_t;

static short s_tts_pcm[XZ_TTS_PCM_CAPACITY];
static QueueHandle_t s_tts_queue;
static StackType_t s_tts_stack_storage[XZ_TTS_STACK_WORDS];
static StaticTask_t s_tts_tcb;
static TaskHandle_t s_tts_task;
static volatile bool s_tts_running;

static void *s_opus_dec_state;

static bool xz_opus_decoder_ensure(void)
{
    if (s_decoder_ready) return true;
    if (!xz_opus_encoder_yield_to_decoder()) {
        ESP_LOGE(TAG, "opus decoder deferred: encoder still busy");
        return false;
    }
    if (!xz_opus_arena_ensure()) return false;
    const int need = opus_decoder_get_size(1);
    if (need <= 0) {
        ESP_LOGE(TAG, "opus decoder size invalid: %d", need);
        return false;
    }
    if (need > s_opus_state_size) {
        ESP_LOGE(TAG, "opus arena too small: decoder=%d arena=%d", need,
                 s_opus_state_size);
        return false;
    }
    memset(s_opus_state, 0, (size_t)s_opus_state_size);
    int err = 0;
    if (opus_decoder_init(s_opus_state, 16000, 1, &err) != OPUS_DEC_OK) {
        ESP_LOGE(TAG, "opus decoder init failed: %d", err);
        return false;
    }
    s_opus_dec_state = s_opus_state;
    s_decoder_ready = true;
    ESP_LOGI(TAG, "opus decoder ready: state=%d bytes", need);
    return true;
}

void xz_opus_decoder_release(void)
{
    if (!s_decoder_ready) return;
    // arena 由编码器/解码器共用, 会话退出时才统一释放。
    s_opus_dec_state = NULL;
    s_decoder_ready = false;
}

// 播放任务: 从队列取 Opus 帧 -> 解码 -> 写 I2S。栈独占给 silk/celt 用。
// TTS 观测计数(2026-10-12): 旧日志只打前 2 帧/前 3 次播放, 第二回合之后
// 到底有没有收到音频完全不可见。现在每 8 帧打一次进度, TTS_STOP 时打汇总。
static volatile int s_tts_frames_rx;
static volatile int s_tts_frames_played;

static void xz_tts_play_task(void *arg)
{
    (void)arg;
    bool fmt_ready = false;
    ESP_LOGI(TAG, "tts task stack: %u bytes free of %d (high water at start)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), XZ_TTS_STACK_WORDS);
    for (;;) {
        xz_tts_item_t item;
        if (xQueueReceive(s_tts_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (item.len <= 0) break;              // 停止标记
        if (!xz_opus_decoder_ensure()) continue;
        if (!fmt_ready) {
            // 每轮播报只在这里 set 一次格式: 每帧调 set_format 会反复
            // close/open codec, 既爆音又把播放切成碎片。
            if (bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
                ESP_LOGE(TAG, "tts audio format failed");
                continue;
            }
            // esp_codec_dev_open() 会重新应用内部音量；收音机路径会设，
            // TTS 路径也必须显式设，否则重开 codec 后可能保持 0 音量。
            extern uint8_t app_settings_sfx_volume(void);
            bsp_audio_set_volume(app_settings_sfx_volume());
            fmt_ready = true;
        }
        // decode_fec=0: 不做前向纠错(下行是完整帧)。
        const int samples = opus_decode(s_opus_dec_state, item.data, item.len,
                                        s_tts_pcm, XZ_TTS_PCM_CAPACITY, 0);
        if (samples <= 0) continue;           // 静音/结束帧返回 0, 不是错误
        const size_t bytes = (size_t)samples * sizeof(short);
        if (bsp_audio_write(s_tts_pcm, bytes) == ESP_OK) {
            ++s_tts_frames_played;
            if (s_tts_frames_played <= 3 || (s_tts_frames_played % 20) == 0) {
                ESP_LOGI(TAG, "tts frame decoded+played: samples=%d bytes=%u (n=%d)",
                         samples, (unsigned)bytes, s_tts_frames_played);
            }
        }
    }
    // 打印栈高水位: 用来把 XZ_TTS_STACK_WORDS 调到"够用且不浪费"的实测值,
    // 而不是靠猜。0 = 从未溢出, 数字 = 剩余字节数。
    ESP_LOGI(TAG, "tts task stack high water: %u bytes free of %d",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), XZ_TTS_STACK_WORDS);
    s_tts_running = false;
    fmt_ready = false;
    s_tts_task = NULL;
    vTaskDelete(NULL);
}

// 供 WS 回调调用: 启动播放任务(懒启动, 只在真有语音时占用栈)。
static void xz_tts_play_start(void)
{
    if (s_tts_task) return;
    if (!s_tts_queue) {
        s_tts_queue = xQueueCreate(XZ_TTS_QUEUE_DEPTH, sizeof(xz_tts_item_t));
        if (!s_tts_queue) {
            ESP_LOGE(TAG, "tts queue create failed");
            return;
        }
    }
    s_tts_running = true;
    s_tts_task = xTaskCreateStatic(xz_tts_play_task, "xz_tts", XZ_TTS_STACK_WORDS,
                                   NULL, XZ_TTS_PRIO, s_tts_stack_storage, &s_tts_tcb);
    if (!s_tts_task) {
        ESP_LOGE(TAG, "tts task create failed");
        s_tts_running = false;
    }
}

// 只丢弃还没播的帧, 不动播放任务本身。
// 用于 WS 断开这类"随时可能再来新帧"的场景: 绝不能在这里阻塞等任务退出,
// 也不能释放解码器 state —— 那两件事由 xz_tts_play_stop 在真正的会话收尾时做。
static void xz_tts_play_discard(void)
{
    if (s_tts_queue) xQueueReset(s_tts_queue);
}

// 停止播放任务并清空残留帧。会话结束/退出小智时调用。
// ⚠ 必须等任务真正退出后才能释放解码器 state, 否则任务里的 opus_decode
//   会用到已 free 的内存(实测表现为随机 HardFault)。
static bool xz_tts_play_stop(void)
{
    // A server TTS "stop" usually arrives while the final frames are still
    // queued (the WS callback runs at the same priority as the I2S writer).
    // Clearing the queue here clipped the last words of every answer. Queue
    // the stop marker and let the playback task drain what it already has.
    if (s_tts_task) {
        const xz_tts_item_t stop = {.len = 0};
        xQueueSend(s_tts_queue, &stop, pdMS_TO_TICKS(200));
        for (int i = 0; i < 100 && s_tts_task; ++i) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_tts_task) {
            // 极端情况下任务没在 2s 内退出: 绝不能复用 arena。
            ESP_LOGW(TAG, "tts task did not exit in time; keep decoder state");
            return false;
        }
    }
    xz_opus_decoder_release();
    return true;
}

// 把一个二进制 Opus 帧排队给播放任务。返回是否收下。
static bool xz_tts_enqueue_opus_frame(const uint8_t *data, int len)
{
    if (data == NULL || len <= 0) return false;
    if (len > XZ_TTS_MAX_OPUS) {
        // 超长帧直接丢, 不截断: 截断后的 Opus 数据必然解不出正确音频。
        static int oversize = 0;
        if (++oversize <= 3)
            ESP_LOGW(TAG, "tts opus frame too large: %d bytes (max=%d)", len, XZ_TTS_MAX_OPUS);
        return false;
    }
    xz_tts_play_start();
    if (!s_tts_queue) return false;
    // 静态: 本函数跑在 3KB 的 websocket_task 上, 400 字节的局部变量会
    // 进一步压缩它本就不够用的栈。只有 WS 回调这一个写者, 无需加锁。
    static xz_tts_item_t item;
    item.len = len;
    memcpy(item.data, data, (size_t)len);
    // 0 tick: 队列满就丢这一帧。阻塞在 WS 回调里会导致整条 WS 停止收包,
    // 反而把后续所有帧都拖死(丢一帧只是 60ms 音频轻微卡顿)。
    const bool ok = xQueueSend(s_tts_queue, &item, 0) == pdTRUE;
    if (ok) ++s_tts_frames_rx;
    return ok;
}

static void ws_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_BEFORE_CONNECT:
        xz_heap_report("ws_before_connect");   // task栈已分配后的真实可用堆
        break;
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS connected");
        s_ws_connected = true;         // 只置标志; hello 由连接任务在正常上下文发送
        break;
    case WEBSOCKET_EVENT_DATA: {
        // ---- 二进制帧: 服务器下发的 TTS 语音(Opus)。此前完全没处理, 所以
        // 小智"能听懂但不说话"。协议头见官方 main/protocols/protocol.h:
        //   v3: type(1) reserved(1) payload_size(2 BE) payload[]
        //   v2: version(2 BE) type(2 BE) reserved(4) timestamp(4) size(4 BE)
        //
        // ⚠ 这里只做"重组 + 拷贝", 绝不碰 opus_decode: 本回调跑在 3KB 栈的
        //   websocket_task 上, silk 解码会踩穿栈(见 xz_tts_play_task 上方说明)。
        // ⚠ 超过 buffer_size 的帧会被 esp_websocket_client 拆成多次 DATA 事件,
        //   必须按 payload_offset 拼回完整帧再解析, 否则长度字段必然对不上。
        if (e->op_code == 0x2 && e->data_ptr && e->data_len > 0) {   // binary
            const uint8_t *p = (const uint8_t *)e->data_ptr;
            int len = e->data_len;
            // 分片重组: 累积到一个静态缓冲, fin 时才解析。
            static uint8_t s_bin_buf[XZ_WS_FRAME_MAX];
            static int s_bin_len;
            if (e->payload_offset == 0) s_bin_len = 0;   // 新帧开始
            if (s_bin_len < 0 || s_bin_len + len > XZ_WS_FRAME_MAX) {
                // 单帧超出上限: 丢弃并等下一个 fin, 避免越界写。
                if (e->fin) s_bin_len = 0;
            } else {
                memcpy(s_bin_buf + s_bin_len, p, (size_t)len);
                s_bin_len += len;
            }
            if (!e->fin) break;      // 等后续分片
            if (s_bin_len <= 0) break;
            const int flen = s_bin_len;
            s_bin_len = 0;
            const uint8_t *f = s_bin_buf;

            const uint8_t *payload = NULL;
            int payload_len = 0;
            int frame_type = -1;
            if (s_proto_version >= 3) {
                if (flen >= 4) {
                    frame_type = f[0];
                    payload_len = (int)(((uint16_t)f[2] << 8) | f[3]);
                    payload = f + 4;
                    if (payload_len <= 0 || payload_len > flen - 4) payload = NULL;
                }
            } else {
                if (flen >= 16) {
                    frame_type = (int)(((uint16_t)f[2] << 8) | f[3]);
                    uint32_t size = ((uint32_t)f[12] << 24) | ((uint32_t)f[13] << 16) |
                                   ((uint32_t)f[14] << 8) | (uint32_t)f[15];
                    payload = f + 16;
                    payload_len = (int)size;
                    if (payload_len <= 0 || payload_len > flen - 16) payload = NULL;
                }
            }
            // type 0 = OPUS 音频, 1 = JSON 控制消息(走文本通道处理)
            if (payload && frame_type == 0) {
                static int tts_seen = 0;
                if (++tts_seen <= 2)
                    ESP_LOGI(TAG, "tts binary frame: %d payload=%d (proto v%d)",
                             flen, payload_len, s_proto_version);
                xz_tts_enqueue_opus_frame(payload, payload_len);
            } else if (payload == NULL && flen > 16) {
                static int bad = 0;
                if (++bad <= 3)
                    ESP_LOGW(TAG, "tts binary frame malformed: total=%d declared=%d (proto v%d)",
                             flen, payload_len, s_proto_version);
            }
            break;
        }
        if (e->op_code == 0x1 && e->data_ptr && e->data_len > 0) {   // text
            char *tmp = malloc(e->data_len + 1);
            if (!tmp) break;
            memcpy(tmp, e->data_ptr, e->data_len);
            tmp[e->data_len] = 0;
            cJSON *root = cJSON_Parse(tmp);
            free(tmp);
            if (!root) break;
            cJSON *type = cJSON_GetObjectItem(root, "type");
            const char *ty = cJSON_IsString(type) ? type->valuestring : "";
            if (strcmp(ty, "hello") == 0) {
                cJSON *sid = cJSON_GetObjectItem(root, "session_id");
                if (cJSON_IsString(sid))
                    snprintf(s_session_id, sizeof(s_session_id), "%s", sid->valuestring);
                s_ws_open = true;
                ESP_LOGI(TAG, "server hello ok, session=%.20s", s_session_id);
                xz_ui_event_t ev = XZ_EV_WS_OPENED;
                xQueueSend(s_event_queue, &ev, 0);
            } else if (strcmp(ty, "stt") == 0) {
                cJSON *t = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(t))
                    snprintf(s_stt_text, sizeof(s_stt_text), "%.120s", t->valuestring);
                // 把云端实际识别到的文本打进串口日志。识别质量问题的第一手证据:
                // 之前只能靠 UI 上的一行字猜, 无法判断是音频上行有问题还是 ASR 本身。
                ESP_LOGI(TAG, "stt: \"%s\"", s_stt_text);
                xz_ui_event_t ev = XZ_EV_STT;
                xQueueSend(s_event_queue, &ev, 0);
            } else if (strcmp(ty, "tts") == 0) {
                cJSON *st = cJSON_GetObjectItem(root, "state");
                const char *s = cJSON_IsString(st) ? st->valuestring : "";
                if (strcmp(s, "start") == 0 || strcmp(s, "sentence_start") == 0) {
                    cJSON *txt = cJSON_GetObjectItem(root, "text");
                    if (cJSON_IsString(txt))
                        snprintf(s_tts_text, sizeof(s_tts_text), "%.120s", txt->valuestring);
                    ESP_LOGI(TAG, "tts text: \"%s\"", s_tts_text);
                    xz_ui_event_t ev = XZ_EV_TTS_START;
                    xQueueSend(s_event_queue, &ev, 0);
                } else if (strcmp(s, "stop") == 0) {
                    ESP_LOGI(TAG, "tts summary: rx=%d played=%d",
                             s_tts_frames_rx, s_tts_frames_played);
                    s_tts_frames_rx = 0; s_tts_frames_played = 0;
                    xz_ui_event_t ev = XZ_EV_TTS_STOP;
                    xQueueSend(s_event_queue, &ev, 0);
                }
            } else if (strcmp(ty, "goodbye") == 0) {
                xz_ui_event_t ev = XZ_EV_WS_CLOSED;
                xQueueSend(s_event_queue, &ev, 0);
            }
            cJSON_Delete(root);
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        s_ws_open = false;
        s_listening = false;
        // 连接断了就丢弃还没播的语音帧, 免得断线后还冒出半句。
        // 这里只丢帧不停任务: 本回调跑在 websocket_task 上, 不能阻塞等任务退出。
        xz_tts_play_discard();
        ESP_LOGW(TAG, "WS closed");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS error");
        break;
    default:
        break;
    }
}

// ---- connect / hello / close ----
static bool ws_connect_and_hello(void)
{
    if (!s_url[0] || xz_cancel_requested()) return false;
    // 认证头必须在 init 前经 cfg.headers 传入(握手前 set_headers 无效, 会返回 INVALID_ARG)
    char headers[384];
    char dev_id[18];
    uint8_t mac_bin[6] = {0};
    esp_read_mac(mac_bin, ESP_MAC_WIFI_STA);
    snprintf(dev_id, sizeof(dev_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac_bin[0], mac_bin[1], mac_bin[2], mac_bin[3], mac_bin[4], mac_bin[5]);
    load_or_create_client_id();
    if (s_token[0]) {
        snprintf(headers, sizeof(headers),
                 "Authorization: Bearer %s\r\nDevice-Id: %s\r\nClient-Id: %s\r\nProtocol-Version: %d\r\n",
                 s_token, dev_id, s_client_id, s_proto_version);
    } else {
        snprintf(headers, sizeof(headers),
                 "Device-Id: %s\r\nClient-Id: %s\r\nProtocol-Version: %d\r\n",
                 dev_id, s_client_id, s_proto_version);
    }    ESP_LOGI(TAG, "WS headers: %s", headers);

    esp_websocket_client_config_t cfg = {};
    cfg.uri = s_url;
    cfg.headers = headers;   // 握手前唯一有效的传法
    cfg.buffer_size = 512;
    // ws task 栈: 明文 ws:// 无需 TLS 握手栈(那才是吃栈大户)。opus 预热常占
    // ~25KB 后 largest 仅 4~8KB, 栈必须小于它否则 xTaskCreate 失败 -> WS 静默
    // 不连(实测 ws_pre_start 后既无 connected 也无 timeout)。3072 够跑明文 WS。
    cfg.task_stack = 3072;
    cfg.task_prio = 5;      // 与 LVGL/radio 同级, 避免饿死 UI 任务(防 taskLVGL 看门狗)
    // network_timeout_ms 必须【小】。这是 2026-10-12 "第一轮能答, 第二轮就再也
    // 识别不到" 的真凶(串口日志 serial_conv.log 第 1445-1487 行):
    //   esp_websocket_client_recv() 持着 client->lock 做阻塞读
    //   (esp_websocket_client.c:1085), 读超时就是这个值。
    //   TTS 一开播, xz_tts 任务(prio 6)高于 ws 任务(prio 5), 又在
    //   i2s_channel_write(超时1000ms) 里连续写, 持锁的 ws 任务被彻底饿死,
    //   锁最长被扣 network_timeout_ms。期间 mic 任务发 listen start/stop 全都
    //   拿不到锁 -> "Could not lock ws-client within 2000 timeout" -> 音频没送
    //   出去 -> 云端等超时 -> WS 断开。实测 20000ms 时锁被扣住了 26 秒。
    // 取 1000ms: 明文 ws:// 收包是即时的, 锁最坏只被扣 1 秒。
    cfg.network_timeout_ms = 1000;
#if !PLAIN_WS_TEST
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // 21:40 实测可用
#endif
#if RELAY_MODE
    cfg.cert_common_name = "api.tenclass.net";   // 连中继IP但按原域名校验证书
#endif
    cfg.reconnect_timeout_ms = -1;   // 禁自动重连(参考固件: 失败由UI态管理, 防TLS握手风暴发热)
    xz_heap_report("ws_pre_init");
    s_ws = esp_websocket_client_init(&cfg);
    xz_heap_report("ws_post_init");
    if (!s_ws) return false;
    if (xz_cancel_requested()) return false;
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_cb, NULL);
    xz_heap_report("ws_pre_start");
    s_ws_connected = false;
    if (xz_cancel_requested()) return false;
    if (esp_websocket_client_start(s_ws) != ESP_OK) return false;
    // 等 WS 真正进入 CONNECTED 状态(事件回调早于状态更新, 必须用 is_connected 轮询)
    for (int i = 0; i < 150; ++i) {
        if (xz_cancel_requested()) return false;
        if (esp_websocket_client_is_connected(s_ws)) { s_ws_connected = true; break; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (xz_cancel_requested()) return false;
    if (!s_ws_connected) { ESP_LOGW(TAG, "ws connect timeout"); return false; }
    vTaskDelay(pdMS_TO_TICKS(50));   // 让 ws 任务完成状态切换
    // 在正常上下文发客户端 hello(官方 xiaozhi-esp32 同款顺序)
    if (xz_cancel_requested()) return false;
    xiaozhi_send_client_hello();
    // 等服务器 hello
    for (int i = 0; i < 120 && !s_ws_open; ++i) {
        if (xz_cancel_requested()) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (xz_cancel_requested()) return false;
    if (!s_ws_open) { ESP_LOGW(TAG, "no server hello within 12s"); return false; }
    return s_ws_open;
}

// Protocol messages, matching FoloToy-AI-Passport v0.2.0-rc.10 exactly.
//
// The official firmware emits exactly three JSON messages and nothing else:
//   {"type":"hello","version":N,...,"audio_params":{...}}
//   ,"type":"listen","state":"start","mode":"auto"}
//   ,"type":"abort"}
//
// There is deliberately NO "listen state:stop". This build used to send one,
// which is not a message the server knows: it gets ignored, so under mode:auto
// the turn never receives an end-of-speech signal and the session wedges. That
// is the "after a few turns it stops responding and OK does nothing" symptom.
// Ending a turn is done by sending type:abort instead.
static bool send_listen_state(const char *state)
{
    if (!s_ws || !s_ws_open) return false;
    char msg[160];
    int n;
    if (strcmp(state, "abort") == 0) {
        n = snprintf(msg, sizeof(msg),
            "{\"session_id\":\"%s\",\"type\":\"abort\"}",
            s_session_id[0] ? s_session_id : ".");
    } else {
        n = snprintf(msg, sizeof(msg),
            "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"auto\"}",
            s_session_id[0] ? s_session_id : ".", state);
    }
    if (n <= 0) return false;

    // ⚠ 发送结果必须【检查】。esp_websocket_client_send_text 拿不到锁或连接已断时
    //   返回 -1, 而旧代码完全忽略返回值, 照样打印 "sent ..." —— 日志里一串
    //   漂亮的 "sent" 其实全是失败的假象, 排查时被彻底带偏(2026-10-12 踩过)。
    //   失败时明确报错并返回 false, 上层据此知道这一轮音频根本没送出去。
    if (esp_websocket_client_send_text(s_ws, msg, (size_t)n,
                                       pdMS_TO_TICKS(2000)) < 0) {
        ESP_LOGE(TAG, "FAILED to send %s (ws send returned -1)", msg);
        return false;
    }
    ESP_LOGI(TAG, "sent %s", msg);
    return true;
}

void xiaozhi_client_close(void)
{
    if (s_listening) send_listen_state("abort");
    s_listening = false;
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_ws_open = false;
    s_ws_connected = false;
}

// ---- mic capture: 16k PCM -> Opus -> ws binary ----
// The xiaozhi wire protocol always carries Opus.  Do not allocate the encoder
// state from the fragmented heap: after TLS/radio activity ESP32-C3 often has
// only ~11KB contiguous, while Opus needs a ~25KB state block.  libopus exposes
// opus_encoder_get_size()/opus_encoder_init(), so the encoder lives in one
// aligned static arena and never allocates from the heap.
#define XZ_CHUNK_SAMPLES 960   // 60ms @16k
#define XZ_OPUS_FRAME_MAX 1024  // 上行单帧 Opus 上限
// ⚠⚠ 曾经的致命 bug(2026-10-12): encode 的输出缓冲大小写成了
//   sizeof(s_mic_opus)。s_mic_opus 是 uint8_t*, 所以 sizeof 拿到的是
//   **指针大小(C3 上 4 字节)**, 而不是它指向的 1024 字节缓冲。
//   结果每一帧 Opus 都被塞进 4 字节, 音频被截断成无法解码的碎片,
//   云端 ASR 听到的基本只有噪声 —— 于是无论说多久都只返回"我"/"嗯"。
//   必须用缓冲的**容量**常量, 不能用 sizeof(指针)。
// ⚠ 这个值必须贴近 opus_encoder_get_size(1) 的实测值(24524), 不要图省事写大:
//   C3 只有 ~330KB 可用 RAM, 每多占 1KB 静态内存都可能在开机时把 I2S 的
   //   DMA 描述符挤掉 -> "i2s tx 初始化失败: ESP_ERR_NO_MEM"(实测 2026-10-12)。
#define XZ_OPUS_STATE_CAPACITY 24576
// ⚠ 这块静态栈 + TTS 播放栈是全局 .bss 的最大头。C3 只有 ~330KB RAM,
//   每多占 1KB 都会在开机时被 WiFi 的 RF 校准抢不到 DMA 而死循环:
//     phy_init: failed to allocate memory for RF calibration data -> abort()
//   (实测 2026-10-12: post_audio_init dma 掉到 308 字节时必现, 5748 才安全)
//   因此这里取 22KB 而非 32KB, 并在录音任务里打印栈高水位实测校准。
// 2026-10-12: 22528 -> 23552. After the uplink Opus buffer was fixed
//   (see XZ_OPUS_FRAME_MAX), the real encode path ran for the first time,
//   and 22KB left only 660 bytes of headroom -> "Stack protection fault" on
//   frame one. The old value had been tuned against the broken path, where
//   opus_encode bailed out early and never reached the deep SILK/CELT stack.
#define XZ_MIC_STACK_WORDS 25600
#define OPUS_OK 0
// AUDIO(2049) 而非 VOIP(2048): 云端要把这段音频喂给 ASR 做识别, 需要保留
// 辅音/高频细节; VOIP 会为通话质量 aggressively 砍掉这些, 导致识别率崩塌。
#define OPUS_APPLICATION_AUDIO 2049
#define OPUS_AUTO (-1000)      // 让编码器按内容自适应码率(官方 ESP_OPUS_BITRATE_AUTO)
#define OPUS_SET_BITRATE_REQUEST 4002
#define OPUS_SET_VBR_REQUEST 4006
#define OPUS_SET_COMPLEXITY_REQUEST 4010
#define OPUS_SET_INBAND_FEC_REQUEST 4012
#define OPUS_SET_DTX_REQUEST 4016

extern int opus_encoder_get_size(int channels);
extern int opus_encoder_init(void *st, int Fs, int channels, int application);
extern int opus_encoder_ctl(void *st, int request, ...);
extern int opus_encode(void *st, const short *pcm, int frame_size,
                       unsigned char *data, int max_data_bytes);

static bool s_encoder_ready;
static bool s_encoder_gave_up;

// Opus CELT stack use exceeds 18KB; keep the capture task on static storage so
// it also cannot fail because of heap fragmentation.
// The mic task only runs while XiaoZhi is open. While it is closed this
// storage is idle static RAM; radio and the minimized Codex bridge share it
// instead of taking equivalent buffers from the already-tight heap.
//
// Layout: [ bridge task stack ][ radio jitter ring ]. The two owners are
// mutually exclusive with XiaoZhi (entering it stops radio and bridge first),
// but they run together on home, so the regions must not overlap.
// CBDP parsing keeps its receive, subscribe, and long event buffers on this
// task stack. 7 KB overflowed on the first real delta event; 12 KB leaves
// room for the complete authenticated message path.
#define XZ_BRIDGE_STACK_BYTES 12288
static StackType_t s_mic_stack_storage[XZ_MIC_STACK_WORDS]
    __attribute__((aligned(16)));
static StaticTask_t s_mic_tcb;
static TaskHandle_t s_mic_task;

// 录音任务栈上的大缓冲必须搬到静态区。2026-10-12 实测崩溃:
//   长按 OK 说话 -> opus_encode() 走 SILK 编码(silk_encode_frame_FIX ->
//   silk_NSQ_c), 22KB 栈被吃穿, 触发 "Stack protection fault":
//     Stack pointer: 0x3fca57d0
//     Stack bounds:  0x3fca581c - 0x3fcab010
//   设备当场重启回首页, 表现为"一说话就崩"。
// 这两块合计 ~2.9KB(XZ_CHUNK_SAMPLES*2 + 1024), 移出栈后给 SILK 留足余量。
// 关键: mic 栈本来就是静态数组 s_mic_stack_storage[], 所以这次搬移是
// **.bss 内部搬家, 净增静态内存为 0** —— 既不碰 RF 校准/DMA 那条敏感线,
// 又把栈溢出彻底解决。不能改成"把栈加到 32KB", 那会让开机
// post_coding_bridge 的 dma 掉到 1032 字节, 触发 RF 校准失败死循环。
static int16_t *s_mic_pcm;
static uint8_t *s_mic_opus;

static bool xz_opus_arena_ensure(void)
{
    if (s_opus_state != NULL) return true;
    const int encoder_need = opus_encoder_get_size(1);
    const int decoder_need = opus_decoder_get_size(1);
    const int need = encoder_need > decoder_need ? encoder_need : decoder_need;
    if (need <= 0 || need > XZ_OPUS_STATE_CAPACITY) {
        ESP_LOGE(TAG, "opus arena capacity invalid: need=%d cap=%d",
                 need, XZ_OPUS_STATE_CAPACITY);
        return false;
    }
    s_opus_state = heap_caps_malloc(XZ_OPUS_STATE_CAPACITY,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_opus_state == NULL) {
        ESP_LOGE(TAG, "opus arena alloc failed: need=%d largest=%u",
                 XZ_OPUS_STATE_CAPACITY,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    s_opus_state_size = XZ_OPUS_STATE_CAPACITY;
    ESP_LOGI(TAG, "opus shared arena ready: %d bytes (encoder=%d decoder=%d)",
             s_opus_state_size, encoder_need, decoder_need);
    return true;
}

static bool xz_opus_encoder_init(void)
{
    if (s_encoder_ready) return true;
    if (s_encoder_gave_up) return false;

    // TTS and capture are deliberately time-sliced on this single arena.
    // Wait for the playback task to leave opus_decode() before re-initializing
    // the same bytes as an encoder.
    if (s_decoder_ready || s_tts_task) {
        if (!xz_tts_play_stop()) {
            // 播放任务只是暂时没退出，下次按键再试；不能永久放弃（放弃会
            // 让本次会话余下的录音全部失败）。
            return false;
        }
    }
    if (!xz_opus_arena_ensure()) {
        s_encoder_gave_up = true;
        return false;
    }
    memset(s_opus_state, 0, (size_t)s_opus_state_size);
    // ⚠⚠ 必须与官方 xiaozhi-esp32 的 AS_OPUS_ENC_CONFIG() 对齐(2026-10-12):
    //   application = AUDIO (不是 VOIP)、bitrate = OPUS_AUTO (不是硬编码 24000)、
    //   VBR 开、DTX 开。
    //
    //   之前用 VOIP + 固定 24kbps + 关闭 VBR/DTX, 结果云端 ASR 几乎听不清 ——
    //   实测无论说多久, 识别结果几乎恒为"我"一个字。原因是 VOIP 模式是为
    //   人声通话压缩的, 会在 aggressively 砍掉高频与辅音细节; 再叠加固定低码率
    //   和关闭 VBR(动态码率), 单帧可用比特被压到 ASR 无法提取特征的程度。
    //   AUDIO 模式保留更多语音细节, OPUS_AUTO 让编码器按内容自适应码率。
    int ret = opus_encoder_init(s_opus_state, 16000, 1, OPUS_APPLICATION_AUDIO);
    if (ret != OPUS_OK) {
        ESP_LOGE(TAG, "opus static init failed: %d", ret);
        s_encoder_gave_up = true;
        return false;
    }
    opus_encoder_ctl(s_opus_state, OPUS_SET_BITRATE_REQUEST, OPUS_AUTO);
    opus_encoder_ctl(s_opus_state, OPUS_SET_VBR_REQUEST, 1);
    opus_encoder_ctl(s_opus_state, OPUS_SET_COMPLEXITY_REQUEST, 0);
    opus_encoder_ctl(s_opus_state, OPUS_SET_INBAND_FEC_REQUEST, 0);
    // DTX must stay OFF (2026-10-12). With DTX=1 Opus emits 1-byte comfort
    // DTX stays ON, matching the official firmware. I briefly turned it off while
    // chasing this bug, but the one configuration that actually produced a
    // correct transcript had DTX=1, so it is part of the working setup.
    // The 1-byte comfort frames are the silence signalling the cloud VAD expects.
    opus_encoder_ctl(s_opus_state, OPUS_SET_DTX_REQUEST, 1);
    s_encoder_ready = true;
    ESP_LOGI(TAG, "opus static encoder ready: state=%d bytes", s_opus_state_size);
    return true;
}

// Compatibility API used by app exit and startup call sites.
bool xiaozhi_opus_prewarm(void) { return xz_opus_encoder_init(); }

static bool xz_mic_task_stop_and_wait(void)
{
    s_listening = false;
    if (!s_mic_task) return true;

    // opus_encode() runs on this task. Do not free s_opus_state until the
    // capture task has actually left the encoder.
    for (int waited_ms = 0; s_mic_task && waited_ms < 3000; waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_mic_task) {
        ESP_LOGE(TAG, "mic task did not stop in time; keeping Opus state");
        return false;
    }
    return true;
}

static bool xz_opus_encoder_yield_to_decoder(void)
{
    if (!s_encoder_ready && !s_mic_task) return true;
    // 麦克风采集任务里也会走 xz_opus_encoder_init() -> xz_tts_play_stop(),
    // 若这里再阻塞等麦克风退出就会互相等待。TTS 侧直接让出这次机会,
    // 由后续帧重试; 麦克风那条路会先把播放任务停干净。
    if (s_mic_task) {
        ESP_LOGW(TAG, "decoder deferred: mic task still owns the arena");
        return false;
    }
    s_encoder_ready = false;
    return true;
}

void xiaozhi_opus_release(void)
{
    if (!xz_mic_task_stop_and_wait()) {
        // The capture task still owns the encoder, so keep it allocated.
        return;
    }
    s_encoder_ready = false;
    // 先停播放任务(它会自己收尾), 再归还 TTS 解码器 state。
    // play_stop 内部也会释放解码器, 这里再调一次是幂等的。
    if (!xz_tts_play_stop()) {
        // The playback task still owns the shared arena; freeing it here
        // would be a use-after-free. Keep the arena until the next close.
        return;
    }
    xz_opus_decoder_release();
    // XiaoZhi owns this encoder only while its app is open. Once the app
    // exits, hand the large contiguous block back so the minimized radio can
    // reclaim its decoder pool. Re-entry allocates it again; OTA TLS now uses
    // small 4KB/1KB buffers, so the old "never free or it never comes back"
    // constraint no longer applies.
    if (s_opus_state != NULL) {
        free(s_opus_state);
        s_opus_state = NULL;
        s_opus_state_size = 0;
    }
    s_encoder_gave_up = false;
}

static void xz_capture_run(int16_t *pcm)
{
    xz_heap_report("capture_start");
    if (!xz_opus_encoder_init()) {
        xz_ui_event_t ev = XZ_EV_ERROR;
        xQueueSend(s_event_queue, &ev, 0);
        return;
    }
    bool fmt_ok = (bsp_audio_set_format(16000, 16, 1) == ESP_OK);
    xz_heap_report(fmt_ok ? "after_audio_fmt" : "audio_fmt_fail");
    if (!fmt_ok) {
        xz_ui_event_t ev = XZ_EV_ERROR;
        xQueueSend(s_event_queue, &ev, 0);
        return;
    }

    send_listen_state("start");
    s_listening = true;
    uint8_t *opus = s_mic_opus;
    // 音频上行的连续失败计数: 发送失败说明链路已经死了, 别再空转。
    int sent = 0;
    static int s_tx_fail = 0;
    uint32_t frames = 0;
    // 自动增益 + 噪声门(2026-10-12 重写)。
    //
    // 为什么旧版 AGC 让识别更糟: 旧逻辑把**每一帧**都归一化到固定 RMS
    // (2600)。人说话时音节之间 RMS 会剧烈起伏, 而旧版把它压成一条几乎恒定的
    // 直线 —— 实测日志里 RMS 长期在 2000~3200 小幅抖动, 完全没有语音该有的
    // 起伏。那不是"音量稳定", 那是把说话和底噪一起放大到同样响, 语音的动态
    // 范围(ASR 最重要的线索之一)被彻底抹平了。
    //
    // 新做法:
    //   1) 先估底噪(本轮见过的最小帧 RMS), 只有明显高于底噪的帧才认为是
    //      人声 —— 这就是噪声门, 安静房间的底噪不会被猛拉;
    //   2) 增益只在"确认是人声但偏小"时往上加, 慢起慢落, 响起来立刻松手
    //      回 1.0, 不做压缩;
    //   3) 统一缩放保波形, 峰值留余量防削波。
    //
    // 关键区别: 目标是"把人声抬到够用的电平", 不是"把每帧拉到同一个值"。
    const double kVoiceGateRatio = 3.0;   // 高于底噪 3 倍才认为是人声
    const double kTargetRms    = 5200.0;  // 只对确认的人声生效, 远低于削波
    const double kMaxGain      = 10.0;
    const double kRelease      = 0.80;    // 增益回落系数(慢释放)
    double agc_gain  = 1.0;
    double noise_rms = 0.0;
    double frms_min = 0.0, frms_max = 0.0;
    while (s_listening && s_ws_open) {
        // bsp_audio_read -> i2s_channel_read(超时1000ms) 是阻塞的, 本任务 prio 5
        // 与 ws 任务同级, 同级轮转不会饿死对方。真正要防的是"持锁方被高优先级
        // 任务压住": xz_tts 曾是 prio 6, 它连续写 I2S 时, 持 client->lock 做
        // 阻塞读的 ws 任务(prio 5)让不出 CPU, 锁一直被扣到读超时 ——
        // 这就是"第一轮答完, 之后按 OK 全都没反应"的直接原因。
        // 已在两处根治: (1) SEPARATE_TX_LOCK=y, 发送路径不再抢这把读锁;
        //              (2) network_timeout_ms 20000 -> 1000, 锁最坏只扣 1 秒。
        if (bsp_audio_read(pcm, XZ_CHUNK_SAMPLES * sizeof(int16_t)) != ESP_OK) break;
        {   // 逐帧 RMS: 噪声门判定 + 动态范围统计
            double acc = 0.0;
            for (int i = 0; i < XZ_CHUNK_SAMPLES; ++i)
                acc += (double)pcm[i] * (double)pcm[i];
            const double rms = __builtin_sqrt(acc / XZ_CHUNK_SAMPLES);
            if (rms > 0.0) {
                if (frms_min <= 0.0 || rms < frms_min) frms_min = rms;
                if (rms > frms_max) frms_max = rms;
            }
            // 噪声底跟踪: 取见过的最小值作为底噪估计。
            if (noise_rms <= 0.0) noise_rms = rms > 1.0 ? rms : 1.0;
            else if (rms < noise_rms) noise_rms = rms;
            const bool is_voice = rms > noise_rms * kVoiceGateRatio && rms > 60.0;
            // 2026-10-12 削波修复: 上一版 AGC 在 TTS 声音很近的情况下会把
            // gain 拉到 2.69, 音频直接打满 32768 -> 云端收到削波后的方波,
            // 根本识别不出内容。加一个硬性安全网: 当前帧峰值已接近满幅时
            // 本帧强制 gain=1.0, 不再往上加。
            if (is_voice && rms < kTargetRms) {
                int32_t peak = 0;
                for (int i = 0; i < XZ_CHUNK_SAMPLES; ++i) {
                    const int32_t a = pcm[i] < 0 ? -(int32_t)pcm[i] : (int32_t)pcm[i];
                    if (a > peak) peak = a;
                }
                if (peak > 26000) {  // close to full scale: force gain 1
                    agc_gain = 1.0;
                } else {
                    double want = kTargetRms / rms;
                    if (want > kMaxGain) want = kMaxGain;
                    agc_gain += (want - agc_gain) * 0.08;
                }
            } else {
                agc_gain *= kRelease;                    // 慢释放, 不硬切
                if (agc_gain < 1.0) agc_gain = 1.0;
            }
            if (agc_gain > 1.01) {
                // 用 double 累加再回写, 避免 16bit 乘法的溢出与截断失真。
                for (int i = 0; i < XZ_CHUNK_SAMPLES; ++i) {
                    const double v = (double)pcm[i] * agc_gain;
                    if (v > 32767.0) pcm[i] = 32767;
                    else if (v < -32768.0) pcm[i] = -32768;
                    else pcm[i] = (int16_t)v;
                }
            }
        }
        int encoded = opus_encode(s_opus_state, pcm, XZ_CHUNK_SAMPLES,
                                  opus, (int)XZ_OPUS_FRAME_MAX);
        if (encoded <= 0) {
            ESP_LOGW(TAG, "opus encode failed: %d", encoded);
            break;
        }
        // 每帧都记录编码字节数: 这是判断"到底有没有把音频送出去"的唯一
        // 可靠信号。以前每 8 帧才打一次, 结果失败轮次显示 0 帧, 让人误以为
        // 麦克风没工作, 实际上是编码在第一帧就失败了(见下面的日志)。
        ESP_LOGI(TAG, "tx frame %lu: %d bytes", (unsigned long)frames, encoded);
        if (s_proto_version >= 3) {
            // BinaryProtocol3: [type=0 audio][reserved=0][payload_size u16BE] + payload
            static uint8_t tx[4 + XZ_OPUS_FRAME_MAX];
            tx[0] = 0;
            tx[1] = 0;
            tx[2] = (uint8_t)((unsigned)encoded >> 8);
            tx[3] = (uint8_t)((unsigned)encoded & 0xFF);
            memcpy(tx + 4, opus, (size_t)encoded);
            sent = esp_websocket_client_send_bin(s_ws, (const char *)tx,
                                               4 + encoded, pdMS_TO_TICKS(1000));
        } else {
            sent = esp_websocket_client_send_bin(s_ws, (const char *)opus,
                                               encoded, pdMS_TO_TICKS(1000));
        }
        // ⚠ 音频上行也必须检查返回值。连接已经断了 / 拿不到锁时 send_bin 返回 -1,
        //   旧代码当无事发生继续录, 于是 UI 上还显示"正在听", 用户以为在说话,
        //   实际上一个字都没送出去 —— 这正是"想了很久然后未识别"的假象。
        //   连续失败 3 次就判定链路已死, 停止本轮并让上层走重连。
        if (sent < 0) {
            if (++s_tx_fail < 3) {
                ESP_LOGW(TAG, "audio tx failed (%d/%d), frame=%lu len=%d",
                         s_tx_fail, 3, (unsigned long)frames, encoded);
            } else {
                ESP_LOGE(TAG, "audio tx failed 3x in a row, giving up this turn");
                s_listening = false;
                break;
            }
        } else if (s_tx_fail) {
            s_tx_fail = 0;
        }
        if (++frames % 8 == 0) {   // ~0.5s
            {
                double acc = 0.0;
                for (int i = 0; i < XZ_CHUNK_SAMPLES; ++i)
                    acc += (double)pcm[i] * (double)pcm[i];
                const double rms_now = __builtin_sqrt(acc / XZ_CHUNK_SAMPLES);
                ESP_LOGI(TAG, "opus up: %lu frames rms=%u", (unsigned long)frames,
                         (unsigned)rms_now);
                // 语音的过零率大致在 0.02~0.35/样本。接近 0 说明信号是停滞的
                // 直流或单频(不是人声); 明显高于 0.45 说明是削波或纯噪声。
                // 这一项能证明我们交给 Opus 的确实是有人声的真实音频。
                {
                    int zc = 0;
                    for (int i = 1; i < XZ_CHUNK_SAMPLES; ++i)
                        if ((pcm[i - 1] >= 0) != (pcm[i] >= 0)) ++zc;
                    const double zcr = (double)zc / (double)XZ_CHUNK_SAMPLES;
                    const char *hint = zcr < 0.005 ? "TOO FLAT (near-DC/stalled)"
                                   : zcr < 0.02   ? "TOO SMOOTH (hum-like)"
                                   : zcr > 0.45   ? "TOO HOT (clipping/noise)"
                                                  : "speech-like";
                    ESP_LOGI(TAG, "zcr=%.3f [%s]", zcr, hint);
                }
                // 人说话时音节之间 RMS 会剧烈起伏; 底噪则几乎是直线。
                // ratio 接近 1 就是"听到的是底噪而不是人声"的特征。
                ESP_LOGI(TAG, "rms spread: min=%u max=%u ratio=%.1fx gain=%.2f noise=%.0f",
                         (unsigned)frms_min, (unsigned)frms_max,
                         frms_min > 0.0 ? (double)frms_max / (double)frms_min : 0.0,
                         agc_gain, noise_rms);
                frms_min = 0.0; frms_max = 0.0;
                // DC offset probe: 大的直流偏置会虚增 RMS, 让波形贴着轨跑,
                // 云端 ASR 就会稳定地返回"我"这种零星噪声。
                {
                    double mean = 0.0;
                    int32_t peak = 0;
                    for (int i = 0; i < XZ_CHUNK_SAMPLES; ++i) {
                        mean += (double)pcm[i];
                        const int32_t a = pcm[i] < 0 ? -(int32_t)pcm[i] : (int32_t)pcm[i];
                        if (a > peak) peak = a;
                    }
                    mean /= XZ_CHUNK_SAMPLES;
                    ESP_LOGI(TAG, "mic dc=%.0f peak=%ld", mean, (long)peak);
                }
            }
            // 栈余量自检: SILK 编码路径实测会吃 20KB+, 余量低于 1KB 时
            // 提前告警。下次再有人调大编码器复杂度, 日志能直接指出问题,
            // 而不是等到用户"一说话就重启"才发现。
            UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
            if (free_bytes < 1024) {
                ESP_LOGE(TAG, "mic stack nearly exhausted: %u bytes free of %d",
                         (unsigned)free_bytes, XZ_MIC_STACK_WORDS);
            }
        }
    }
    // 说完一轮后必须发 listen state:stop。
    //
    // 2026-10-12 踩过的坑, 记下来免得再犯两次:
    //   1) 最初这里发 stop, 但同时还在发别的东西, 表现为"第一轮能答,
    //      第二轮开始一直思考中到超时"。
    //   2) 中间我对照反编译的官方固件, 以为 mode:auto 不用发结束消息
    //      (硬编码里只看到 "start" 那一条格式串), 于是把 stop 去掉了 ——
    //      结果更糟: 云端收不到任何结束信号, 一直等到超时, 界面显示"未识别"。
    //      stop 用的确实是另一条格式串, 官方固件里有, 我第一次搜索漏了。
    //   3) 也试过只发 type:abort, 那是"打断"语义, 会把云端正在生成的回答
    //      切掉, 同样表现为第二轮卡死。
    // 结论: 正常收尾发 stop; type:abort 只留给"思考超时"强行解锁。
    // 兜底: 若采集循环是自行退出的(而不是 UI 调 stop_listening), 这里补发 stop。
    if (s_listening) send_listen_state("stop");
    s_listening = false;
}

static void xz_capture_task(void *arg)
{
    (void)arg;
    // 借堆: 录音期间才需要这两块缓冲, 录完立刻还给堆。
    // 进小智时收音机已暂停并释放了解码器池, 此时的堆足以借出 ~2.9KB。
    s_mic_pcm = (int16_t *)malloc(sizeof(int16_t) * XZ_CHUNK_SAMPLES);
    s_mic_opus = (uint8_t *)malloc(XZ_OPUS_FRAME_MAX);
    if (!s_mic_pcm || !s_mic_opus) {
        ESP_LOGE(TAG, "mic buffers alloc failed (pcm=%p opus=%p)",
                 s_mic_pcm, s_mic_opus);
        free(s_mic_pcm);  free(s_mic_opus);
        s_mic_pcm = NULL;  s_mic_opus = NULL;
        s_listening = false;
        s_mic_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    xz_capture_run(s_mic_pcm);
    free(s_mic_pcm);  free(s_mic_opus);
    s_mic_pcm = NULL;  s_mic_opus = NULL;
    ESP_LOGI(TAG, "mic task stack high water: %u bytes free of %d",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), XZ_MIC_STACK_WORDS);
    s_mic_task = NULL;
    vTaskDelete(NULL);
}

// ---- public API ----
bool xiaozhi_client_connect(void)
{
    // 2026-10-12 二次修复: 重进小智时上一轮的 s_ws 可能还挂着(退出时 cancel
    // 走的是"由连接任务负责关闭"路径, 但如果那时任务已经死了, handle 就泄漏,
    // 下次进来看到的 transport_poll_write(0) 都是那个僵尸 WS 的。这里兜底销毁,
    // 否则 UI 卡"回答中", 用户必须重启设备。
    if (s_ws) {
        ESP_LOGW(TAG, "stale ws handle detected, closing before reconnect");
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_ws_open = false; s_ws_connected = false; s_listening = false;
    if (s_ws_open) return true;
    if (!xz_connect_begin()) return false;
    bool success = false;
    s_activation_code[0] = 0;
    s_activation_msg[0] = 0;
    // 连接前先停收音机并等它释放 HTTP/解码缓冲, 否则 heap 碎片不足(TLS 需连续 ~10K)
    extern bool radio_is_playing_c(void);
    extern void radio_pause_for_xiaozhi(void);
    if (radio_is_playing_c()) {
        radio_pause_for_xiaozhi();
        ESP_LOGI(TAG, "radio paused for xiaozhi; heap after=free:%u largest:%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    // ⚠ opus 编码器(需 ~24KB 连续块)**不在此处**预热: 它会把连续块占掉,
    // 导致随后 OTA 的 TLS 握手(需 ~20KB 连续块)失败
    // (实测 ESP_ERR_MBEDTLS_SSL_SETUP_FAILED)。
    // opus 预热移到 OTA 成功之后(OTA 用 TLS 需大块, 两者互斥)。
    ESP_LOGI(TAG, "pre-OTA heap: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    // 网关 DNS 不稳(getaddrinfo 202 间歇全挂): 显式设置主备公共 DNS
    extern esp_netif_t *wifi_manager_get_station_netif(void);
    extern int wifi_manager_is_connected(void);
    extern const char *wifi_manager_get_ip_address(void);
    ESP_LOGI(TAG, "wifi state connected=%d ip=%s",
             wifi_manager_is_connected(), wifi_manager_get_ip_address());
    esp_netif_t *netif = wifi_manager_get_station_netif();
    if (!netif) netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    ESP_LOGI(TAG, "DNS target netif=%p", netif);
    if (netif) {
        esp_netif_dns_info_t dns = {};
#if RELAY_MODE
        dns.ip.u_addr.ip4.addr = ipaddr_addr("192.168.0.35");  // 中继/电脑接管DNS
#else
        dns.ip.u_addr.ip4.addr = ipaddr_addr("223.5.5.5");
#endif
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
        dns.ip.u_addr.ip4.addr = ipaddr_addr("114.114.114.114");
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns);
        ESP_LOGI(TAG, "DNS set: 223.5.5.5 / 114.114.114.114");
        // 实时双向音频期间关闭 modem-sleep。MIN_MODEM 会丢 UDP/DNS 响应,
        // 实测 OTA 连续 getaddrinfo()=202, 也会让 WebSocket 收发抖动。
        extern void app_wifi_xiaozhi_mode(bool active);
        app_wifi_xiaozhi_mode(true);
    }
    s_session_id[0] = 0;
    s_encoder_gave_up = false;   // 新会话: 允许再次尝试初始化编码器
    // 证书有效期校验需要正确的系统时间; 若 SNTP 未完成则等待(最多 8s)
    {
        time_t now = 0;
        time(&now);
        if (now < 1700000000) {   // 2023-11 之前 = 未同步
            ESP_LOGW(TAG, "system time invalid (%ld), waiting SNTP up to 8s", (long)now);
            for (int i = 0; i < 80; ++i) {
                if (xz_cancel_requested()) break;
                vTaskDelay(pdMS_TO_TICKS(100));
                time(&now);
                if (now >= 1700000000) break;
            }
            ESP_LOGI(TAG, "time after wait: %ld", (long)now);
        } else {
            ESP_LOGI(TAG, "system time ok: %ld", (long)now);
        }
    }
    if (xz_cancel_requested()) goto done;
    xz_heap_report("xz_connect_pre");
    // 先在堆还完整时抢下 Opus(24.5KB 连续块): 进小智时收音机池已释放、
    // 桥接已停, 此刻连续块最富余。若让 OTA 先跑, TLS 会把这块切碎, Opus
    // 往往就再也拿不到(实测曾固定卡在 largest 20480 < 24524 失败)。
    if (!xiaozhi_opus_prewarm()) goto done;

    // Fast path: reuse the last OTA result. The token and WS endpoint are
    // stable until the device is re-bound, so a normal re-entry should go
    // straight to the WebSocket handshake instead of repeating HTTPS OTA.
    xz_cache_load();
    if (s_config_cached && s_url[0]) {
        ESP_LOGI(TAG, "trying cached WS config first");
        if (ws_connect_and_hello()) {
            success = true;
        } else {
            if (xz_cancel_requested()) goto done;
            ESP_LOGW(TAG, "cached WS failed; refreshing OTA config");
            xiaozhi_client_close();
            xz_cache_clear();
        }
    }

    // Cold path / cached-token fallback: fetch a fresh OTA config, then
    // connect once. If the server is rotating configs this also refreshes NVS.
    if (!success) {
        if (!ota_fetch_config()) goto done;
        s_cached_ver = s_proto_version;
        if (xz_cancel_requested()) goto done;
        if (!ws_connect_and_hello()) goto done;
        success = true;
    }

done:
    if (!success || xz_cancel_requested()) {
        xiaozhi_client_close();
    }
    if (xz_connect_finish()) {
        // Cancellation may have arrived just before ownership was released.
        xiaozhi_client_close();
        xiaozhi_opus_release();
        return false;
    }
    return success;
}

void xiaozhi_client_start_listening(void)
{
    if (!s_ws_open || s_listening) return;
    if (!xz_opus_encoder_init()) {
        xz_ui_event_t ev = XZ_EV_ERROR;
        xQueueSend(s_event_queue, &ev, 0);
        return;
    }
    xz_heap_report("pre_mic_create");
    if (!s_mic_task) {
        s_mic_task = xTaskCreateStatic(xz_capture_task, "xz_mic",
                                       XZ_MIC_STACK_WORDS, NULL, 5,
                                       s_mic_stack_storage, &s_mic_tcb);
        if (!s_mic_task) {
            ESP_LOGE(TAG, "mic task create failed");
            return;
        }
        ESP_LOGI(TAG, "mic task created (static stack)");
    }
}

// 强行打断当前轮次并解锁会话。
// 只有在"已经卡住"时才用(例如思考超时): 官方固件在同样的场景下也是发
// type:abort。正常说完一轮【不要】调用它 —— 那会让服务端断开连接。
void xiaozhi_client_abort_turn(void)
{
    s_listening = false;
    send_listen_state("abort");
}
// 停止本轮采集并发送 listen state:stop。
//
// stop 必须发出去 —— 云端靠它才知道"音频到此为止", 否则会一直等到超时,
// 表现为"未识别"。这里主动发, 而不是只置个标志让采集任务自己收尾,
// 因为 STT 可能在用户还按着 OK 时就到了, 那条路径不会再回到采集循环的收尾处。
void xiaozhi_client_stop_listening(void)
{
    if (!s_listening) return;
    s_listening = false;
    send_listen_state("stop");
}

QueueHandle_t xiaozhi_client_events(void) { return s_event_queue; }

bool xiaozhi_client_init(void)
{
    if (s_event_queue) return true;
    s_event_queue = xQueueCreate(8, sizeof(xz_ui_event_t));
    return s_event_queue != NULL;
}

static bool xz_mic_stack_is_idle(void)
{
    if (s_ws || s_ws_open || s_ws_connected || s_mic_task || s_tts_task ||
        s_encoder_ready || s_decoder_ready) {
        return false;
    }
    return true;
}

// Carved out of the idle microphone stack for the always-on minimized bridge.
// This keeps the bridge's 12KB task stack out of the heap; entering XiaoZhi
// stops the bridge before the mic task is allowed to reuse the full array.
uint8_t *xiaozhi_bridge_stack(size_t *size)
{
    if (size) *size = 0;
    if (!xz_mic_stack_is_idle()) return NULL;
    if (size) *size = XZ_BRIDGE_STACK_BYTES;
    return s_mic_stack_storage;
}

// Radio borrows the remainder as a jitter ring. Ownership is exclusive by
// lifecycle: entering XiaoZhi stops the radio stream first, and this returns
// NULL while any XiaoZhi task or WS owns the stack.
uint8_t *xiaozhi_idle_buffer(size_t *size)
{
    if (size) *size = 0;
    if (!xz_mic_stack_is_idle()) return NULL;
    if (size) *size = sizeof(s_mic_stack_storage) - XZ_BRIDGE_STACK_BYTES;
    return s_mic_stack_storage + XZ_BRIDGE_STACK_BYTES;
}



