#include "coding_bridge.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "cJSON.h"
#include "codex_bridge_protocol.h"
#include "codex_background_state.h"
#include "codex_alert.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "fap_screenshot.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "app_registry.h"
#include "wireless_bridge.h"
#include "ui_sfx.h"

#define BRIDGE_LINE_MAX 768
#define BRIDGE_TIMEOUT_US (15LL * 1000LL * 1000LL)
#define CAPTURE_SAMPLE_RATE 16000
#define CAPTURE_CHUNK_SAMPLES 160
#define CAPTURE_CHUNK_BYTES (CAPTURE_CHUNK_SAMPLES * sizeof(int16_t))
#define CAPTURE_TASK_STACK 4096
#define SPEECH_CHUNK_BYTES 384
#define CBDP_IDEM_BYTES 16U

// Maps CBDP error codes to a short, user-facing message. The device only
// receives the numeric `code` in the fixed header; error_message (0x0062) is
// optional and carries detail.
static const char *cbdp_error_text(uint16_t code)
{
    switch (code) {
    case CBDP_ERR_NOT_FOUND: return "没有找到对应的会话或任务";
    case CBDP_ERR_BUSY: return "电脑端正忙，请稍后重试";
    case CBDP_ERR_UNAUTHORIZED: return "配对已失效，请重新配对";
    case CBDP_ERR_CAPABILITY_NOT_ENABLED: return "该功能未在电脑端启用";
    case CBDP_ERR_INVALID_PARAM: return "请求参数不完整";
    case CBDP_ERR_INVALID_STATE: return "请先选择一个会话";
    case CBDP_ERR_UPSTREAM_UNAVAILABLE: return "电脑端暂时无法处理，请稍后重试";
    case CBDP_ERR_PAYLOAD_TOO_LARGE: return "内容过大，请缩短后重试";
    case CBDP_ERR_TIMEOUT: return "操作超时，请重试";
    case CBDP_ERR_QUEUE_FULL: return "电脑端队列已满，请稍后重试";
    default: return "电脑端处理失败，请重试";
    }
}

static const char *TAG = "coding_bridge";
extern bool radio_is_playing_c(void);
extern void radio_pause_for_xiaozhi(void);
static bool s_ready;

static bool s_connected;
static bool s_wireless_started;
static bool s_wireless_enabled;
static bool s_bridge_online;
static bool s_allow_foreground_ui;
static cbdp_session_t s_session;
static int64_t s_last_message_us;
// A reconnect replays a backlog of deltas in one burst. Every delta that
// reaches the UI takes the LVGL lock and invalidates the screen, which starved
// the LVGL/IDLE tasks long enough to trip the task watchdog. Apply UI work at a
// bounded rate and let later events converge the final state.
static int64_t s_last_ui_us;
static bool ui_budget_available(void)
{
    const int64_t now = esp_timer_get_time();
    if (now - s_last_ui_us < 120000LL) return false;
    s_last_ui_us = now;
    return true;
}
static app_codex_state_t s_background_codex_state = APP_CODEX_OFFLINE;
static codex_background_state_t s_background_state;
static volatile bool s_capture_requested;
static volatile bool s_capture_cancelled;
static uint32_t s_catalog_request_id;
// Request id of the in-flight projects/threads list command, and its method.
// A host ERROR frame carries only request_id + code; without this mapping the
// catalog state machine stayed on "loading projects" forever (observed on a
// real device when CodexBridge answered projects.list with INTERNAL).
static uint32_t s_catalog_pending_request_id;
static uint16_t s_catalog_pending_method;
// When the list request went out; a host that never answers must not leave the
// picker on "loading" forever.
static int64_t s_catalog_pending_since_us;
static uint32_t s_thread_topic_id;
static uint32_t s_thread_scope_seq;
static uint64_t s_thread_scope_cursor;
static uint32_t s_thread_ref_for_topic;
static uint32_t s_summary_scope_seq;

static uint32_t s_last_acked_seq;
// Bridge pushes only aggregate "unread terminal result" counters over CBDP
// (system.codex_state, event 0x0704); individual turn.completed events are
// not translated on that transport. Remember the previous counters here so a
// rise in completed/failed still produces a local completion alert.
static uint32_t s_last_codex_done_count;
static uint32_t s_last_codex_failed_count;
static bool s_codex_counts_initialized;
// The unread counters oscillate while other clients read results
// (2->3->2->3), so a plain "greater than last" test rang seven times in eleven
// seconds on real hardware. Arm a pending alert on a rise and only ring if the
// count is still elevated after a settle window; a genuine completion stays
// unread, whereas the oscillation falls back within a second.
static int64_t s_codex_alert_armed_us;
static uint32_t s_codex_alert_armed_done;
static uint32_t s_codex_alert_armed_failed;
static bool s_codex_alert_armed_is_failure;
#define CODEX_ALERT_SETTLE_US (4LL * 1000 * 1000)
// One completion is reported through several independent channels: the
// terminal turn event, the aggregate done/failed counters (which only ring
// after CODEX_ALERT_SETTLE_US), and the foreground UI update. Each used to
// ring on its own, so a single finished task produced two or three tones.
// Claim the right to ring once; anything that follows within the window is
// the same completion.
static codex_alert_gate_t s_alert_gate;
#define MAX_SUBSCRIPTIONS 6
static uint32_t s_sub_topic[MAX_SUBSCRIPTIONS];
static uint8_t s_sub_scope[MAX_SUBSCRIPTIONS];
static int s_sub_count;

// 15.1: `threads.list` objects already carry the stable thread topic_id, so
// cache the most recent list. Selection can then bind and subscribe straight
// from the cache instead of depending on a second thread.read round trip that
// a host may reject.
#define THREAD_CACHE_MAX 16
#define THREAD_CACHE_TITLE_MAX 96
static struct {
    uint32_t thread_ref;
    uint32_t topic_id;
    char title[THREAD_CACHE_TITLE_MAX];
} s_thread_cache[THREAD_CACHE_MAX];
static int s_thread_cache_count;

static bool wireless_is_subscribed(uint32_t topic_id, uint8_t scope)
{
    for (int i = 0; i < s_sub_count; ++i) {
        if (s_sub_topic[i] == topic_id && s_sub_scope[i] == scope) return true;
    }
    return false;
}

static bool wireless_topic_subscribed(uint32_t topic_id)
{
    for (int i = 0; i < s_sub_count; ++i) {
        if (s_sub_topic[i] == topic_id) return true;
    }
    return false;
}

// Drop every subscription for one topic. Selecting a new thread replaces the
// previous thread topic; without this the fixed-size table fills up after a few
// selections and new subscriptions are silently refused.
static void wireless_unsubscribe_topic(uint32_t topic_id)
{
    int write_index = 0;
    for (int read_index = 0; read_index < s_sub_count; ++read_index) {
        if (s_sub_topic[read_index] == topic_id) continue;
        s_sub_topic[write_index] = s_sub_topic[read_index];
        s_sub_scope[write_index] = s_sub_scope[read_index];
        ++write_index;
    }
    s_sub_count = write_index;
}

static uint64_t s_summary_scope_cursor;
static TaskHandle_t s_capture_task;
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_ota_partition;
static size_t s_ota_expected;
static size_t s_ota_written;
static int s_ota_progress;
static bool s_ota_active;
static uint32_t s_wireless_request_id = 1U;
static uint32_t s_wireless_tx_id = 1U;
// 本段录音已通过 AUDIO_END 提交的互斥标志(CBDP §19.4: Bridge STT 后自动提交
// turn)。置位后任何 SEND_* 文本提交(重复回调/超时重试/UI 重复确认)都被丢弃,
// 防止同文两投; 新的 AUDIO_BEGIN(下一段录音)时清除。
static bool s_voice_turn_submitted;

// Section 17.4 keys host-side idempotency on (device_id, request_id, method_id,
// params) and remembers it for 24h. A counter that restarts at 1 on every boot
// therefore replays ids the host already recorded and is rejected with
// IDEMPOTENCY_CONFLICT, so both init paths seed it per boot.
static void seed_wireless_counters(void)
{
    s_wireless_request_id = esp_random() & 0x00FFFFFFU;
    if (s_wireless_request_id == 0U) s_wireless_request_id = 1U;
    s_wireless_tx_id = esp_random() & 0x00FFFFFFU;
    if (s_wireless_tx_id == 0U) s_wireless_tx_id = 1U;
}
static size_t s_capture_offset;
static size_t s_text_offset;
static uint32_t s_approval_ref;
static char s_active_thread_id[CODEX_BRIDGE_ID_MAX];
static char s_active_turn_id[CODEX_BRIDGE_ID_MAX];
static uint32_t s_active_thread_ref;
static uint32_t s_active_turn_ref;
static uint8_t s_text_idem[CBDP_IDEM_BYTES];
static uint8_t s_audio_idem[CBDP_IDEM_BYTES];

static void clear_active_turn(void)
{
    s_active_turn_id[0] = '\0';
    s_active_turn_ref = 0U;
}

static void wireless_subscribe(uint32_t topic_id, uint8_t scope, uint64_t cursor);

// Bind the device to one thread and make its event topic reachable.
// `threads.list` objects already carry thread_ref (0x0021) and topic_id
// (0x0030), so selection does not have to depend on a second thread.read
// round trip. `thread.read` is still requested for metadata, but a host that
// cannot serve it must not leave the device unable to subscribe.
static void wireless_bind_thread(uint32_t thread_ref, uint32_t topic_id)
{
    if (thread_ref != 0U) {
        s_active_thread_ref = thread_ref;
        snprintf(s_active_thread_id, sizeof(s_active_thread_id), "%lu",
                 (unsigned long)thread_ref);
    }
    if (topic_id == 0U) return;
    if (s_thread_topic_id != 0U && s_thread_topic_id != topic_id) {
        wireless_unsubscribe_topic(s_thread_topic_id);
    }
    s_thread_topic_id = topic_id;
    s_thread_ref_for_topic = thread_ref;
    s_thread_scope_seq = 0U;
    s_thread_scope_cursor = 0U;
    wireless_subscribe(topic_id, CBDP_SCOPE_THREAD, 0U);
    // `message.read` is a summary-scope event on the same topic.
    wireless_subscribe(topic_id, CBDP_SCOPE_SUMMARY, 0U);
}

static void thread_cache_reset(void) { s_thread_cache_count = 0; }

static void thread_cache_add(uint32_t thread_ref, uint32_t topic_id, const char *title)
{
    if (thread_ref == 0U || topic_id == 0U) return;
    for (int i = 0; i < s_thread_cache_count; ++i) {
        if (s_thread_cache[i].thread_ref == thread_ref) {
            s_thread_cache[i].topic_id = topic_id;
            snprintf(s_thread_cache[i].title, THREAD_CACHE_TITLE_MAX, "%s",
                     title != NULL ? title : "");
            return;
        }
    }
    if (s_thread_cache_count >= THREAD_CACHE_MAX) return;
    s_thread_cache[s_thread_cache_count].thread_ref = thread_ref;
    s_thread_cache[s_thread_cache_count].topic_id = topic_id;
    snprintf(s_thread_cache[s_thread_cache_count].title, THREAD_CACHE_TITLE_MAX, "%s",
             title != NULL ? title : "");
    ++s_thread_cache_count;
}

static uint32_t thread_cache_topic(uint32_t thread_ref)
{
    for (int i = 0; i < s_thread_cache_count; ++i) {
        if (s_thread_cache[i].thread_ref == thread_ref) return s_thread_cache[i].topic_id;
    }
    return 0U;
}

static const char *thread_cache_title(uint32_t thread_ref)
{
    for (int i = 0; i < s_thread_cache_count; ++i) {
        if (s_thread_cache[i].thread_ref == thread_ref) return s_thread_cache[i].title;
    }
    return NULL;
}

typedef enum {
    LONG_TEXT_NONE = 0,
    LONG_TEXT_TRANSCRIPT,
    LONG_TEXT_DONE,
    LONG_TEXT_ERROR,
} long_text_target_t;

static long_text_target_t s_long_text_target;
static char s_long_text[CODING_STATUS_MAX];
static size_t s_long_text_length;
static bool s_long_text_truncated;
static char s_long_text_thread[64];
static char s_long_text_turn[64];
static bool s_speech_playing;
static uint32_t s_long_text_elapsed_ms;

static void wireless_subscribe(uint32_t topic_id, uint8_t scope, uint64_t cursor);
static void wireless_ack(uint32_t topic_id, uint8_t scope, uint32_t ack_seq,
                         uint64_t cursor);
