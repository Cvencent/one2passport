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
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "fap_screenshot.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "wireless_bridge.h"

#define BRIDGE_LINE_MAX 768
#define BRIDGE_TIMEOUT_US (12LL * 1000LL * 1000LL)
#define CAPTURE_SAMPLE_RATE 16000
#define CAPTURE_CHUNK_SAMPLES 160
#define CAPTURE_CHUNK_BYTES (CAPTURE_CHUNK_SAMPLES * sizeof(int16_t))
#define CAPTURE_BASE64_BYTES (4 * ((CAPTURE_CHUNK_BYTES + 2) / 3))
#define CAPTURE_TASK_STACK 4096
#define SPEECH_CHUNK_BYTES 384

static const char *TAG = "coding_bridge";
extern bool radio_is_playing_c(void);
static bool s_ready;
static bool s_connected;
static int64_t s_last_message_us;
static volatile bool s_capture_requested;
static volatile bool s_capture_cancelled;
static bool s_capture_wireless;
static uint32_t s_catalog_request_id;
static TaskHandle_t s_capture_task;
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_ota_partition;
static size_t s_ota_expected;
static size_t s_ota_written;
static int s_ota_progress;
static bool s_ota_active;

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
        // One event must use one transport. Mirroring the 50 audio chunks per
        // second to an attached but unopened USB serial port fills its small TX
        // ring buffer and can block each chunk for 250 ms, truncating a spoken
        // command to a fraction of its real duration. Prefer the authenticated
        // Wi-Fi bridge and use USB only as a fallback.
        bool sent_wirelessly = wireless_bridge_write(wire, length + 1);
        sent = sent_wirelessly;
        if (!sent_wirelessly && usb_serial_jtag_is_connected()) {
            sent = fap_usb_write(wire, length + 1);
        }
        free(wire);
    }
    cJSON_free(json);
    return sent;
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

static void send_simple(const char *type)
{
    send_json(event_create(type));
}

