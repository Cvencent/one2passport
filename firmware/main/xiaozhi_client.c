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
#define PLAIN_WS_TEST 0            // 1=连本地明文 WS(8089)抓包
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
static char s_url[256];
static char s_token[192];
static char s_stt_text[256];     // last recognized text
static char s_tts_text[256];     // last tts sentence
// 设备激活码: OTA 返回 activation.code 时填入。用户在 https://xiaozhi.me
// 输入此码完成设备绑定(官方固件同款流程)。
static char s_activation_code[32];
static char s_activation_msg[192];
static bool s_activated;         // 已激活(OTA 未返回 activation 段)
static QueueHandle_t s_event_queue;   // ui events: enum xz_ui_event + text ptr

typedef enum {
    XZ_EV_WS_OPENED = 0,
    XZ_EV_WS_CLOSED,
    XZ_EV_STT,          // user speech recognized -> s_stt_text
    XZ_EV_TTS_START,    // answer start -> s_tts_text
    XZ_EV_TTS_STOP,
    XZ_EV_ERROR,
    XZ_EV_ACTIVATION,   // device needs activation -> s_activation_code
} xz_ui_event_t;

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
    esp_http_client_write(c, post, strlen(post));
    esp_http_client_fetch_headers(c);
    const int status = esp_http_client_get_status_code(c);
    ESP_LOGI(TAG, "OTA http status=%d", status);
    static char buf[4096];
    memset(buf, 0, sizeof(buf));
    int read_total = 0;
    while (read_total < (int)sizeof(buf) - 1) {
        int n = esp_http_client_read(c, buf + read_total, sizeof(buf) - 1 - read_total);
        if (n <= 0) break;
        read_total += n;
    }
    buf[read_total] = 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
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
    snprintf(s_url, sizeof(s_url), "ws://192.168.0.35:8089/xiaozhi/v1/");
    ESP_LOGW(TAG, "PLAIN_WS_TEST url=%s", s_url);
#endif
    ESP_LOGI(TAG, "OTA ok: url=%.60s token=%d chars", s_url, (int)strlen(s_token));
    return true;
}