// Section 15.8/9.4 allow the host to coalesce delivery (ack_delay_ms), and a
// subscribe that replays a topic's backlog arrives as a burst. Acknowledging
// every replayed frame one-for-one doubled the traffic and saturated the socket
// until the connection dropped mid-recording, so only the newest cursor is
// acknowledged, at most once per ACK_COALESCE_US.
#define ACK_COALESCE_US (150LL * 1000LL)
static bool s_ack_pending;
static uint32_t s_pending_ack_topic;
static uint8_t s_pending_ack_scope;
static uint32_t s_pending_ack_seq;
static uint64_t s_pending_ack_cursor;
static int64_t s_last_ack_us;
static void wireless_ack_flush(void);

static const char *json_string(const cJSON *root, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring != NULL
               ? value->valuestring
               : "";
}

static uint32_t json_u32(const cJSON *root, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0) return 0U;
    if (value->valuedouble > (double)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)value->valuedouble;
}

static long_text_target_t long_text_target(const char *name)
{
    if (strcmp(name, "transcript") == 0) return LONG_TEXT_TRANSCRIPT;
    if (strcmp(name, "done") == 0) return LONG_TEXT_DONE;
    if (strcmp(name, "error") == 0) return LONG_TEXT_ERROR;
    return LONG_TEXT_NONE;
}

static size_t long_text_capacity(long_text_target_t target)
{
    return target == LONG_TEXT_TRANSCRIPT ? CODING_TRANSCRIPT_MAX
                                         : CODING_STATUS_MAX;
}

static size_t utf8_prefix_bytes(const char *text, size_t maximum)
{
    size_t length = 0U;
    while (text[length] != '\0') {
        unsigned char byte = (unsigned char)text[length];
        size_t width = 1U;
        if ((byte & 0xE0U) == 0xC0U) width = 2U;
        else if ((byte & 0xF0U) == 0xE0U) width = 3U;
        else if ((byte & 0xF8U) == 0xF0U) width = 4U;
        if (length + width > maximum || strlen(text + length) < width) break;
        length += width;
    }
    return length;
}

static void long_text_begin(const cJSON *root)
{
    s_long_text_target = long_text_target(json_string(root, "target"));
    s_long_text_length = 0U;
    s_long_text[0] = '\0';
    s_long_text_truncated = false;
    strlcpy(s_long_text_thread, json_string(root, "thread_id"),
            sizeof(s_long_text_thread));
    strlcpy(s_long_text_turn, json_string(root, "turn_id"),
            sizeof(s_long_text_turn));
    s_long_text_elapsed_ms = json_u32(root, "elapsed_ms");
}

static void long_text_append(const cJSON *root)
{
    if (s_long_text_target == LONG_TEXT_NONE ||
        long_text_target(json_string(root, "target")) != s_long_text_target) {
        return;
    }
    const char *text = json_string(root, "text");
    size_t capacity = long_text_capacity(s_long_text_target);
    size_t available = capacity - 1U - s_long_text_length;
    size_t length = utf8_prefix_bytes(text, available);
    memcpy(s_long_text + s_long_text_length, text, length);
    s_long_text_length += length;
    s_long_text[s_long_text_length] = '\0';
    if (text[length] != '\0') s_long_text_truncated = true;
}

static void long_text_finish(void)
{
    if (s_long_text_target == LONG_TEXT_NONE) return;
    if (s_long_text_truncated) {
        static const char marker[] = "\n内容过长，剩余部分请在电脑查看";
        size_t capacity = long_text_capacity(s_long_text_target);
        size_t marker_length = sizeof(marker) - 1U;
        size_t prefix = utf8_prefix_bytes(s_long_text,
                                          capacity - marker_length - 1U);
        s_long_text[prefix] = '\0';
        strlcat(s_long_text, marker, capacity);
    }
    if (bsp_lvgl_lock(500)) {
        if (s_long_text_target == LONG_TEXT_TRANSCRIPT) {
            app_coding_bridge_transcript(s_long_text);
        } else if (s_long_text_target == LONG_TEXT_ERROR) {
            app_coding_result_identity(s_long_text_thread, s_long_text_turn);
            app_coding_bridge_error_with_elapsed(s_long_text, s_long_text_elapsed_ms);
        } else {
            app_coding_result_identity(s_long_text_thread, s_long_text_turn);
            app_coding_bridge_done_with_elapsed(s_long_text, s_long_text_elapsed_ms);
        }
        bsp_lvgl_unlock();
    }
    s_long_text_target = LONG_TEXT_NONE;
    s_long_text_length = 0U;
}

static void speech_start(const cJSON *root)
{
    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
    uint32_t sample_rate = cJSON_IsNumber(rate) ? (uint32_t)rate->valuedouble
                                                : 16000U;
    s_speech_playing = false;
    if (s_capture_requested || radio_is_playing_c() ||
        app_settings_voice_mode() != APP_VOICE_SPEECH ||
        sample_rate < 8000U || sample_rate > 24000U) {
        return;
    }
    if (bsp_audio_set_format(sample_rate, 16, 1) == ESP_OK) {
        bsp_audio_set_volume(app_settings_sfx_volume());
        s_speech_playing = true;
    }
}

static void speech_chunk(const cJSON *root)
{
    if (!s_speech_playing) return;
    if (radio_is_playing_c()) {
        s_speech_playing = false;
        return;
    }
    const char *encoded = json_string(root, "data");
    unsigned char decoded[SPEECH_CHUNK_BYTES];
    size_t decoded_length = 0U;
    if (encoded[0] == '\0' ||
        mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_length,
                              (const unsigned char *)encoded,
                              strlen(encoded)) != 0 ||
        decoded_length == 0U || (decoded_length & 1U) != 0U ||
        bsp_audio_write(decoded, decoded_length) != ESP_OK) {
        s_speech_playing = false;
    }
}

static bool send_json(cJSON *root)
{
    if (!s_ready || root == NULL) {
        cJSON_Delete(root);
        return false;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return false;
    }

    bool sent = false;
    size_t length = strlen(json);
    char *wire = malloc(length + 2);
    if (wire != NULL) {
        memcpy(wire, json, length);
        wire[length] = '\n';
        wire[length + 1] = '\0';
        sent = usb_serial_jtag_is_connected() && fap_usb_write(wire, length + 1);
        free(wire);
    }
    cJSON_free(json);
    return sent;
}

bool coding_bridge_capability_enabled(const char *capability)
{
    // 9.3: only capabilities that survived the three-way intersection may be
    // used, so consult the session's effective set instead of the firmware's
    // own supported set.
    if (capability == NULL || !s_session.ready) return false;
    static const struct { const char *name; uint8_t bit; } names[] = {
        {"status", CBDP_CAP_STATUS}, {"text", CBDP_CAP_TEXT},
        {"tasks", CBDP_CAP_TASKS}, {"results", CBDP_CAP_RESULTS},
        {"approval", CBDP_CAP_APPROVAL}, {"audio", CBDP_CAP_AUDIO},
        {"voice", CBDP_CAP_VOICE}, {"read_ack", CBDP_CAP_READ_ACK},
        {"control", CBDP_CAP_CONTROL}, {"history", CBDP_CAP_HISTORY},
    };
    for (size_t index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (strcmp(names[index].name, capability) == 0) {
            const uint64_t bit = UINT64_C(1) << names[index].bit;
            return (s_session.effective_caps & bit) != 0U;
        }
    }
    return false;
}

static cJSON *event_create(const char *type)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddStringToObject(root, "type", type) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

bool coding_bridge_is_connected(void)
{
    return s_connected;
}

static app_codex_state_t background_pet_state(void)
{
    switch (codex_background_pet_state(&s_background_state)) {
    case CODEX_BACKGROUND_IDLE: return APP_CODEX_IDLE;
    case CODEX_BACKGROUND_BUSY: return APP_CODEX_BUSY;
    case CODEX_BACKGROUND_DONE: return APP_CODEX_DONE;
    case CODEX_BACKGROUND_FAIL: return APP_CODEX_FAIL;
    case CODEX_BACKGROUND_OFFLINE:
    default: return APP_CODEX_OFFLINE;
    }
}

static void home_codex_counts_changed(void)
{
    app_home_set_codex_counts(s_background_state.running,
                              s_background_state.done,
                              s_background_state.failed);
}

static void home_codex_state_changed(void)
{
    s_background_codex_state = background_pet_state();
    home_codex_counts_changed();
    app_home_set_codex_state(s_background_codex_state);
}

static bool wireless_build_and_send_command(uint16_t method_id,
                                             const uint8_t *params,
                                             size_t params_length);

static void background_read_state_changed(const char *message_id)
{
    if (message_id == NULL || message_id[0] == '\0') return;
    if (!codex_background_mark_read(&s_background_state, message_id)) return;
    home_codex_state_changed();
}

bool app_coding_catalog_is_loading(void);

void coding_bridge_set_foreground_ui(bool active)
{
    s_allow_foreground_ui = active;

    /* The bridge can come online or drop while the Codex page sits in the
     * background; that path only updates background state and never touches the
     * foreground state machine. Reconcile once the page reaches the foreground,
     * otherwise it keeps showing a stale OFFLINE (not connected) or ONLINE. */
    if (active && bsp_lvgl_lock(500)) {
        app_coding_bridge_connected(s_connected);
        bsp_lvgl_unlock();
    }

    // Re-issue the catalog request now that background delivery is on.
    // app_coding_enter() asks for projects before this function runs, so the
    // first RESULT can arrive while s_allow_foreground_ui is still false and be
    // dropped (items skipped and no END), which stranded the picker on
    // "正在读取真实 Codex 项目…". Asking again here is cheap and idempotent.
    if (active && app_coding_catalog_is_loading()) {
        coding_bridge_request_projects();
    }
}

bool coding_bridge_foreground_ui_active(void)
{
    return s_allow_foreground_ui;
}

static void send_wifi_status(const char *status, const char *ip)
{
    cJSON *root = event_create("wifi_status");
    if (root != NULL) {
        cJSON_AddStringToObject(root, "status", status != NULL ? status : "error");
        if (ip != NULL && ip[0] != '\0') {
            cJSON_AddStringToObject(root,
                                   status != NULL && strcmp(status, "connected") == 0
                                       ? "ip" : "text",
                                   ip);
        }
    }
    send_json(root);
}

static void send_ota_status(const char *status, int progress, const char *text)
{
    cJSON *root = event_create("ota_status");
    if (root != NULL) {
        cJSON_AddStringToObject(root, "status", status);
        cJSON_AddNumberToObject(root, "progress", progress);
        if (text != NULL && text[0] != '\0') {
            cJSON_AddStringToObject(root, "text", text);
        }
    }
    send_json(root);
}

static void ota_abort_with_error(const char *message)
{
    if (s_ota_active) esp_ota_abort(s_ota_handle);
    s_ota_active = false;
    s_ota_partition = NULL;
    send_ota_status("error", s_ota_progress, message);
}

static void ota_begin_message(const cJSON *root)
{
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsNumber(size) || size->valuedouble <= 0) {
        send_ota_status("error", 0, "固件大小无效");
        return;
    }
    size_t expected = (size_t)size->valuedouble;
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL || expected > partition->size) {
        send_ota_status("error", 0, "没有足够的 OTA 空间");
        return;
    }
    if (s_ota_active) esp_ota_abort(s_ota_handle);
    esp_err_t result = esp_ota_begin(partition, expected, &s_ota_handle);
    if (result != ESP_OK) {
        send_ota_status("error", 0, esp_err_to_name(result));
        return;
    }
    s_ota_partition = partition;
    s_ota_expected = expected;
    s_ota_written = 0;
    s_ota_progress = 0;
    s_ota_active = true;
    send_ota_status("ready", 0, "开始接收固件");
}

static void ota_chunk_message(const cJSON *root)
{
    if (!s_ota_active) {
        send_ota_status("error", 0, "OTA 尚未开始");
        return;
    }
    const char *encoded = json_string(root, "data");
    unsigned char decoded[512];
    size_t decoded_length = 0;
    if (encoded[0] == '\0' ||
        mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_length,
                              (const unsigned char *)encoded,
                              strlen(encoded)) != 0 ||
        decoded_length == 0 || s_ota_written + decoded_length > s_ota_expected) {
        ota_abort_with_error("固件分片损坏");
        return;
    }
    esp_err_t result = esp_ota_write(s_ota_handle, decoded, decoded_length);
    if (result != ESP_OK) {
        ota_abort_with_error(esp_err_to_name(result));
        return;
    }
    s_ota_written += decoded_length;
    int progress = (int)(s_ota_written * 100 / s_ota_expected);
    if (progress >= s_ota_progress + 5) {
        s_ota_progress = progress;
        send_ota_status("writing", progress, "");
    }
}