bool coding_bridge_is_connected(void)
{
    return s_connected;
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

static bool send_record_header(void)
{
    static const char wire[] =
        "{\"type\":\"record_start\",\"sample_rate\":16000,\"bits\":16,\"channels\":1}\n";
    s_capture_wireless = wireless_bridge_write(wire, sizeof(wire) - 1);
    return s_capture_wireless ||
           (usb_serial_jtag_is_connected() &&
            fap_usb_write(wire, sizeof(wire) - 1));
}

static bool send_capture_packet(const char *wire, size_t length)
{
    /* Keep a recording on the transport that accepted its header. */
    return s_capture_wireless ? wireless_bridge_write(wire, length)
                              : fap_usb_write(wire, length);
}

static bool send_audio_chunk(const int16_t *samples, size_t sample_count)
{
    const size_t byte_count = sample_count * sizeof(samples[0]);
    static const char prefix[] = "{\"type\":\"audio_chunk\",\"data\":\"";
    char wire[CAPTURE_BASE64_BYTES + sizeof(prefix) + 4];
    const size_t prefix_length = sizeof(prefix) - 1;
    memcpy(wire, prefix, prefix_length);
    size_t encoded_length = 0;
    /* Base64 contains no JSON escape characters. Encode straight into the
     * packet to avoid three simultaneous heap buffers during microphone I/O. */
    if (mbedtls_base64_encode((unsigned char *)wire + prefix_length,
                              CAPTURE_BASE64_BYTES + 1, &encoded_length,
                              (const unsigned char *)samples, byte_count) != 0) {
        send_capture_error("音频编码失败");
        s_capture_requested = false;
        return false;
    }
    memcpy(wire + prefix_length + encoded_length, "\"}\n", 3);
    size_t length = prefix_length + encoded_length + 3;
    return send_capture_packet(wire, length);
}

static bool send_record_footer(size_t byte_count)
{
    char wire[64];
    int length = snprintf(wire, sizeof(wire),
                          "{\"type\":\"record_stop\",\"bytes\":%u}\n",
                          (unsigned)byte_count);
    return send_capture_packet(wire, (size_t)length);
}

static void capture_task(void *argument)
{
    (void)argument;
    int16_t samples[CAPTURE_CHUNK_SAMPLES];
    size_t total_bytes = 0;
    unsigned level_tick = 0;
    bool capture_ok = true;

    if (bsp_audio_set_format(CAPTURE_SAMPLE_RATE, 16, 1) != ESP_OK) {
        send_capture_error("麦克风格式设置失败");
        s_capture_requested = false;
        s_capture_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    capture_ok = send_record_header();
    while (capture_ok && s_capture_requested) {
        if (bsp_audio_read(samples, sizeof(samples)) != ESP_OK) {
            send_capture_error("麦克风读取失败");
            s_capture_requested = false;
            capture_ok = false;
            break;
        }
        if (!send_audio_chunk(samples, CAPTURE_CHUNK_SAMPLES)) {
            capture_ok = false;
            break;
        }
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
    if (capture_ok && !s_capture_cancelled) {
        capture_ok = send_record_footer(total_bytes);
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
    send_simple("record_cancel");
}

bool coding_bridge_send_voice(const char *text)
{
    cJSON *root = event_create("voice_command");
    if (root != NULL && cJSON_AddStringToObject(root, "text", text != NULL ? text : "") == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }
    return send_json(root);
}

static const char *decision_wire_name(coding_decision_t decision)
{
    static const char *const names[] = {
        "approve_once", "approve_session", "deny", "cancel_task",
    };
    return decision >= CODING_DECISION_APPROVE_ONCE && decision < CODING_DECISION_COUNT
               ? names[decision]
               : "deny";
}

void coding_bridge_send_approval(coding_decision_t decision)
{
    cJSON *root = event_create("approval_decision");
    if (root != NULL &&
        cJSON_AddStringToObject(root, "decision", decision_wire_name(decision)) == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }
    send_json(root);
}

void coding_bridge_send_cancel(void)
{
    send_simple("cancel_task");
}

void coding_bridge_request_projects(void)
{
    cJSON *root = event_create("codex_catalog_request");
    if (root) cJSON_AddNumberToObject(root, "request_id", ++s_catalog_request_id);
    send_json(root);
}

void coding_bridge_cancel_catalog(void)
{
    ++s_catalog_request_id;
    send_simple("codex_catalog_cancel");
}

void coding_bridge_confirm_selection(void)
{
    cJSON *root = event_create("codex_selection_confirm");
    if (root) cJSON_AddNumberToObject(root, "request_id", s_catalog_request_id);
    send_json(root);
}

static void send_catalog_selection(const char *type, const char *id)
{
    cJSON *root = event_create(type);
    if (root) cJSON_AddNumberToObject(root, "request_id", ++s_catalog_request_id);
    if (root != NULL &&
        cJSON_AddStringToObject(root, "id", id != NULL ? id : "") == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }
    send_json(root);
}

void coding_bridge_select_project(const char *id)
{
    send_catalog_selection("codex_project_select", id);
}

void coding_bridge_select_thread(const char *id)
{
    send_catalog_selection("codex_thread_select", id);
}

void coding_bridge_create_thread(void)
{
    send_catalog_selection("codex_thread_new", "");
}

void coding_bridge_request_review(void)
{
    send_simple("codex_review_request");
}

bool coding_bridge_result_seen(const char *thread_id, const char *turn_id)
{
    cJSON *root = event_create("codex_result_seen");
    if (root) {
        cJSON_AddStringToObject(root, "thread_id", thread_id);
        cJSON_AddStringToObject(root, "turn_id", turn_id);
    }
    return send_json(root);
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

static void process_wireless_line(const char *line)
{
    process_line(line, false);
}

static void process_usb_line(const char *line)
{
    process_line(line, true);
}

static void bridge_tick(void)
{
    if (s_connected &&
            esp_timer_get_time() - s_last_message_us > BRIDGE_TIMEOUT_US) {
            s_capture_requested = false;
            s_long_text_target = LONG_TEXT_NONE;
            if (bsp_lvgl_lock(500)) {
                s_connected = false;
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
    esp_err_t wireless = wireless_bridge_init(process_wireless_line,
                                              send_wifi_status);
    if (wireless != ESP_OK) {
        ESP_LOGW(TAG, "wireless bridge unavailable: %s",
                 esp_err_to_name(wireless));
    }
    ESP_LOGI(TAG, "USB audio bridge ready, wireless=%d", wireless == ESP_OK);
    return ESP_OK;
}
