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
    s_config_cached = true;
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
// 解码器 state 按需分配(见上方说明), 首次 TTS 帧到达时才从堆里取。
static bool s_decoder_ready;

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
#define XZ_TTS_QUEUE_DEPTH 4          // 约 0.24s 缓冲, 抗网络抖动
#define XZ_TTS_MAX_OPUS 256           // 单帧上限; 60ms@24kbps ≈ 180B
// ⚠ 单位是"字节": 本平台 portSTACK_TYPE == uint8_t(见 RISC-V portmacro.h),
//   所以 12288 就是 12KB 栈, 不是 12K words。
//   实测阶梯: 3KB(websocket_task 原值)必崩; 8KB 仍在 silk_decode_frame 崩;
//   故给到 12KB, 再由下面的 high water 日志实测校准到"够用且不浪费"。
#define XZ_TTS_STACK_WORDS 12288
#define XZ_TTS_PRIO 6                 // 高于 WS(5), 保证下行不积压; 低于 LVGL 主循环压力可控

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
    const int need = opus_decoder_get_size(1);
    if (need <= 0) {
        ESP_LOGE(TAG, "opus decoder size invalid: %d", need);
        return false;
    }
    void *st = heap_caps_malloc((size_t)need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!st) {
        ESP_LOGW(TAG, "opus decoder alloc failed (need=%d, largest=%u)", need,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    int err = 0;
    if (opus_decoder_init(st, 16000, 1, &err) != OPUS_DEC_OK) {
        ESP_LOGE(TAG, "opus decoder init failed: %d", err);
        free(st);
        return false;
    }
    s_opus_dec_state = st;
    s_decoder_ready = true;
    ESP_LOGI(TAG, "opus decoder ready: state=%d bytes", need);
    return true;
}

void xz_opus_decoder_release(void)
{
    if (!s_decoder_ready) return;
    // libopus 解码器没有额外堆资源, 直接归还 state 内存。
    free(s_opus_dec_state);
    s_opus_dec_state = NULL;
    s_decoder_ready = false;
}

// 播放任务: 从队列取 Opus 帧 -> 解码 -> 写 I2S。栈独占给 silk/celt 用。
static void xz_tts_play_task(void *arg)
{
    (void)arg;
    bool fmt_ready = false;
    uint32_t played = 0;
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
            fmt_ready = true;
        }
        // decode_fec=0: 不做前向纠错(下行是完整帧)。
        const int samples = opus_decode(s_opus_dec_state, item.data, item.len,
                                        s_tts_pcm, XZ_TTS_PCM_CAPACITY, 0);
        if (samples <= 0) continue;           // 静音/结束帧返回 0, 不是错误
        const size_t bytes = (size_t)samples * sizeof(short);
        if (bsp_audio_write(s_tts_pcm, bytes) == ESP_OK) {
            if (++played <= 3) {
                ESP_LOGI(TAG, "tts frame decoded+played: samples=%d bytes=%u (n=%u)",
                         samples, (unsigned)bytes, (unsigned)played);
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
void xz_tts_play_stop(void)
{
    xz_tts_play_discard();
    if (!s_tts_task) return;
    const xz_tts_item_t stop = {.len = 0};
    xQueueSend(s_tts_queue, &stop, pdMS_TO_TICKS(200));
    for (int i = 0; i < 100 && s_tts_task; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_tts_task) {
        // 极端情况下任务没在 2s 内退出: 放弃释放解码器, 宁可泄漏 17KB 也不能
        // 制造 use-after-free。下次进小智会重新分配(见 xz_opus_decoder_ensure)。
        ESP_LOGW(TAG, "tts task did not exit in time; keep decoder state");
        return;
    }
    xz_opus_decoder_release();
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
    return xQueueSend(s_tts_queue, &item, 0) == pdTRUE;
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
    cfg.network_timeout_ms = 20000;  // C3上TLS握手慢, 10s不够
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

static void send_listen_state(const char *state)
{
    if (!s_ws || !s_ws_open) return;
    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"manual\"",
             s_session_id[0] ? s_session_id : ".", state);
    // close json
    size_t len = strlen(msg);
    snprintf(msg + len, sizeof(msg) - len, "}");
    esp_websocket_client_send_text(s_ws, msg, strlen(msg), pdMS_TO_TICKS(2000));
}

void xiaozhi_client_close(void)
{
    if (s_listening) send_listen_state("stop");
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
// ⚠ 这个值必须贴近 opus_encoder_get_size(1) 的实测值(24524), 不要图省事写大:
//   C3 只有 ~330KB 可用 RAM, 每多占 1KB 静态内存都可能在开机时把 I2S 的
   //   DMA 描述符挤掉 -> "i2s tx 初始化失败: ESP_ERR_NO_MEM"(实测 2026-10-12)。
#define XZ_OPUS_STATE_CAPACITY 24576
// ⚠ 这块静态栈 + TTS 播放栈是全局 .bss 的最大头。C3 只有 ~330KB RAM,
//   每多占 1KB 都会在开机时被 WiFi 的 RF 校准抢不到 DMA 而死循环:
//     phy_init: failed to allocate memory for RF calibration data -> abort()
//   (实测 2026-10-12: post_audio_init dma 掉到 308 字节时必现, 5748 才安全)
//   因此这里取 22KB 而非 32KB, 并在录音任务里打印栈高水位实测校准。
#define XZ_MIC_STACK_WORDS 22528
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

static uint8_t s_opus_state[XZ_OPUS_STATE_CAPACITY] __attribute__((aligned(4)));
static int s_opus_state_size;
static bool s_encoder_ready;
static bool s_encoder_gave_up;

// Opus CELT stack use exceeds 18KB; keep the capture task on static storage so
// it also cannot fail because of heap fragmentation.
static StackType_t s_mic_stack_storage[XZ_MIC_STACK_WORDS];
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

static bool xz_opus_encoder_init(void)
{
    if (s_encoder_ready) return true;
    if (s_encoder_gave_up) return false;

    s_opus_state_size = opus_encoder_get_size(1);
    if (s_opus_state_size <= 0 || s_opus_state_size > (int)sizeof(s_opus_state)) {
        ESP_LOGE(TAG, "opus state too large: need=%d capacity=%u",
                 s_opus_state_size, (unsigned)sizeof(s_opus_state));
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
    opus_encoder_ctl(s_opus_state, OPUS_SET_DTX_REQUEST, 1);
    s_encoder_ready = true;
    ESP_LOGI(TAG, "opus static encoder ready: state=%d bytes", s_opus_state_size);
    return true;
}

// Compatibility API used by app exit and startup call sites.
bool xiaozhi_opus_prewarm(void) { return xz_opus_encoder_init(); }

void xiaozhi_opus_release(void)
{
    s_encoder_ready = false;
    s_encoder_gave_up = false;
    // 先停播放任务(它会自己收尾), 再归还 TTS 解码器 state。
    // play_stop 内部也会释放解码器, 这里再调一次是幂等的。
    xz_tts_play_stop();
    xz_opus_decoder_release();
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
    uint32_t frames = 0;
    // 软件自动增益(AGC)。
    //
    // 为什么需要: 设备是桌面摆件, 人站在旁边说话, 声压本来就小; ES8311 的模拟
    // PGA 只有 ~40dB 上限, 光靠硬件增益在嘈杂/安静不同的房间里很难稳定。
    // 实测未做 AGC 时上行 RMS 仅 147, 云端 ASR 只能从近乎静音的信号里认出"我"。
    //
    // 做法: 逐帧算 RMS, 把整帧统一缩放到目标 RMS(留峰值余量防削波)。
    // 统一缩放保留波形形状, 不会破坏 ASR 关心的频谱特征。
    // 只在偏安静时往上拉(最多 12x), 偏响时不动 —— 避免把噪声一起放大。
    const double kTargetRms = 2600.0;
    const double kMaxGain = 12.0;
    double agc_gain = 1.0;
    while (s_listening && s_ws_open) {
        if (bsp_audio_read(pcm, XZ_CHUNK_SAMPLES * sizeof(int16_t)) != ESP_OK) break;
        {
            double acc = 0.0;
            for (int i = 0; i < XZ_CHUNK_SAMPLES; ++i)
                acc += (double)pcm[i] * (double)pcm[i];
            const double rms = __builtin_sqrt(acc / XZ_CHUNK_SAMPLES);
            if (rms > 40.0 && rms < kTargetRms) {
                const double want = kTargetRms / rms;
                agc_gain = agc_gain * 0.7 + (want < kMaxGain ? want : kMaxGain) * 0.3;
                if (agc_gain > 1.0) {
                    // 用 double 累加再回写, 避免 16bit 乘法的溢出与截断失真。
                    for (int i = 0; i < XZ_CHUNK_SAMPLES; ++i) {
                        const double v = (double)pcm[i] * agc_gain;
                        if (v > 32767.0) pcm[i] = 32767;
                        else if (v < -32768.0) pcm[i] = -32768;
                        else pcm[i] = (int16_t)v;
                    }
                }
            } else if (rms >= kTargetRms) {
                agc_gain = 1.0;
            }
        }
        int encoded = opus_encode(s_opus_state, pcm, XZ_CHUNK_SAMPLES,
                                  opus, (int)sizeof(s_mic_opus));
        if (encoded <= 0) {
            ESP_LOGW(TAG, "opus encode failed: %d", encoded);
            break;
        }
        if (s_proto_version >= 3) {
            // BinaryProtocol3: [type=0 audio][reserved=0][payload_size u16BE] + payload
            static uint8_t tx[4 + 1024];
            tx[0] = 0;
            tx[1] = 0;
            tx[2] = (uint8_t)((unsigned)encoded >> 8);
            tx[3] = (uint8_t)((unsigned)encoded & 0xFF);
            memcpy(tx + 4, opus, (size_t)encoded);
            esp_websocket_client_send_bin(s_ws, (const char *)tx,
                                          4 + encoded, pdMS_TO_TICKS(1000));
        } else {
            esp_websocket_client_send_bin(s_ws, (const char *)opus,
                                          encoded, pdMS_TO_TICKS(1000));
        }
        if (++frames % 16 == 0) {  // about once per second (60ms/frame)
            // 麦克风 RMS 探针: 确认真的在拾音。若长期为 0, 说明 I2S 读回来的
            // 是静音, 上行给云端的就只是"房间里很安静" —— ASR 自然只能识别出
            // "我"这种零星噪声。前馈 RMS 是判断"麦克风有没有工作"的唯一可靠信号。
            {
                // 用 double 累加: 960 个样本各自可到 32768, 平方后总和会远超
                // int32 范围(实测打印出 4294967295 就是这里溢出成了 UINT_MAX)。
                double acc = 0.0;
                for (int i = 0; i < XZ_CHUNK_SAMPLES; ++i)
                    acc += (double)pcm[i] * (double)pcm[i];
                ESP_LOGI(TAG, "opus up: %lu frames rms=%u", (unsigned long)frames,
                         (unsigned)__builtin_sqrt(acc / XZ_CHUNK_SAMPLES));
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
    send_listen_state("stop");
    s_listening = false;
}

static void xz_capture_task(void *arg)
{
    (void)arg;
    // 借堆: 录音期间才需要这两块缓冲, 录完立刻还给堆。
    // 进小智时收音机已暂停并释放了解码器池, 此时的堆足以借出 ~2.9KB。
    s_mic_pcm = (int16_t *)malloc(sizeof(int16_t) * XZ_CHUNK_SAMPLES);
    s_mic_opus = (uint8_t *)malloc(1024);
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
    }
    s_url[0] = 0; s_token[0] = 0; s_session_id[0] = 0;
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
    // Static Opus state no longer competes with TLS for heap. Keep OTA first
    // so the session token is fresh, then initialize the encoder in place.
    if (!ota_fetch_config()) goto done;
    s_cached_ver = s_proto_version;
    if (xz_cancel_requested()) goto done;
    if (!xiaozhi_opus_prewarm()) goto done;
    if (!ws_connect_and_hello()) goto done;
    success = true;

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

void xiaozhi_client_stop_listening(void)
{
    s_listening = false;   // capture task ������������������ listen stop
}

QueueHandle_t xiaozhi_client_events(void) { return s_event_queue; }

bool xiaozhi_client_init(void)
{
    if (s_event_queue) return true;
    s_event_queue = xQueueCreate(8, sizeof(xz_ui_event_t));
    return s_event_queue != NULL;
}