static void ota_end_message(void)
{
    if (!s_ota_active || s_ota_written != s_ota_expected) {
        ota_abort_with_error("固件数据不完整");
        return;
    }
    esp_err_t result = esp_ota_end(s_ota_handle);
    s_ota_active = false;
    if (result == ESP_OK) result = esp_ota_set_boot_partition(s_ota_partition);
    s_ota_partition = NULL;
    if (result != ESP_OK) {
        send_ota_status("error", s_ota_progress, esp_err_to_name(result));
        return;
    }
    send_ota_status("complete", 100, "升级完成，设备正在重启");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void send_capture_error(const char *message)
{
    cJSON *root = event_create("capture_error");
    if (root != NULL &&
        cJSON_AddStringToObject(root, "message", message) == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }
    send_json(root);
}

// 17.4: idem = first_16_bytes(SHA-256(device_id || request_id || method_id ||
// canonical_params)). Seeding from the request counter alone made a restarted
// device reuse an idem that the host had already recorded for a different
// method, and the host then rejected the command with IDEMPOTENCY_CONFLICT.
static void make_idem_for(uint8_t idem[CBDP_IDEM_BYTES], uint32_t request_id,
                          uint16_t method_id, const uint8_t *params,
                          size_t params_length)
{
    cbdp_build_idem(idem, request_id, method_id, params, params_length);
}

static bool send_audio_chunk(const int16_t *samples, size_t sample_count)
{
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    if (samples == NULL || sample_count == 0U ||
        sample_count > SIZE_MAX / sizeof(*samples)) {
        return false;
    }
    const size_t payload_length = sample_count * sizeof(*samples);
    if (!cbdp_build_audio_chunk(frame, sizeof(frame), &length, s_wireless_tx_id,
                                (uint32_t)s_capture_offset, (const uint8_t *)samples,
                                payload_length)) {
        return false;
    }
    return wireless_bridge_send_frame(frame, length);
}

static uint32_t next_tx_id(void)
{
    // Each SEND_*/AUDIO_* transaction needs its own tx_id: idempotency and the
    // host-side transaction state are keyed by it. Reusing one tx_id across
    // recordings left stale transactions behind whenever a recording ended
    // without AUDIO_END.
    ++s_wireless_tx_id;
    if (s_wireless_tx_id == 0U) s_wireless_tx_id = 1U;
    return s_wireless_tx_id;
}

static bool send_audio_begin(void)
{
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    s_voice_turn_submitted = false;  // 新录音周期: 允许本段 AUDIO_END 置位
    s_wireless_tx_id = next_tx_id();
    make_idem_for(s_audio_idem, s_wireless_tx_id, CBDP_MT_AUDIO_BEGIN,
                  (const uint8_t *)s_active_thread_id, strlen(s_active_thread_id));
    if (!cbdp_build_audio_begin(frame, sizeof(frame), &length, s_wireless_tx_id, 0U,
                                CAPTURE_SAMPLE_RATE, 1U, 16U, 0U, NULL,
                                s_active_thread_ref, s_active_thread_id,
                                s_active_thread_ref == 0U && s_active_thread_id[0] == '\0',
                                s_audio_idem)) {
        return false;
    }
    return wireless_bridge_send_frame(frame, length);
}

static bool send_audio_end(size_t total_bytes)
{
    // 重传纪律(CBDP §19.4/§4): 弱网重发必须复用同一个 s_wireless_tx_id 和
    // s_audio_idem —— 换新事务号会被 Bridge 当成新输入。本函数不复用
    // 发号器, socket_send_all 的重试也始终重发同一帧字节。
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    if (total_bytes > UINT32_MAX ||
        !cbdp_build_audio_end(frame, sizeof(frame), &length, s_wireless_tx_id,
                              (uint32_t)total_bytes)) {
        return false;
    }
    const bool ok = wireless_bridge_send_frame(frame, length);
    if (ok) s_voice_turn_submitted = true;
    return ok;
}

static bool send_text_bytes(const char *text)
{
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    if (text == NULL || !s_connected || !wireless_bridge_session_ready()) {
        return false;
    }
    const size_t total_bytes = strlen(text);
    if (total_bytes == 0U || total_bytes > UINT32_MAX) {
        return false;
    }
    (void)next_tx_id();
    make_idem_for(s_text_idem, s_wireless_tx_id, CBDP_MT_SEND_BEGIN,
                  (const uint8_t *)text, total_bytes);
    if (!cbdp_build_send_begin(frame, sizeof(frame), &length, s_wireless_tx_id,
                               0U, (uint32_t)total_bytes,
                               s_active_thread_ref, s_active_thread_id,
                               s_active_thread_ref == 0U && s_active_thread_id[0] == '\0',
                               s_text_idem) ||
        !wireless_bridge_send_frame(frame, length)) {
        return false;
    }
    size_t offset = 0U;
    while (offset < total_bytes) {
        size_t chunk = total_bytes - offset;
        if (chunk > CBDP_MAX_CHUNK_BYTES) chunk = CBDP_MAX_CHUNK_BYTES;
        if (!cbdp_build_send_chunk(frame, sizeof(frame), &length,
                                   s_wireless_tx_id, (uint32_t)offset,
                                   (const uint8_t *)(text + offset), chunk) ||
            !wireless_bridge_send_frame(frame, length)) {
            return false;
        }
        offset += chunk;
    }
    if (!cbdp_build_send_end(frame, sizeof(frame), &length, s_wireless_tx_id,
                             (uint32_t)total_bytes)) {
        return false;
    }
    s_text_offset = total_bytes;
    return wireless_bridge_send_frame(frame, length);
}

static void capture_task(void *argument)
{
    (void)argument;
    ui_sfx_suspend(true);
    int16_t samples[CAPTURE_CHUNK_SAMPLES];
    size_t total_bytes = 0;
    unsigned level_tick = 0;
    unsigned chunk_stalls = 0U;
    bool capture_ok = true;
        if (bsp_audio_set_format(CAPTURE_SAMPLE_RATE, 16, 1) != ESP_OK) {
        send_capture_error("麦克风格式设置失败");
        s_capture_requested = false;
        s_capture_task = NULL;
        ui_sfx_suspend(false);
        vTaskDelete(NULL);
        return;
    }

    s_capture_offset = 0U;
    capture_ok = send_audio_begin();
    const bool audio_begun = capture_ok;
    while (capture_ok && s_capture_requested) {
        // 19.5: never send more audio than the host declared in WELCOME.
        if (s_session.max_audio_bytes != 0U &&
            total_bytes + sizeof(samples) > s_session.max_audio_bytes) {
            ESP_LOGW(TAG, "audio capture hit max_audio_bytes=%u",
                     (unsigned)s_session.max_audio_bytes);
            s_capture_requested = false;
            capture_ok = false;
            send_capture_error("录音超过电脑端允许的长度");
            break;
        }
        if (bsp_audio_read(samples, sizeof(samples)) != ESP_OK) {
            send_capture_error("麦克风读取失败");
            s_capture_requested = false;
            capture_ok = false;
            break;
        }
        if (!send_audio_chunk(samples, CAPTURE_CHUNK_SAMPLES)) {
            // The host can briefly stop draining (for example while it streams
            // topic history back to us). Tolerate a short stall, but do not keep
            // re-reading and discarding microphone frames: the transport is a
            // real-time stream, so beyond a moment there is nothing useful left
            // to send. Keep s_capture_offset unchanged so whatever did go out
            // stays contiguous for the host.
            if (++chunk_stalls < 25U) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            capture_ok = false;
            send_capture_error("上传通道拥塞，录音已停止");
            break;
        }
        chunk_stalls = 0U;
        s_capture_offset += sizeof(samples);
        total_bytes += sizeof(samples);
        if (++level_tick >= 4) {
            int32_t peak = 0;
            for (size_t i = 0; i < CAPTURE_CHUNK_SAMPLES; ++i) {
                int32_t magnitude = samples[i];
                if (magnitude < 0) magnitude = -magnitude;
                if (magnitude > peak) peak = magnitude;
            }
            int level = peak <= 128 ? 0 : (int)((peak - 128) * 100 / 8000);
            if (level > 100) level = 100;
            if (bsp_lvgl_lock(0)) {
                app_coding_capture_level((uint8_t)level);
                bsp_lvgl_unlock();
            }
            level_tick = 0;
        }
    }
    // An opened transaction must always be closed: AUDIO_BEGIN without
    // AUDIO_END strands host-side state and makes every later recording fail
    // on the same tx_id. Cancelling still sends the end frame so the host can
    // discard the partial audio deliberately.
    if (audio_begun) {
        if (!send_audio_end(total_bytes)) capture_ok = false;
    }
    s_capture_requested = false;
    ESP_LOGI(TAG, "capture bytes=%u stack_free=%u",
             (unsigned)total_bytes, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (!capture_ok) send_capture_error("音频发送失败，请检查桥接连接");
    if (bsp_lvgl_lock(2000)) {
        app_coding_capture_level(0);
        if (!capture_ok && !s_capture_cancelled) {
            app_coding_bridge_error("音频发送失败，请检查桥接连接");
        }
        bsp_lvgl_unlock();
    }
    s_capture_task = NULL;
    ui_sfx_suspend(false);
    vTaskDelete(NULL);
}

static void start_capture(void)
{
    if (!s_ready || s_capture_task != NULL) {
        return;
    }
    s_capture_requested = true;
    s_capture_cancelled = false;
    if (xTaskCreate(capture_task, "voice_capture", CAPTURE_TASK_STACK, NULL, 5,
                    &s_capture_task) != pdPASS) {
        s_capture_requested = false;
        s_capture_task = NULL;
        send_capture_error("录音任务创建失败");
    }
}

void coding_bridge_send_record_start(void)
{
    // 19.5: AUDIO_BEGIN needs the audio capability, and an AUDIO_* transaction
    // must be bound to a selected thread. Refuse locally with a clear message
    // instead of starting a capture the host will reject.
    if (!wireless_bridge_session_ready() ||
        !coding_bridge_capability_enabled("audio")) {
        if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_bridge_error("电脑端未启用语音上行");
            bsp_lvgl_unlock();
        }
        return;
    }
    if (s_active_thread_ref == 0U && s_active_thread_id[0] == '\0') {
        if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_bridge_error("请先选择一个会话");
            bsp_lvgl_unlock();
        }
        return;
    }
    // The codec sits behind one shared bus lock. While the radio or speech
    // playback owns it, a 16 kHz capture read can be starved for seconds and
    // the recording silently stops making progress. Stop the radio and wait for
    // it to actually idle before opening the microphone.
    if (radio_is_playing_c()) {
        radio_pause_for_xiaozhi();
    }
    start_capture();
}

void coding_bridge_send_record_stop(void)
{
    s_capture_requested = false;
}

void coding_bridge_cancel_record(void)
{
    s_capture_cancelled = true;
    s_capture_requested = false;
}

bool coding_bridge_send_voice(const char *text)
{
    if (s_voice_turn_submitted) {
        // 本段录音已经通过 AUDIO_* 提交(Bridge STT 自动提交 turn), 这里的
        // 文本只是回显 —— 再发一遍就是重复输入(2026-10-25 Bridge 取证)。
        ESP_LOGW(TAG, "voice turn already submitted via AUDIO_*; "
                      "drop duplicate text submission");
        return true;
    }
    return send_text_bytes(text);
}

void coding_bridge_send_approval(coding_decision_t decision)
{
    uint8_t idem[CBDP_IDEM_BYTES];
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    uint8_t wire_decision = 2U;
    if (decision == CODING_DECISION_APPROVE_ONCE) wire_decision = 0U;
    else if (decision == CODING_DECISION_APPROVE_SESSION) wire_decision = 1U;
    else if (decision == CODING_DECISION_DENY) wire_decision = 2U;
    else if (decision == CODING_DECISION_CANCEL_TASK) wire_decision = 3U;
    ++s_wireless_request_id;
    {
        uint8_t approve_params[8];
        size_t approve_length = 0U;
        (void)cbdp_tlv_write_u32(approve_params, sizeof(approve_params),
                                 &approve_length, CBDP_TLV_APPROVAL_REF,
                                 s_approval_ref);
        (void)cbdp_tlv_write_u8(approve_params, sizeof(approve_params),
                                &approve_length, CBDP_TLV_DECISION, wire_decision);
        make_idem_for(idem, s_wireless_request_id, CBDP_METHOD_APPROVAL_RESOLVE,
                      approve_params, approve_length);
    }
    if (!cbdp_build_approval_resolve(frame, sizeof(frame), &length,
                                     s_wireless_request_id, s_approval_ref,
                                     wire_decision, NULL, idem)) {
        return;
    }
    wireless_bridge_send_frame(frame, length);
}