// ---- WS events ----
static bool opus_encoder_init(void);   // 前置声明(WS 连上后预热用)

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
                xz_ui_event_t ev = XZ_EV_STT;
                xQueueSend(s_event_queue, &ev, 0);
            } else if (strcmp(ty, "tts") == 0) {
                cJSON *st = cJSON_GetObjectItem(root, "state");
                const char *s = cJSON_IsString(st) ? st->valuestring : "";
                if (strcmp(s, "start") == 0 || strcmp(s, "sentence_start") == 0) {
                    cJSON *txt = cJSON_GetObjectItem(root, "text");
                    if (cJSON_IsString(txt))
                        snprintf(s_tts_text, sizeof(s_tts_text), "%.120s", txt->valuestring);
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
    if (!s_url[0]) return false;
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
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_cb, NULL);
    xz_heap_report("ws_pre_start");
    s_ws_connected = false;
    if (esp_websocket_client_start(s_ws) != ESP_OK) return false;
    // 等 WS 真正进入 CONNECTED 状态(事件回调早于状态更新, 必须用 is_connected 轮询)
    for (int i = 0; i < 150; ++i) {
        if (esp_websocket_client_is_connected(s_ws)) { s_ws_connected = true; break; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_ws_connected) { ESP_LOGW(TAG, "ws connect timeout"); return false; }
    vTaskDelay(pdMS_TO_TICKS(50));   // 让 ws 任务完成状态切换
    // 在正常上下文发客户端 hello(官方 xiaozhi-esp32 同款顺序)
    xiaozhi_send_client_hello();
    // 等服务器 hello
    for (int i = 0; i < 120 && !s_ws_open; ++i) vTaskDelay(pdMS_TO_TICKS(100));
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

// ---- mic capture: 16k PCM -> opus -> ws binary ----
// (v1 ������: ������ hello ��������� opus; ������60ms=960������, ������320���������3���������? 
//  ������������: ������������ PCM ��������� ��� ������������ opus������ esp_audio_codec ��� opus enc)
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_audio_enc_default.h"

static esp_audio_enc_handle_t s_opus_enc;
static uint8_t s_opus_out[1024];   // 10ms@24kbps opus 单帧 <<100B, 1KB 足够
static bool s_encoder_ready;
static bool s_encoder_gave_up;     // 本轮初始化失败后不再重试(防反复分配失败发热)

// mic 任务栈:**随 opus 编码器一起预分配**(进小智时, 此时收音机池已释放,
// 堆还有大连续块)。若等到长按录音时再分配, 堆已被 opus(25KB)切碎,
// 18KB 连续块拿不到 -> "mic task create failed"(实测)。
#define XZ_MIC_STACK_WORDS 18432   // 18KB, SILK 栈深实测需 >=17.2KB
static StackType_t s_mic_stack_storage[XZ_MIC_STACK_WORDS];
static StackType_t *s_mic_stack = s_mic_stack_storage; // 静态栈，避免和 Opus 抢连续堆
static StaticTask_t s_mic_tcb;

// 供 main 调用(当前 main 不再开机预热, 保留接口以兼容)
bool xiaozhi_opus_prewarm(void) {
    // 先抢 mic 栈(18KB 连续块), 再抢 opus 编码器(25KB): 顺序反了会因
    // opus 造成的碎片导致 mic 栈分配失败。
    // 录音任务栈使用静态存储，Opus 可以独占 OTA 后的连续堆块。
    return opus_encoder_init();
}

// 释放编码器内存: 小智与收音机互斥(编码器 25KB 会让收音机 MP3 解码器
// 无内存可用 -> "There is no memory for MP3 required")。退出小智即释放,
// 把内存还给收音机; 下次进小智(此时收音机已暂停)再申请。
void xiaozhi_opus_release(void)
{
    if (s_opus_enc) {
        esp_audio_enc_close(s_opus_enc);
        s_opus_enc = NULL;
    }
    // mic 栈是静态存储，不随会话释放；仅释放 Opus 堆资源。
    s_encoder_ready = false;
    s_encoder_gave_up = false;
    ESP_LOGI(TAG, "opus encoder released; free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static bool opus_encoder_init(void)
{
    if (s_encoder_ready) return true;
    if (s_encoder_gave_up) return false;   // 已放弃: 不再反复 open 造成内存抖动
    // 使用官方协议的 60ms Opus 帧；当前 ESP32-C3 编码器对 10ms 配置返回
    // ESP_ERR_NOT_SUPPORTED(ret:-7)，导致连接成功但永远没有音频上行。
    esp_opus_enc_config_t cfg = {
        .sample_rate = 16000,
        .channel = 1,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = true,
        .enable_vbr = true,
    };
    esp_audio_err_t reg_ret = esp_opus_enc_register();
    ESP_LOGI(TAG, "opus register ret=%d", reg_ret);
    esp_audio_enc_frame_info_t fi = {0};
    ESP_LOGI(TAG, "opus cfg info ret=%d", esp_opus_enc_get_frame_info_by_cfg(&cfg, &fi));
    if (esp_opus_enc_open(&cfg, sizeof(cfg), (void **)&s_opus_enc) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus enc open failed (heap?), give up this session");
        s_encoder_gave_up = true;
        return false;
    }
    s_encoder_ready = true;
    return true;
}

#define XZ_CHUNK_SAMPLES 960   // 60ms @16k
static void xz_capture_run(int16_t *pcm);   // 前置声明
static TaskHandle_t      s_mic_task;

static void xz_capture_task(void *arg)
{
    (void)arg;
    int16_t pcm[XZ_CHUNK_SAMPLES];
    xz_capture_run(pcm);
    s_mic_task = NULL;
    vTaskDelete(NULL);   // 自删, 栈内存由 xiaozhi_opus_release 释放
}

static void xz_capture_run(int16_t *pcm)
{
    xz_heap_report("capture_start");
    bool fmt_ok = (bsp_audio_set_format(16000, 16, 1) == ESP_OK);
    xz_heap_report(fmt_ok ? "after_audio_fmt" : "audio_fmt_fail");
    if (!fmt_ok || !opus_encoder_init()) {
        xz_ui_event_t ev = XZ_EV_ERROR;
        xQueueSend(s_event_queue, &ev, 0);
        return;   // 常驻任务: 不能自杀, 下次长按仍可重试
    }
    send_listen_state("start");
    s_listening = true;
    const size_t kFrameBytes = XZ_CHUNK_SAMPLES * sizeof(int16_t);   // 60ms@16k = 1920 字节
    uint32_t frames = 0;
    while (s_listening && s_ws_open) {
        if (bsp_audio_read(pcm, kFrameBytes) != ESP_OK) break;
        esp_audio_enc_in_frame_t in = { .buffer = (uint8_t *)pcm, .len = kFrameBytes };
        esp_audio_enc_out_frame_t out = { .buffer = s_opus_out, .len = sizeof(s_opus_out) };
        if (esp_audio_enc_process(s_opus_enc, &in, &out) != ESP_OK) { ESP_LOGE(TAG, "opus process failed"); break; }
        if (out.encoded_bytes > 0) {
            if (s_proto_version >= 3) {
                // BinaryProtocol3: [type=0 opus][reserved=0][payload_size u16BE] + payload
                // 裸发会被服务器当坏帧丢弃(与官方 websocket_protocol.cc 对齐, PC 已实测)
                static uint8_t tx[4 + sizeof(s_opus_out)];
                tx[0] = 0; tx[1] = 0;
                tx[2] = (uint8_t)(out.encoded_bytes >> 8);
                tx[3] = (uint8_t)(out.encoded_bytes & 0xFF);
                memcpy(tx + 4, s_opus_out, out.encoded_bytes);
                esp_websocket_client_send_bin(s_ws, (const char *)tx,
                                              4 + out.encoded_bytes, pdMS_TO_TICKS(1000));
            } else {
                esp_websocket_client_send_bin(s_ws, (const char *)s_opus_out,
                                              out.encoded_bytes, pdMS_TO_TICKS(1000));
            }
            if (++frames % 16 == 0)   // 每约1s一条日志(60ms/帧)
                ESP_LOGI(TAG, "opus up: %lu frames", (unsigned long)frames);
        }
    }
    send_listen_state("stop");
    s_listening = false;
}

// ---- public API ----
bool xiaozhi_client_connect(void)
{
    if (s_ws_open) return true;
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
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
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
                vTaskDelay(pdMS_TO_TICKS(100));
                time(&now);
                if (now >= 1700000000) break;
            }
            ESP_LOGI(TAG, "time after wait: %ld", (long)now);
        } else {
            ESP_LOGI(TAG, "system time ok: %ld", (long)now);
        }
    }
    xz_heap_report("xz_connect_pre");
    if (!ota_fetch_config()) return false;
    // OTA 完成(TLS 已用完并释放), 此时一次性预留录音任务栈并初始化 Opus。
    // xiaozhi_client_start_listening() 使用这块静态栈创建采集任务；只初始化
    // 编码器而不预留栈会让按键路径直接命中 "mic stack not reserved"，
    // 既不会发送 listen start，也不会上传任何语音帧。
    xiaozhi_opus_prewarm();
    ESP_LOGI(TAG, "opus enc post-OTA: ready=%d free=%u largest=%u", s_encoder_ready,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!ws_connect_and_hello()) {
        xiaozhi_client_close();
        return false;
    }
    return true;
}

void xiaozhi_client_start_listening(void)
{
    if (!s_ws_open || s_listening) return;
    xz_heap_report("pre_mic_create");
    // 用预分配的静态栈建任务(栈内存随 opus 一起在进小智时抢下);
    // 任务结束自删, 栈内存由 xiaozhi_opus_release 统一释放。
    if (!s_mic_task) {
        if (!s_mic_stack) {
            ESP_LOGE(TAG, "mic stack not reserved");
            return;
        }
        s_mic_task = xTaskCreateStatic(xz_capture_task, "xz_mic",
                                       XZ_MIC_STACK_WORDS, NULL, 5,
                                       s_mic_stack, &s_mic_tcb);
        if (!s_mic_task) {
            ESP_LOGE(TAG, "mic task create failed");
            return;
        }
        ESP_LOGI(TAG, "mic task created (reserved stack)");
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