void coding_bridge_send_cancel(void)
{
    if (!s_connected || !wireless_bridge_session_ready()) return;
    uint8_t params[128];
    size_t params_length = 0U;
    if (s_active_thread_ref != 0U &&
        !cbdp_tlv_write_u32(params, sizeof(params), &params_length,
                            CBDP_TLV_THREAD_REF, s_active_thread_ref)) {
        return;
    }
    if (s_active_turn_ref != 0U &&
        !cbdp_tlv_write_u32(params, sizeof(params), &params_length,
                            CBDP_TLV_TURN_REF, s_active_turn_ref)) {
        return;
    }
    if (s_active_thread_ref == 0U && s_active_thread_id[0] != '\0' &&
        !cbdp_tlv_write_bytes(params, sizeof(params), &params_length,
                              CBDP_TLV_THREAD_ID, s_active_thread_id,
                              strlen(s_active_thread_id))) {
        return;
    }
    uint8_t idem[CBDP_IDEM_BYTES];
    ++s_wireless_request_id;
    make_idem_for(idem, s_wireless_request_id, CBDP_METHOD_TURN_INTERRUPT,
                  params, params_length);
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    if (cbdp_build_command(frame, sizeof(frame), &length, s_wireless_request_id,
                           CBDP_METHOD_TURN_INTERRUPT, params, params_length, idem)) {
        wireless_bridge_send_frame(frame, length);
    }
}

void coding_bridge_request_projects(void)
{
    wireless_build_and_send_command(CBDP_METHOD_PROJECTS_LIST, NULL, 0U);
}

void coding_bridge_cancel_catalog(void)
{
    s_catalog_request_id++;
}

void coding_bridge_confirm_selection(void)
{
    s_catalog_request_id++;
}

void coding_bridge_select_project(const char *id)
{
    if (id == NULL) return;
    const unsigned long reference = strtoul(id, NULL, 10);
    if (reference == 0UL || reference > UINT32_MAX) return;
    uint8_t params[16];
    size_t params_length = 0U;
    if (!cbdp_tlv_write_u32(params, sizeof(params), &params_length,
                            CBDP_TLV_PROJECT_REF, (uint32_t)reference)) {
        return;
    }
    wireless_build_and_send_command(CBDP_METHOD_THREADS_LIST, params,
                                    params_length);
}

void coding_bridge_select_thread(const char *id)
{
    if (id == NULL) return;
    const unsigned long reference = strtoul(id, NULL, 10);
    if (reference == 0UL || reference > UINT32_MAX) return;
    // Ask for the thread's metadata/history FIRST and subscribe only once that
    // reply lands. Subscribing up front makes the host immediately replay the
    // whole topic backlog, and while it is busy draining that replay it stops
    // answering anything else: the read below then sat behind the replay for
    // more than 3 x heartbeat_seconds and the device dropped the session before
    // the selection ever completed.
    uint8_t params[64];
    size_t params_length = 0U;
    if (!cbdp_tlv_write_u32(params, sizeof(params), &params_length,
                            CBDP_TLV_THREAD_REF, (uint32_t)reference) ||
        !cbdp_tlv_write_u16(params, sizeof(params), &params_length,
                           CBDP_TLV_HISTORY_DEPTH, 2U)) {
        return;
    }
    wireless_build_and_send_command(CBDP_METHOD_THREAD_READ, params,
                                    params_length);
}

void coding_bridge_create_thread(void)
{
    wireless_build_and_send_command(CBDP_METHOD_THREAD_START, NULL, 0U);
}

const char *coding_bridge_thread_title(const char *id)
{
    if (id == NULL) return NULL;
    const unsigned long reference = strtoul(id, NULL, 10);
    if (reference == 0UL || reference > UINT32_MAX) return NULL;
    return thread_cache_title((uint32_t)reference);
}

void coding_bridge_request_review(void)
{
    if (s_active_thread_ref != 0U) {
        uint8_t params[16];
        size_t params_length = 0U;
        if (cbdp_tlv_write_u32(params, sizeof(params), &params_length,
                               CBDP_TLV_THREAD_REF, s_active_thread_ref)) {
            wireless_build_and_send_command(CBDP_METHOD_THREAD_READ, params,
                                            params_length);
        }
    }
}

bool coding_bridge_result_seen(const char *thread_id, const char *turn_id)
{
    (void)turn_id;
    if (thread_id == NULL || thread_id[0] == '\0' || !s_connected) return false;
    uint32_t thread_ref = s_active_thread_ref;
    if (thread_ref == 0U) {
        const unsigned long parsed = strtoul(thread_id, NULL, 10);
        if (parsed == 0UL || parsed > UINT32_MAX) return false;
        thread_ref = (uint32_t)parsed;
    }
    if (thread_ref != 0U) {
        uint8_t params[16];
        size_t params_length = 0U;
        if (cbdp_tlv_write_u32(params, sizeof(params), &params_length,
                               CBDP_TLV_THREAD_REF, thread_ref)) {
            wireless_build_and_send_command(CBDP_METHOD_THREAD_READ, params,
                                            params_length);
            return true;
        }
    }
    return false;
}

static bool dispatch_message(const cJSON *root)
{
    const char *type = json_string(root, "type");
    bool recognized = strcmp(type, "bridge_ready") == 0 ||
                      strcmp(type, "transcript") == 0 ||
                      strcmp(type, "task_queued") == 0 ||
                      strcmp(type, "task_status") == 0 ||
                      strcmp(type, "approval") == 0 ||
                      strcmp(type, "done") == 0 ||
                      strcmp(type, "error") == 0 ||
                      strcmp(type, "codex_catalog_begin") == 0 ||
                      strcmp(type, "codex_catalog_item") == 0 ||
                      strcmp(type, "codex_catalog_end") == 0 ||
                      strcmp(type, "codex_selected") == 0 ||
                      strcmp(type, "codex_state") == 0 ||
                      strcmp(type, "text_begin") == 0 ||
                      strcmp(type, "text_chunk") == 0 ||
                      strcmp(type, "text_end") == 0 ||
                      strcmp(type, "speech_start") == 0 ||
                      strcmp(type, "speech_chunk") == 0 ||
                      strcmp(type, "speech_end") == 0;
    if (!recognized) {
        return false;
    }
    const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    if (cJSON_IsNumber(request_id) &&
        (uint32_t)request_id->valuedouble != s_catalog_request_id) return true;

    if (strcmp(type, "error") == 0) {
        s_capture_requested = false;
    }

    s_last_message_us = esp_timer_get_time();

    if (strcmp(type, "text_begin") == 0) {
        long_text_begin(root);
        return true;
    }
    if (strcmp(type, "text_chunk") == 0) {
        long_text_append(root);
        return true;
    }
    if (strcmp(type, "text_end") == 0) {
        long_text_finish();
        return true;
    }
    if (strcmp(type, "speech_start") == 0) {
        speech_start(root);
        return true;
    }
    if (strcmp(type, "speech_chunk") == 0) {
        speech_chunk(root);
        return true;
    }
    if (strcmp(type, "speech_end") == 0) {
        s_speech_playing = false;
        return true;
    }
    if (!bsp_lvgl_lock(500)) {
        return true;
    }

    if (!s_connected) {
        s_connected = true;
        app_coding_bridge_connected(true);
    }

    if (strcmp(type, "codex_state") == 0) {
        app_coding_bridge_state(json_u32(root, "running"),
                                json_u32(root, "done"), json_u32(root, "failed"));
    } else if (strcmp(type, "transcript") == 0) {
        app_coding_bridge_transcript(json_string(root, "text"));
    } else if (strcmp(type, "task_queued") == 0) {
        app_coding_bridge_task_queued(json_string(root, "text"));
    } else if (strcmp(type, "task_status") == 0) {
        app_coding_bridge_task_status(json_string(root, "text"));
    } else if (strcmp(type, "approval") == 0) {
        app_coding_bridge_approval(json_string(root, "question"),
                                   json_string(root, "detail"));
    } else if (strcmp(type, "done") == 0) {
        app_coding_result_identity(json_string(root, "thread_id"),
                                   json_string(root, "turn_id"));
        app_coding_bridge_done_with_elapsed(json_string(root, "text"), json_u32(root, "elapsed_ms"));
    } else if (strcmp(type, "error") == 0) {
        app_coding_result_identity(json_string(root, "thread_id"),
                                   json_string(root, "turn_id"));
        app_coding_bridge_error_with_elapsed(json_string(root, "text"), json_u32(root, "elapsed_ms"));
    } else if (strcmp(type, "codex_catalog_begin") == 0) {
        app_coding_catalog_begin(json_string(root, "scope"));
    } else if (strcmp(type, "codex_catalog_item") == 0) {
        app_coding_catalog_item(json_string(root, "scope"),
                                json_string(root, "id"),
                                json_string(root, "title"),
                                json_string(root, "detail"));
    } else if (strcmp(type, "codex_catalog_end") == 0) {
        app_coding_catalog_end(json_string(root, "scope"));
    } else if (strcmp(type, "codex_selected") == 0) {
        app_coding_task_selected(json_string(root, "title"),
                                 json_string(root, "project"),
                                 json_string(root, "last_message"));
    }
    bsp_lvgl_unlock();
    return true;
}

static void process_line(const char *line, bool usb_transport)
{
    cJSON *root = cJSON_Parse(line);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }
    const char *type = json_string(root, "type");
    if (strcmp(type, "wifi_setup") == 0) {
        esp_err_t result = wireless_bridge_configure(json_string(root, "ssid"),
                                                     json_string(root, "password"),
                                                     json_string(root, "token"));
        send_wifi_status(result == ESP_OK ? "saved" : "error",
                         result == ESP_OK ? "" : esp_err_to_name(result));
    } else if (!usb_transport && strcmp(type, "ota_begin") == 0) {
        ota_begin_message(root);
    } else if (!usb_transport && strcmp(type, "ota_chunk") == 0) {
        ota_chunk_message(root);
    } else if (!usb_transport && strcmp(type, "ota_end") == 0) {
        ota_end_message();
    } else {
        dispatch_message(root);
    }
    cJSON_Delete(root);
}

static bool wireless_build_and_send_command(uint16_t method_id,
                                             const uint8_t *params, size_t params_length)
{
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    uint8_t idem[CBDP_IDEM_BYTES];
    ++s_wireless_request_id;
    if (method_id == CBDP_METHOD_PROJECTS_LIST ||
        method_id == CBDP_METHOD_THREADS_LIST) {
        s_catalog_pending_request_id = s_wireless_request_id;
        s_catalog_pending_method = method_id;
        s_catalog_pending_since_us = esp_timer_get_time();
    }
    make_idem_for(idem, s_wireless_request_id, method_id, params, params_length);
    if (!cbdp_build_command(frame, sizeof(frame), &length, s_wireless_request_id,
                            method_id, params, params_length, idem)) {
        return false;
    }
    return wireless_bridge_send_frame(frame, length);
}

static const uint8_t *wireless_tlv_bytes(const uint8_t *tlv, size_t tlv_len,
                                          uint16_t tag, size_t *value_len)
{
    const cbdp_tlv_t *item = cbdp_tlv_find(tlv, tlv_len, tag);
    const uint8_t *value = NULL;
    if (item == NULL || !cbdp_tlv_read_bytes(item, &value, value_len)) return NULL;
    return value;
}

static bool copy_utf8_field(const uint8_t *tlv, size_t tlv_len, uint16_t tag,
                            char *output, size_t output_size)
{
    if (output == NULL || output_size == 0U) return false;
    size_t value_len = 0U;
    const uint8_t *value = wireless_tlv_bytes(tlv, tlv_len, tag, &value_len);
    if (value == NULL) {
        output[0] = '\0';
        return false;
    }
    const size_t copy_len = value_len < output_size - 1U ? value_len : output_size - 1U;
    memcpy(output, value, copy_len);
    output[copy_len] = '\0';
    return true;
}

// Event bodies can be several kilobytes. Handing that straight to an LVGL
// label makes text layout (word wrapping over CJK) run long enough on the
// caller's task to trip the task watchdog, so the UI only ever sees a bounded
// preview. The full text stays available in the background state.
#define UI_PREVIEW_BYTES 384U
static bool copy_utf8_preview(const uint8_t *tlv, size_t tlv_len, uint16_t tag,
                              char *output, size_t output_size)
{
    if (output == NULL || output_size == 0U) return false;
    const size_t limit = output_size - 1U < UI_PREVIEW_BYTES
                             ? output_size - 1U : UI_PREVIEW_BYTES;
    if (!copy_utf8_field(tlv, tlv_len, tag, output, limit + 1U)) {
        output[0] = '\0';
        return false;
    }
    // Drop a trailing partial multi-byte sequence so LVGL never has to walk an
    // invalid UTF-8 tail.
    size_t length = strlen(output);
    while (length > 0U) {
        const unsigned char last = (unsigned char)output[length - 1U];
        if ((last & 0x80U) == 0U) break;          // ASCII
        size_t lead = length - 1U;
        while (lead > 0U && ((unsigned char)output[lead] & 0xC0U) == 0x80U) --lead;
        const unsigned char first = (unsigned char)output[lead];
        size_t expected = 1U;
        if ((first & 0xE0U) == 0xC0U) expected = 2U;
        else if ((first & 0xF0U) == 0xE0U) expected = 3U;
        else if ((first & 0xF8U) == 0xF0U) expected = 4U;
        if (length - lead >= expected) break;      // complete sequence
        length = lead;
        output[length] = '\0';
    }
    return length > 0U;
}

static void wireless_set_connected(bool connected)
{
    if (s_connected == connected) return;
    s_connected = connected;
    s_background_state.connected = connected;
    if (!connected) {
        s_bridge_online = false;
        s_catalog_pending_request_id = 0U;
        s_catalog_pending_since_us = 0;
        codex_background_reset(&s_background_state);
    }
    home_codex_state_changed();
    if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
        app_coding_bridge_connected(connected);
        bsp_lvgl_unlock();
    }
}

// Section 15.4: subscriptions are per-session. A fresh TCP session starts with
// none, so drop the local table whenever the transport loses the peer and
// re-issue the summary (plus any active thread) subscription on AUTH_OK.
static void wireless_reset_subscriptions(void)
{
    s_sub_count = 0;
    s_last_acked_seq = 0U;
    s_summary_scope_seq = 0U;
    s_summary_scope_cursor = 0U;
    s_thread_scope_seq = 0U;
    s_thread_scope_cursor = 0U;
}

static void wireless_handle_result(const cbdp_frame_header_t *header,
                                   const uint8_t *tlv, size_t tlv_len)
{
    (void)header;
    uint16_t method_id = 0U;
    uint8_t state = 0U;
    const cbdp_tlv_t *item = cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_METHOD_ID);
    (void)cbdp_tlv_read_u16(item, &method_id);
    item = cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_STATE);
    (void)cbdp_tlv_read_u8(item, &state);
    const cbdp_tlv_t *result = cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_RESULT);
    const uint8_t *result_tlv = result != NULL ? result->value : NULL;
    const size_t result_len = result != NULL ? result->length : 0U;

    if (method_id == CBDP_METHOD_PROJECTS_LIST || method_id == CBDP_METHOD_THREADS_LIST ||
        method_id == CBDP_METHOD_THREAD_READ) {
        char thread_ref_text[16] = {0};
        uint32_t thread_ref = 0U;
        cbdp_tlv_read_u32(cbdp_tlv_find(result_tlv, result_len, CBDP_TLV_THREAD_REF),
                          &thread_ref);
        if (thread_ref == 0U && method_id != CBDP_METHOD_PROJECTS_LIST) {
            cbdp_tlv_read_u32(cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_THREAD_REF),
                              &thread_ref);
        }
        const cbdp_tlv_t *objects = cbdp_tlv_find(result_tlv, result_len,
                                                  CBDP_TLV_OBJECTS);
        if (objects == NULL) objects = cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_OBJECTS);
        // The catalog state machine only emits items while it is in the
        // matching LOADING_* state and only becomes selectable after the END
        // event. The CBDP result path therefore has to bracket the object list
        // with begin/end exactly like the legacy JSON catalog stream does;
        // without END the device stayed in LOADING_* forever, so OK was ignored
        // and the user could never leave the project/thread picker.
        const bool is_list = method_id == CBDP_METHOD_PROJECTS_LIST ||
                             method_id == CBDP_METHOD_THREADS_LIST;
        const char *list_scope = method_id == CBDP_METHOD_PROJECTS_LIST
                                     ? "projects" : "threads";
        if (method_id == CBDP_METHOD_THREADS_LIST) thread_cache_reset();
        if (is_list && coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_catalog_begin(list_scope);
            bsp_lvgl_unlock();
        }
        if (objects != NULL) {
            cbdp_tlv_cursor_t cursor = {
                .data = objects->value, .length = objects->length, .offset = 0U,
            };
            uint16_t object_count = (uint16_t)(objects->length / 2U);
            if (objects->length >= 2U) {
                object_count = cbdp_read_u16(objects->value);
                cursor.offset = 2U;
            }
            cbdp_tlv_t object;
            uint16_t parsed_count = 0U;
            while (parsed_count < object_count && cbdp_tlv_next(&cursor, &object)) {
                if (object.tag != CBDP_TLV_OBJECT) continue;
                ++parsed_count;
                char title[128] = {0};
                char id[16] = {0};
                char detail[256] = {0};
                uint8_t object_type = 0U;
                uint32_t object_ref = 0U;
                uint32_t object_topic_id = 0U;
                copy_utf8_field(object.value, object.length, CBDP_TLV_TITLE,
                                title, sizeof(title));
                copy_utf8_field(object.value, object.length, CBDP_TLV_SUMMARY,
                                detail, sizeof(detail));
                cbdp_tlv_read_u8(cbdp_tlv_find(object.value, object.length,
                                               CBDP_TLV_OBJECT_TYPE),
                                 &object_type);
                if (object_type == 10U) {
                    cbdp_tlv_read_u32(cbdp_tlv_find(object.value, object.length,
                                                    CBDP_TLV_PROJECT_REF),
                                      &object_ref);
                } else if (object_type == 0U) {
                    cbdp_tlv_read_u32(cbdp_tlv_find(object.value, object.length,
                                                    CBDP_TLV_THREAD_REF),
                                      &object_ref);
                    // 15.1: thread objects carry their stable topic_id. Without
                    // it the device cannot subscribe to that thread's events.
                    cbdp_tlv_read_u32(cbdp_tlv_find(object.value, object.length,
                                                    CBDP_TLV_TOPIC_ID),
                                      &object_topic_id);
                }
                if (object_ref != 0U) snprintf(id, sizeof(id), "%lu",
                                               (unsigned long)object_ref);
                if (method_id == CBDP_METHOD_THREADS_LIST) {
                    thread_cache_add(object_ref, object_topic_id, title);
                }
                if (!coding_bridge_foreground_ui_active() || !bsp_lvgl_lock(500)) {
                    continue;
                }
                // A thread.read REPLIES with the thread object followed by its
                // message history. Only the thread object belongs in the
                // catalog/selection path; rendering every historical message as
                // a catalog item held the LVGL lock long enough to starve the
                // device (task watchdog inside wireless_bridge).
                if (method_id == CBDP_METHOD_THREAD_READ && object_type != 0U) {
                    bsp_lvgl_unlock();
                    continue;
                }
                const char *scope = method_id == CBDP_METHOD_PROJECTS_LIST
                                        ? "projects" : "threads";
                app_coding_catalog_item(scope, id, title, detail);
                if (method_id == CBDP_METHOD_THREAD_READ) {
                    // 18.1/19.1: remember the selected thread so SEND_* and
                    // AUDIO_* can bind to it. Falling back to use_active_thread
                    // is invalid because the device never ran active_state.read.
                    if (thread_ref != 0U) {
                        snprintf(thread_ref_text, sizeof(thread_ref_text), "%lu",
                                 (unsigned long)thread_ref);
                    }
                    wireless_bind_thread(thread_ref, object_topic_id);
                    app_coding_result_identity(thread_ref_text, "");
                    app_coding_task_selected(title, "", detail);
                }
                bsp_lvgl_unlock();
            }
        }
        if (is_list && coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_catalog_end(list_scope);
            bsp_lvgl_unlock();
        }
        if (is_list) {
            s_catalog_pending_request_id = 0U;
            s_catalog_pending_since_us = 0;
        }
        s_last_message_us = esp_timer_get_time();
        return;
    }
    // 19.4: an AUDIO_* transaction is answered with RESULT(method=AUDIO_BEGIN)
    // carrying the transcript. The device previously handled only the legacy
    // JSON "transcript" message, so a CBDP recording ended on a vague error and
    // the user never reached the confirm screen.
    if (method_id == 0x0013U && state <= 1U) {
        char transcript[CODING_TRANSCRIPT_MAX] = {0};
        if (!copy_utf8_field(tlv, tlv_len, CBDP_TLV_TRANSCRIPT,
                             transcript, sizeof(transcript))) {
            copy_utf8_field(result_tlv, result_len, CBDP_TLV_TRANSCRIPT,
                            transcript, sizeof(transcript));
        }
        if (transcript[0] != '\0' && coding_bridge_foreground_ui_active() &&
            bsp_lvgl_lock(500)) {
            app_coding_bridge_transcript(transcript);
            bsp_lvgl_unlock();
        } else {
            ESP_LOGW(TAG, "audio result without transcript");
            if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
                app_coding_bridge_error("没有识别到语音，请重试");
                bsp_lvgl_unlock();
            }
        }
        s_last_message_us = esp_timer_get_time();
        return;
    }

    if (method_id == CBDP_METHOD_TURN_START || method_id == CBDP_METHOD_TURN_STEER ||
        method_id == CBDP_METHOD_THREAD_START) {
        uint32_t thread_ref = 0U;
        uint32_t turn_ref = 0U;
        uint32_t topic_id = 0U;
        char thread_id[CODEX_BRIDGE_ID_MAX] = {0};
        // RESULT fields live in the frame header (the gateway's _result()
        // writes them top-level); some hosts additionally nest them under
        // TAG_RESULT. Read the header first and fall back to the nested block
        // so both wire shapes are accepted.
        if (!cbdp_tlv_read_u32(cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_THREAD_REF),
                               &thread_ref)) {
            cbdp_tlv_read_u32(cbdp_tlv_find(result_tlv, result_len, CBDP_TLV_THREAD_REF),
                              &thread_ref);
        }
        if (!cbdp_tlv_read_u32(cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_TURN_REF),
                               &turn_ref)) {
            cbdp_tlv_read_u32(cbdp_tlv_find(result_tlv, result_len, CBDP_TLV_TURN_REF),
                              &turn_ref);
        }
        if (!cbdp_tlv_read_u32(cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_TOPIC_ID),
                               &topic_id)) {
            cbdp_tlv_read_u32(cbdp_tlv_find(result_tlv, result_len, CBDP_TLV_TOPIC_ID),
                              &topic_id);
        }
        if (!copy_utf8_field(tlv, tlv_len, CBDP_TLV_THREAD_ID,
                             thread_id, sizeof(thread_id))) {
            copy_utf8_field(result_tlv, result_len, CBDP_TLV_THREAD_ID,
                            thread_id, sizeof(thread_id));
        }
        if (thread_ref == 0U && thread_id[0] == '\0') {
            // A thread.start reply that selects nothing would strand the picker
            // on its loading screen forever. Surface a real error instead of a
            // silent dead end, and drop out of the catalog state machine.
            ESP_LOGW(TAG, "thread.start returned no thread selector");
            coding_bridge_cancel_catalog();
            if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
                app_coding_catalog_abort(NULL);
                bsp_lvgl_unlock();
            }
            s_last_message_us = esp_timer_get_time();
            return;
        }
        if (thread_ref != 0U) {
            s_active_thread_ref = thread_ref;
            snprintf(s_active_thread_id, sizeof(s_active_thread_id), "%lu",
                     (unsigned long)thread_ref);
        } else {
            s_active_thread_ref = 0U;
            snprintf(s_active_thread_id, sizeof(s_active_thread_id), "%s", thread_id);
        }
        if (turn_ref != 0U) {
            s_active_turn_ref = turn_ref;
            snprintf(s_active_turn_id, sizeof(s_active_turn_id), "%lu",
                     (unsigned long)turn_ref);
        }
        if (topic_id != 0U) {
            wireless_bind_thread(s_active_thread_ref, topic_id);
        }
        // thread.start answers with the new thread selector but not its topic,
        // so reuse the exact thread.read path that normal selection uses: it
        // returns the thread object (with topic_id) plus the last message, which
        // lands the picker on the new task instead of leaving it loading.
        if (method_id == CBDP_METHOD_THREAD_START && thread_ref != 0U) {
            uint8_t read_params[64];
            size_t read_length = 0U;
            if (cbdp_tlv_write_u32(read_params, sizeof(read_params), &read_length,
                                   CBDP_TLV_THREAD_REF, thread_ref) &&
                cbdp_tlv_write_u16(read_params, sizeof(read_params), &read_length,
                                   CBDP_TLV_HISTORY_DEPTH, 2U)) {
                wireless_build_and_send_command(CBDP_METHOD_THREAD_READ,
                                                read_params, read_length);
            }
        }
    } else if (method_id == CBDP_METHOD_TURN_INTERRUPT && state <= 1U) {
        clear_active_turn();
    }
    if (state == 0U || state == 1U) {
        s_last_message_us = esp_timer_get_time();
        return;
    }
    if (state == 2U || state == 3U || state == 4U) {
        char error[192] = {0};
        copy_utf8_field(tlv, tlv_len, CBDP_TLV_ERROR_MESSAGE, error, sizeof(error));
        if (error[0] == '\0') copy_utf8_field(tlv, tlv_len, CBDP_TLV_ERROR_DETAIL,
                                              error, sizeof(error));
        if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_bridge_error(error[0] != '\0' ? error : "Bridge command failed");
            bsp_lvgl_unlock();
        }
    }
    s_last_message_us = esp_timer_get_time();
}

// The single alert entry point for every Codex completion/failure source.
// It owns the dedup window and the user's speech/tone/silent preference,
// so the bridge and the UI layer cannot drift or double-ring.
void codex_alert_play(bool failed)
{
    if (!codex_alert_claim(&s_alert_gate, esp_timer_get_time())) return;
    const app_voice_mode_t voice = app_settings_voice_mode();
    if (failed) {
        ui_sfx_play(UI_SFX_FAIL);
    } else if (voice == APP_VOICE_SPEECH) {
        ui_sfx_play(UI_SFX_DONE_SPEECH);
    } else if (voice == APP_VOICE_TONE) {
        ui_sfx_play(UI_SFX_CONFIRM);
    }
}

// Shared by both counter channels: the CBDP TCP event 0x0704 and the UDP
// home-counters frame 0x1E the Bridge pushes on the discovery port. Keeping one
// implementation means the two paths cannot drift or double-count.
static void apply_codex_counts(uint32_t running, uint32_t done, uint32_t failed) {
    ESP_LOGI(TAG, "codex_state: running=%u done=%u failed=%u",
             (unsigned)running, (unsigned)done, (unsigned)failed);
    s_background_state.connected = true;
    s_background_state.running = running;
    // ⚠ 2026-10-22: done/failed 改为**直接跟随 Bridge**, 不再取 max。
    //
    // 这三个数字是 Bridge 用全局已读状态算出来的权威值: 用户在手机/电脑上
    // 读掉一条结果, Bridge 下一次推送就会把 completed_unread 降下来。
    // 旧代码写的是 `if (done > state.done) state.done = done;` —— 只增不减,
    // 于是首页那个"已完成"角标一旦亮起就永远不消失, 就是用户说的
    // "数量老是不对"。
    //
    // 本机 codex_background_apply_result() 里的乐观 +1 只用于即时反馈和
    // 提示音去重; 它最多领先 Bridge 一个推送周期(1~2s), 之后由这里对齐。
    s_background_state.done = done;
    s_background_state.failed = failed;

    // Completion alert fallback. On CBDP the Bridge only sends this
    // aggregate counter, so compare against the previous authoritative
    // value. Replayed historical counts seed the baseline and stay silent;
    // only a real increase after that rings.
    if (!s_codex_counts_initialized) {
        s_last_codex_done_count = done;
        s_last_codex_failed_count = failed;
        s_codex_counts_initialized = true;
    } else {
        const bool rose_done = done > s_last_codex_done_count;
        const bool rose_failed = failed > s_last_codex_failed_count;
        s_last_codex_done_count = done;
        s_last_codex_failed_count = failed;
        const int64_t now_us = esp_timer_get_time();
        if (rose_done || rose_failed) {
            // Arm and wait for the count to settle. Another client reading
            // the same result makes these counters fall back within a
            // second; a genuine completion stays unread.
            s_codex_alert_armed_us = now_us;
            s_codex_alert_armed_done = done;
            s_codex_alert_armed_failed = failed;
            s_codex_alert_armed_is_failure = rose_failed;
        } else if (s_codex_alert_armed_us != 0 &&
                   done >= s_codex_alert_armed_done &&
                   failed >= s_codex_alert_armed_failed) {
            if (now_us - s_codex_alert_armed_us >= CODEX_ALERT_SETTLE_US) {
                const bool alert_failed = s_codex_alert_armed_is_failure;
                s_codex_alert_armed_us = 0;
                codex_alert_play(alert_failed);
            }
        } else if (s_codex_alert_armed_us != 0) {
            // Count fell back: it was another client reading, not new work.
            s_codex_alert_armed_us = 0;
        }
    }
    home_codex_state_changed();
    if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
        app_coding_bridge_state(running, s_background_state.done,
                                s_background_state.failed);
        bsp_lvgl_unlock();
    }
}

// Bridge -> device over UDP 8764 (message type 0x1E). The counters travel in
// the frame header TLVs, not a payload, matching the discovery frames on this
// channel. Used by the home layer, which keeps no CBDP TCP session.
bool coding_bridge_apply_udp_home_counters(const uint8_t *header_tlv, size_t header_tlv_len) {
    if (header_tlv == NULL || header_tlv_len == 0U) return false;
    uint16_t running16 = 0U, done16 = 0U, failed16 = 0U;
    if (cbdp_tlv_find(header_tlv, header_tlv_len, CBDP_TLV_RUNNING_COUNT) == NULL ||
        cbdp_tlv_find(header_tlv, header_tlv_len, CBDP_TLV_COMPLETED_UNREAD) == NULL ||
        cbdp_tlv_find(header_tlv, header_tlv_len, CBDP_TLV_FAILED_UNREAD) == NULL) {
        return false;
    }
    (void)cbdp_tlv_read_u16(cbdp_tlv_find(header_tlv, header_tlv_len, CBDP_TLV_RUNNING_COUNT), &running16);
    (void)cbdp_tlv_read_u16(cbdp_tlv_find(header_tlv, header_tlv_len, CBDP_TLV_COMPLETED_UNREAD), &done16);
    (void)cbdp_tlv_read_u16(cbdp_tlv_find(header_tlv, header_tlv_len, CBDP_TLV_FAILED_UNREAD), &failed16);
    apply_codex_counts(running16, done16, failed16);
    return true;
}

static void wireless_handle_delta(const cbdp_frame_header_t *header,
                                  const uint8_t *tlv, size_t tlv_len)
{
    cbdp_event_header_t event = {0};
    if (!cbdp_parse_event_header(tlv, tlv_len, &event)) {
        ESP_LOGW(TAG, "delta: event header parse failed");
        return;
    }
    // 协议上主机只应投递本会话已订阅的 topic。收到未订阅事件是主机的缺陷,
    // 而且这类帧会实打实占用 lwIP 内存、挤掉正在播放的电台(见 docs/
    // CodexBridge-CBDP-unsubscribed-events-flood-2026-10-20.md)。仅在异常时
    // 告警, 正常路径不打印, 避免日志本身变成噪声。
    if (!wireless_topic_subscribed(header->topic_id)) {
        ESP_LOGW(TAG, "unsubscribed delta type=0x%04X topic=0x%08X len=%u (host bug)",
                 (unsigned)event.event_type, (unsigned)header->topic_id,
                 (unsigned)event.event_payload_length);
    }
    // 13.1: the transport seq lives in the fixed header; topic_id does too.
    // topic_seq (in the event TLV) is the per-topic ordering key.
    const uint32_t topic_id = header->topic_id;
    const uint32_t frame_seq = header->seq;

    // 15.4: the host only promises to deliver topics this session subscribed
    // to, so any delta on another topic is a protocol anomaly. Applying it
    // mutates application state for content the device never asked for (the
    // unselected threads' events were driving the UI into stale DONE/ERROR
    // screens before the user picked anything).
    if (!wireless_topic_subscribed(topic_id)) {
        return;
    }

    uint32_t *expected_seq = NULL;
    uint64_t *cursor = NULL;
    uint8_t scope = CBDP_SCOPE_THREAD;
    if (topic_id == 0x50000003U) {
        expected_seq = &s_summary_scope_seq;
        cursor = &s_summary_scope_cursor;
        scope = CBDP_SCOPE_SUMMARY;
    } else {
        expected_seq = &s_thread_scope_seq;
        cursor = &s_thread_scope_cursor;
        scope = CBDP_SCOPE_THREAD;
    }

    const uint32_t topic_seq = (uint32_t)event.topic_seq;
    if (topic_seq != 0U && *expected_seq != 0U && topic_seq < *expected_seq) {
        // Duplicate/older topic event: drop it, but still acknowledge the
        // transport frame so the host can retire it.
        if (frame_seq != 0U && frame_seq != s_last_acked_seq) {
            s_last_acked_seq = frame_seq;
            wireless_ack(topic_id, scope, frame_seq, *cursor);
        }
        return;
    }
    if (topic_seq != 0U && *expected_seq != 0U && topic_seq > *expected_seq) {
        uint8_t resync[CBDP_MAX_FRAME_BYTES];
        size_t resync_length = 0U;
        if (cbdp_build_resync(resync, sizeof(resync), &resync_length,
                              ++s_wireless_request_id, 0U)) {
            wireless_bridge_send_frame(resync, resync_length);
        }
        // Keep processing the event we do have; the resync fills the gap.
    }
    if (topic_seq != 0U) *expected_seq = topic_seq + 1U;
    if (event.global_seq != 0U) *cursor = event.global_seq;

    s_last_message_us = esp_timer_get_time();

    if (event.event_type == CBDP_EVENT_MESSAGE_READ) {
        char message_id[CODEX_BRIDGE_ID_MAX] = {0};
        uint32_t message_ref = 0U;
        (void)copy_utf8_field(event.event_payload, event.event_payload_length,
                              CBDP_TLV_MESSAGE_ID, message_id, sizeof(message_id));
        cbdp_tlv_read_u32(cbdp_tlv_find(event.event_payload, event.event_payload_length,
                                        CBDP_TLV_MESSAGE_REF),
                          &message_ref);
        if (message_id[0] == '\0' && message_ref != 0U) {
            snprintf(message_id, sizeof(message_id), "%lu", (unsigned long)message_ref);
        }
        if (message_id[0] != '\0') {
            background_read_state_changed(message_id);
            if (ui_budget_available() && coding_bridge_foreground_ui_active() &&
                bsp_lvgl_lock(500)) {
                app_coding_apply_read_state(message_id, 1U);
                bsp_lvgl_unlock();
            }
        }
        return;
    }

    if (event.event_type == CBDP_EVENT_CODEX_STATE) {
        // 首页机器人状态与"运行中/完成/失败"数量。主机在 summary 主题上推送
        // 聚合计数(事件 0x0704, tag 0x00BC/0x00BD/0x00BE)。处理器以前没有这个
        // 分支, 事件被静默丢弃, 于是 Codex 明明在跑, 首页仍停在睡眠/idle 且
        // 计数恒为 0。
        uint16_t running16 = 0U, done16 = 0U, failed16 = 0U;
        (void)cbdp_tlv_read_u16(cbdp_tlv_find(event.event_payload,
                                              event.event_payload_length,
                                              CBDP_TLV_RUNNING_COUNT), &running16);
        (void)cbdp_tlv_read_u16(cbdp_tlv_find(event.event_payload,
                                              event.event_payload_length,
                                              CBDP_TLV_COMPLETED_UNREAD), &done16);
        (void)cbdp_tlv_read_u16(cbdp_tlv_find(event.event_payload,
                                              event.event_payload_length,
                                              CBDP_TLV_FAILED_UNREAD), &failed16);
        const uint32_t running = running16;
        const uint32_t done = done16;
        const uint32_t failed = failed16;
        apply_codex_counts(running, done, failed);
        return;
    }

    if (event.event_type == CBDP_EVENT_TURN_COMPLETED ||
        event.event_type == CBDP_EVENT_TURN_FAILED ||
        event.event_type == CBDP_EVENT_TURN_INTERRUPTED) {
        char body[CODING_STATUS_MAX] = {0};
        char thread_id[64] = {0};
        char turn_id[64] = {0};
        if (!copy_utf8_preview(event.event_payload, event.event_payload_length,
                               CBDP_TLV_BODY, body, sizeof(body))) {
            copy_utf8_preview(event.event_payload, event.event_payload_length,
                              CBDP_TLV_SUMMARY, body, sizeof(body));
        }
        copy_utf8_field(event.event_payload, event.event_payload_length,
                        CBDP_TLV_THREAD_ID, thread_id, sizeof(thread_id));
        copy_utf8_field(event.event_payload, event.event_payload_length,
                        CBDP_TLV_TURN_ID, turn_id, sizeof(turn_id));
        uint32_t thread_ref = 0U;
        uint32_t turn_ref = 0U;
        cbdp_tlv_read_u32(cbdp_tlv_find(event.event_payload,
                                        event.event_payload_length,
                                        CBDP_TLV_THREAD_REF), &thread_ref);
        cbdp_tlv_read_u32(cbdp_tlv_find(event.event_payload,
                                        event.event_payload_length,
                                        CBDP_TLV_TURN_REF), &turn_ref);
        if (thread_id[0] == '\0' && thread_ref != 0U) {
            snprintf(thread_id, sizeof(thread_id), "%lu", (unsigned long)thread_ref);
            s_active_thread_ref = thread_ref;
        }
        if (turn_id[0] == '\0' && turn_ref != 0U) {
            snprintf(turn_id, sizeof(turn_id), "%lu", (unsigned long)turn_ref);
        }
        if (event.event_type == CBDP_EVENT_TURN_COMPLETED ||
            event.event_type == CBDP_EVENT_TURN_FAILED ||
            event.event_type == CBDP_EVENT_TURN_INTERRUPTED) {
            if ((turn_ref != 0U && turn_ref == s_active_turn_ref) ||
                (turn_ref == 0U && turn_id[0] != '\0' &&
                 strcmp(turn_id, s_active_turn_id) == 0)) {
                clear_active_turn();
            }
        }
        // Terminal turn events drive the home pet/badge and the completion
        // alert even while the full Codex page is closed. Previously this ran
        // only for the foreground UI, so a task finishing in the background
        // produced no sound and never lit the home indicator.
        const bool failed = event.event_type == CBDP_EVENT_TURN_FAILED;
        const char *result_kind = failed ? "failed" : "completed";
        char identity[CODEX_BRIDGE_ID_MAX + 16] = {0};
        if (turn_id[0] != '\0') {
            snprintf(identity, sizeof(identity), "turn:%s", turn_id);
        } else if (thread_id[0] != '\0') {
            snprintf(identity, sizeof(identity), "thread:%s", thread_id);
        } else {
            snprintf(identity, sizeof(identity), "seq:%lu",
                     (unsigned long)event.global_seq);
        }
        if (codex_background_apply_notice(&s_background_state, "result",
                                          identity, result_kind, 0U)) {
            home_codex_state_changed();
            // Alert only for a newly delivered result, not for a replay, and
            // honour the user's notification mode exactly like the in-page
            // path does (speech / tone / silent).
            codex_alert_play(failed);
        }
        if (body[0] != '\0' && ui_budget_available() &&
            coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_result_identity(thread_id, turn_id);
            if (failed) {
                app_coding_bridge_error(body);
            } else {
                app_coding_bridge_done(body);
            }
            bsp_lvgl_unlock();
        }
        return;
    }

    if (event.event_type == CBDP_EVENT_MESSAGE_APPENDED ||
        event.event_type == CBDP_EVENT_MESSAGE_UPDATED) {
        // A message event is NOT a turn result. The Bridge sends the initial
        // task/commentary row (kind=2, status=1) as soon as a turn starts; the
        // old code showed every such row as "任务完成" before the model had
        // done any work. Terminal completion is delivered by TURN_COMPLETED /
        // TURN_FAILED above, so here we only refresh the running status.
        uint8_t kind = 0U;
        uint16_t status = 0U;
        (void)cbdp_tlv_read_u8(cbdp_tlv_find(event.event_payload,
                                             event.event_payload_length,
                                             CBDP_TLV_KIND), &kind);
        (void)cbdp_tlv_read_u16(cbdp_tlv_find(event.event_payload,
                                              event.event_payload_length,
                                              CBDP_TLV_STATUS), &status);
        if (kind != 2U || status != 1U) {
            return;
        }
        char body[CODING_STATUS_MAX] = {0};
        char thread_id[64] = {0};
        char message_id[CODEX_BRIDGE_ID_MAX] = {0};
        uint32_t thread_ref = 0U;
        uint32_t message_ref = 0U;
        if (!copy_utf8_preview(event.event_payload, event.event_payload_length,
                               CBDP_TLV_BODY, body, sizeof(body))) {
            copy_utf8_preview(event.event_payload, event.event_payload_length,
                              CBDP_TLV_SUMMARY, body, sizeof(body));
        }
        copy_utf8_field(event.event_payload, event.event_payload_length,
                        CBDP_TLV_THREAD_ID, thread_id, sizeof(thread_id));
        copy_utf8_field(event.event_payload, event.event_payload_length,
                        CBDP_TLV_MESSAGE_ID, message_id, sizeof(message_id));
        cbdp_tlv_read_u32(cbdp_tlv_find(event.event_payload,
                                        event.event_payload_length,
                                        CBDP_TLV_THREAD_REF), &thread_ref);
        cbdp_tlv_read_u32(cbdp_tlv_find(event.event_payload,
                                        event.event_payload_length,
                                        CBDP_TLV_MESSAGE_REF), &message_ref);
        if (thread_id[0] == '\0' && thread_ref != 0U) {
            snprintf(thread_id, sizeof(thread_id), "%lu", (unsigned long)thread_ref);
        }
        if (message_id[0] == '\0' && message_ref != 0U) {
            snprintf(message_id, sizeof(message_id), "%lu", (unsigned long)message_ref);
        }
        if (body[0] != '\0' && ui_budget_available() &&
            coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_remote_task(thread_id, body, message_id);
            bsp_lvgl_unlock();
        }
    }
    // 15.8: ACK the transport seq with the durable cursor. Only ACK when the
    // transport sequence advances, otherwise replayed frames cause an ACK storm.
    if (frame_seq != 0U && frame_seq != s_last_acked_seq) {
        s_last_acked_seq = frame_seq;
        wireless_ack(topic_id, scope, frame_seq, *cursor);
    }
}

static void wireless_handle_approval_request(const uint8_t *tlv, size_t tlv_len)
{
    char prompt[192] = {0};
    char detail[192] = {0};
    uint32_t approval_ref = 0U;
    uint8_t risk = 0U;
    cbdp_tlv_read_u32(cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_APPROVAL_REF),
                      &approval_ref);
    cbdp_tlv_read_u8(cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_RISK), &risk);
    copy_utf8_field(tlv, tlv_len, CBDP_TLV_PROMPT, prompt, sizeof(prompt));
    copy_utf8_field(tlv, tlv_len, CBDP_TLV_APPROVAL_DETAIL, detail, sizeof(detail));
    if (prompt[0] == '\0') {
        copy_utf8_field(tlv, tlv_len, CBDP_TLV_TITLE, prompt, sizeof(prompt));
    }
    if (detail[0] == '\0') {
        snprintf(detail, sizeof(detail), "风险等级：%u", (unsigned)risk);
    }
    s_approval_ref = approval_ref;
    s_last_message_us = esp_timer_get_time();
    if (approval_ref != 0U && coding_bridge_foreground_ui_active() &&
        bsp_lvgl_lock(500)) {
        app_coding_bridge_approval(prompt, detail);
        bsp_lvgl_unlock();
    }
}

// 19.5: Bridge must report what went wrong instead of silently dropping an
// uplink transaction. Surface the code (and any host detail) to the user so a
// missing active thread or a disabled capability is actionable.
// 15.4: subscribe one topic at one scope. Message events live on the thread's
// own topic (0x1xxxxxxx), so after selecting a thread the device must subscribe
// to that topic as well; the summary topic alone never carries them.
static void wireless_subscribe(uint32_t topic_id, uint8_t scope, uint64_t cursor)
{
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    ESP_LOGI(TAG, "SUBSCRIBE topic=0x%08lx scope=%u cursor=%llu",
             (unsigned long)topic_id, (unsigned)scope, (unsigned long long)cursor);
    // A subscription is per-session state, and re-issuing one for a topic we
    // already hold is a duplicate request the host rejects. thread.read and the
    // list response can both bind the same thread, so guard here rather than at
    // each call site.
    if (wireless_is_subscribed(topic_id, scope)) return;
    if (s_sub_count < MAX_SUBSCRIPTIONS) {
        s_sub_topic[s_sub_count] = topic_id;
        s_sub_scope[s_sub_count] = scope;
        ++s_sub_count;
    }
    if (cbdp_build_subscribe(frame, sizeof(frame), &length, ++s_wireless_request_id,
                             topic_id, scope, CBDP_QOS_STATE, cursor)) {
        wireless_bridge_send_frame(frame, length);
    }
}

// 15.8: ACK carries the cursor the device has durably applied so the Bridge can
// advance its checkpoint and resume from it after a reconnect.
// Section 15.8/9.4 allow the host to coalesce delivery (ack_delay_ms), and a
// subscribe that replays a topic's backlog arrives as a burst. Acknowledging
// every replayed frame one-for-one doubled the traffic and saturated the socket
// until the connection dropped mid-recording, so only the newest cursor is
// acknowledged, at most once per ACK_COALESCE_US.
static void wireless_ack_send(uint32_t topic_id, uint8_t scope, uint32_t ack_seq,
                              uint64_t cursor)
{
    uint8_t frame[CBDP_MAX_FRAME_BYTES];
    size_t length = 0U;
    if (cbdp_build_ack(frame, sizeof(frame), &length, ack_seq, topic_id, scope,
                       cursor)) {
        wireless_bridge_send_frame(frame, length);
    }
}

// Flush a coalesced ACK once the quiet window elapsed. Driven by bridge_tick.
static void wireless_ack_flush(void)
{
    if (!s_ack_pending) return;
    if (esp_timer_get_time() - s_last_ack_us < ACK_COALESCE_US) return;
    s_ack_pending = false;
    s_last_ack_us = esp_timer_get_time();
    wireless_ack_send(s_pending_ack_topic, s_pending_ack_scope,
                      s_pending_ack_seq, s_pending_ack_cursor);
}

static void wireless_ack(uint32_t topic_id, uint8_t scope, uint32_t ack_seq,
                         uint64_t cursor)
{
    // 15.8: only acknowledge topics this device actually subscribed to; the
    // host rejects ACKs for unknown subscriptions with UNSUPPORTED_FEATURE.
    if (!wireless_is_subscribed(topic_id, scope)) {
        return;
    }
    s_pending_ack_topic = topic_id;
    s_pending_ack_scope = scope;
    s_pending_ack_seq = ack_seq;
    s_pending_ack_cursor = cursor;
    s_ack_pending = true;
    if (esp_timer_get_time() - s_last_ack_us >= ACK_COALESCE_US) {
        wireless_ack_flush();
    }
}

static void wireless_handle_error(const cbdp_frame_header_t *header,
                                  const uint8_t *tlv, size_t tlv_len)
{
    char detail[192] = {0};
    copy_utf8_field(tlv, tlv_len, CBDP_TLV_ERROR_MESSAGE, detail, sizeof(detail));
    if (detail[0] == '\0') {
        copy_utf8_field(tlv, tlv_len, CBDP_TLV_ERROR_DETAIL, detail, sizeof(detail));
    }
    const uint16_t code = header != NULL ? header->code : CBDP_ERR_NONE;
    const char *message = detail[0] != '\0' ? detail : cbdp_error_text(code);
    ESP_LOGW(TAG, "bridge error code=0x%04X: %s", (unsigned)code, message);

    s_last_message_us = esp_timer_get_time();

    // 8.4 catalog requests: a failed projects.list/threads.list used to leave
    // the picker on "正在读取真实 Codex 项目…" forever, because only a RESULT
    // drove the state machine. Match the request_id in the ERROR frame and
    // surface a real, actionable message instead of an endless spinner.
    uint32_t error_request_id = 0U;
    (void)cbdp_tlv_read_u32(cbdp_tlv_find(tlv, tlv_len, CBDP_TLV_REQUEST_ID),
                            &error_request_id);
    if (s_catalog_pending_request_id != 0U &&
        (error_request_id == 0U ||
         error_request_id == s_catalog_pending_request_id)) {
        char catalog_error[96];
        snprintf(catalog_error, sizeof(catalog_error), "桥接错误(%.48s)", message);
        s_catalog_pending_request_id = 0U;
        s_catalog_pending_since_us = 0;
        coding_bridge_cancel_catalog();
        if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_catalog_abort(catalog_error);
            bsp_lvgl_unlock();
        }
        return;
    }

    // 21.3: only authentication errors end the session. A peer ERROR such as
    // INVALID_FRAME or INVALID_STATE must not, and per 21.3 the device must not
    // answer an ERROR with another ERROR.
    if (code == CBDP_ERR_UNAUTHORIZED || code == CBDP_ERR_AUTH_EXPIRED ||
        code == CBDP_ERR_TOKEN_ROTATED) {
        s_capture_requested = false;
        s_long_text_target = LONG_TEXT_NONE;
        wireless_bridge_disconnect();
        if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_bridge_connected(false);
            bsp_lvgl_unlock();
        }
        return;
    }
    // A peer ERROR concerns the request it answers; it is not a reason to abort
    // an in-flight recording. Clearing the capture flag here cut multi-second
    // recordings down to the moment an unrelated command (for example a
    // thread.read the host could not serve) failed. Per 19.4/19.5 an AUDIO_END
    // that the host cannot transcribe is reported on that transaction instead.
    if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
        app_coding_bridge_error(message);
        bsp_lvgl_unlock();
    }
}

static void process_wireless_frame(const cbdp_frame_header_t *header,
                                   const uint8_t *tlv, size_t tlv_len,
                                   const uint8_t *payload, size_t payload_len)
{
    (void)payload;
    (void)payload_len;
    if (!s_wireless_enabled || header == NULL) return;
    switch (header->message_type) {
    case CBDP_MT_AUTH_OK:
        if (cbdp_parse_auth_ok(tlv, tlv_len, &s_session)) {
            // cbdp_parse_auth_ok() overwrites the session with AUTH_OK fields
            // only, which zeroes the policy negotiated in WELCOME. Re-read the
            // transport's copy so max_audio_bytes/max_chunk_bytes are honoured;
            // without it the device ignored the host cap and uploaded past the
            // limit, and the Bridge reset the socket mid-recording.
            cbdp_session_t transport = {0};
            if (wireless_bridge_session_snapshot(&transport) &&
                transport.ready) {
                s_session.max_audio_bytes = transport.max_audio_bytes;
                s_session.max_chunk_bytes = transport.max_chunk_bytes;
                s_session.max_frame_bytes = transport.max_frame_bytes;
                s_session.heartbeat_seconds = transport.heartbeat_seconds;
                s_session.queue_depth = transport.queue_depth;
                s_session.supported_caps = transport.supported_caps;
                s_session.ready = true;
            }
            wireless_reset_subscriptions();
            wireless_subscribe(0x50000003U, CBDP_SCOPE_SUMMARY, 0U);
            // 15.4: re-attach the selected thread topic so message events keep
            // flowing across a transport reconnect without another UI action.
            if (s_thread_topic_id != 0U) {
                wireless_subscribe(s_thread_topic_id, CBDP_SCOPE_THREAD, 0U);
            }
            wireless_set_connected(true);
        }
        break;
    case CBDP_MT_CAPABILITY_UPDATE:
        if (cbdp_parse_capability_update(tlv, tlv_len, &s_session)) {
            s_last_message_us = esp_timer_get_time();
            s_bridge_online = true;
            wireless_set_connected(true);
        }
        break;
    case CBDP_MT_RESULT:
        wireless_handle_result(header, tlv, tlv_len);
        break;
    case CBDP_MT_DELTA:
        wireless_handle_delta(header, tlv, tlv_len);
        break;
    case CBDP_MT_APPROVAL_REQUEST:
        wireless_handle_approval_request(tlv, tlv_len);
        break;
    case CBDP_MT_ERROR:
        wireless_handle_error(header, tlv, tlv_len);
        break;
    default:
        break;
    }
}
static void process_usb_line(const char *line)
{
    process_line(line, true);
}

// Host replied to nothing: a projects/threads list that never produces a
// RESULT (dropped frame, older bridge, or a session that reconnected mid
// request) left the picker loading indefinitely. Time it out and say so.
#define CATALOG_REPLY_TIMEOUT_US (20LL * 1000LL * 1000LL)

static void bridge_tick(void)
{
    if (!s_wireless_enabled) return;
    wireless_ack_flush();

    if (s_catalog_pending_request_id != 0U && s_catalog_pending_since_us != 0 &&
        esp_timer_get_time() - s_catalog_pending_since_us >
            CATALOG_REPLY_TIMEOUT_US) {
        s_catalog_pending_request_id = 0U;
        s_catalog_pending_since_us = 0;
        coding_bridge_cancel_catalog();
        if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_catalog_abort("连接超时，未取到任务列表");
            bsp_lvgl_unlock();
        }
    }
    // Section 12 keeps heartbeat liveness in the transport: the socket layer
    // sends PING on idle links and tears the session down after
    // 3 x heartbeat_seconds without any inbound frame. The application layer
    // therefore only reacts to a session the transport has actually lost;
    // using a fixed application-silence timer here disconnected healthy idle
    // sessions and forced a full re-handshake every few seconds.
    if (s_connected && !wireless_bridge_session_active()) {
        s_capture_requested = false;
        s_long_text_target = LONG_TEXT_NONE;
        s_connected = false;
        s_bridge_online = false;
        codex_background_reset(&s_background_state);
        home_codex_state_changed();
        wireless_bridge_disconnect();
        if (coding_bridge_foreground_ui_active() && bsp_lvgl_lock(500)) {
            app_coding_bridge_connected(false);
            bsp_lvgl_unlock();
        }
    }
}

esp_err_t coding_bridge_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .tx_buffer_size = 2048,
            .rx_buffer_size = 2048,
        };
        esp_err_t result = usb_serial_jtag_driver_install(&config);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "USB driver install failed: %s", esp_err_to_name(result));
            return result;
        }
    }
    usb_serial_jtag_vfs_use_driver();

    s_ready = true;
    fap_set_json_line_hook(process_usb_line);
    fap_set_tick_hook(bridge_tick);
    codex_bridge_protocol_init();
    seed_wireless_counters();
    esp_err_t wireless = wireless_bridge_init(process_wireless_frame,
                                              send_wifi_status);
    if (wireless != ESP_OK) {
        ESP_LOGW(TAG, "wireless bridge unavailable: %s",
                 esp_err_to_name(wireless));
    } else {
        s_wireless_started = true;
        s_wireless_enabled = true;
        s_bridge_online = false;
    }
    ESP_LOGI(TAG, "USB audio bridge ready, wireless=%d", wireless == ESP_OK);
    return ESP_OK;
}

esp_err_t coding_bridge_init_usb_only(void)
{
    if (s_ready) return ESP_OK;
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .tx_buffer_size = 2048,
            .rx_buffer_size = 2048,
        };
        esp_err_t result = usb_serial_jtag_driver_install(&config);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "USB driver install failed: %s", esp_err_to_name(result));
            return result;
        }
    }
    usb_serial_jtag_vfs_use_driver();
    s_ready = true;
    fap_set_json_line_hook(process_usb_line);
    fap_set_tick_hook(bridge_tick);
    ESP_LOGI(TAG, "USB audio bridge ready");
    return ESP_OK;
}

bool coding_bridge_wireless_started(void)
{
    return s_wireless_started;
}

esp_err_t coding_bridge_start_wireless(void)
{
    if (s_wireless_started) {
        if (s_wireless_enabled) return ESP_OK;
        esp_err_t resumed = wireless_bridge_init(process_wireless_frame,
                                                 send_wifi_status);
        if (resumed != ESP_OK) {
            ESP_LOGW(TAG, "wireless bridge restart failed: %s",
                     esp_err_to_name(resumed));
            return resumed;
        }
        s_wireless_enabled = true;
        /* 若传输层已经在运行且会话仍 ready, 直接恢复投递, 不再强制 disconnect()。
         * 之前这里无条件断线, 会把一个本来健康的会话拆掉, 回首页还得等主机
         * 下一次发现节拍(实测 11 秒), 用户感知就是切回首页必卡。 */
        if (wireless_bridge_session_ready()) {
            s_bridge_online = true;

            home_codex_state_changed();
            return ESP_OK;
        }
        s_bridge_online = false;
        s_connected = false;
        codex_background_reset(&s_background_state);
        home_codex_state_changed();
        /* 会话真的断了才需要重连。 */
        wireless_bridge_disconnect();
        return ESP_OK;
    }
    esp_err_t ready = coding_bridge_init_usb_only();
    if (ready != ESP_OK) return ready;
    codex_bridge_protocol_init();
    seed_wireless_counters();
    esp_err_t wireless = wireless_bridge_init(process_wireless_frame,
                                              send_wifi_status);
    if (wireless != ESP_OK) {
        ESP_LOGW(TAG, "wireless bridge unavailable: %s",
                 esp_err_to_name(wireless));
        return wireless;
    }
    s_wireless_started = true;
    s_wireless_enabled = true;
    s_bridge_online = false;
    return ESP_OK;
}

void coding_bridge_stop_wireless(void)
{
    /* 完整收音机/小智前台期间释放桥接资源。
     *
     * 这里必须真正停掉传输任务: 它持有 12KB 任务栈(借自小智麦克风区)、
     * 监听 socket 与 TCP 接收缓冲。真机实测"软暂停(只停投递)"会让收音机
     * 进入后 dma 只剩 ~568 字节, 切台直接 Open failed —— 音频 socket 拿不到
     * 收包内存, 得不偿失。
     *
     * 回首页的重连成本已通过另一条路径优化: 恢复时若会话仍然 ready 就直接
     * 复用, 不再强制 disconnect()。 */
    s_wireless_enabled = false;
    wireless_bridge_stop();
    s_wireless_started = false;
    s_allow_foreground_ui = false;
    s_bridge_online = false;
    s_connected = false;
    codex_background_reset(&s_background_state);
    s_background_codex_state = APP_CODEX_OFFLINE;
    app_home_set_codex_counts(0U, 0U, 0U);
    app_home_set_codex_state(APP_CODEX_OFFLINE);
}

bool coding_bridge_send_read_ack(const char *message_id)
{
    if (message_id == NULL || message_id[0] == '\0' || !s_connected ||
        !wireless_bridge_session_ready()) {
        return false;
    }
    uint8_t params[96];
    size_t params_length = 0U;
    const unsigned long parsed = strtoul(message_id, NULL, 10);
    if (parsed != 0UL && parsed <= UINT32_MAX) {
        if (!cbdp_tlv_write_u32(params, sizeof(params), &params_length,
                                CBDP_TLV_MESSAGE_REF, (uint32_t)parsed)) {
            return false;
        }
    } else if (!cbdp_tlv_write_bytes(params, sizeof(params), &params_length,
                                     CBDP_TLV_MESSAGE_ID, message_id,
                                     strlen(message_id))) {
        return false;
    }
    if (s_active_thread_ref != 0U &&
        !cbdp_tlv_write_u32(params, sizeof(params), &params_length,
                            CBDP_TLV_THREAD_REF, s_active_thread_ref)) {
        return false;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms < 0) now_ms = 0;
    if (!cbdp_tlv_write_i64(params, sizeof(params), &params_length,
                            CBDP_TLV_READ_AT_MS, now_ms) ||
        !wireless_build_and_send_command(CBDP_METHOD_MESSAGE_READ_UPDATE,
                                         params, params_length)) {
        return false;
    }
    /* The host broadcasts read_state to other devices, not back to the
     * sender. Clear this device's own unread badge as soon as its ack lands,
     * otherwise leaving the full Codex page would resurrect the stale count. */
    if (codex_background_mark_read(&s_background_state, message_id)) {
        home_codex_state_changed();
    }
    return true;
}
