// main/wireless_bridge.c
// CBDP v1 LAN transport and session handshake.
#include "wireless_bridge.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "codex_bridge_protocol.h"
#include "coding_bridge.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "nvs.h"
#include "nvs_flash.h"

#define BRIDGE_PORT 8765
#define DISCOVERY_PORT 8764
#define NETWORK_FRAME_MAX CBDP_MAX_FRAME_BYTES
// The receive path nests the TCP/UDP scratch buffers, a subscribe frame, and
// the event body copy before the parser returns. Keep enough headroom for the
// worst-case CBDP message instead of tuning to the handshake-only path.
#define NETWORK_TASK_STACK 12288
#define TOKEN_MAX 64
#define TCP_TEXT_BUFFER_MAX 8192
#define FRAGMENT_TIMEOUT_MS 30000U
// Section 12 default timeouts: a peer that never completes the handshake, and
// a heartbeat interval used when WELCOME did not carry heartbeat_seconds.
#define HANDSHAKE_TIMEOUT_US (10LL * 1000LL * 1000LL)
#define HEARTBEAT_FALLBACK_SECONDS 15U

static const char *TAG = "wireless_bridge";
static const char *NVS_NAMESPACE = "folo_bridge";
static const char *NVS_TOKEN_KEY = "token";

static wireless_bridge_frame_cb_t s_frame_cb;
static wireless_bridge_status_cb_t s_status_cb;
static wireless_bridge_wifi_configure_t s_wifi_configure_cb;
static SemaphoreHandle_t s_client_lock;
static int s_client_fd = -1;
static bool s_client_authenticated;
// Lock-free mirror of the authenticated session state so tick hooks and send
// paths can observe liveness without contending on the socket lock.
static volatile bool s_session_ready;
static bool s_wifi_connected;
static char s_token[TOKEN_MAX + 1];
static char s_ip[16];
static cbdp_session_t s_session;
static cbdp_fragment_assembly_t s_fragment_assembly;
static uint32_t s_fragment_started_ms;
static uint8_t s_client_nonce[16];
static uint32_t s_request_id = 1U;
// Section 12 keeps heartbeat liveness at the transport layer: record the
// arrival time of every valid inbound frame so the session can be kept alive
// without the application layer owning the socket or its timers.
static int64_t s_last_rx_us;
static int64_t s_last_ping_us;
static int64_t s_client_connected_us;
static volatile bool s_task_stop_requested;
// Cleared when WiFi drops so the next connection announces again.
static volatile bool s_presence_announced;
// Last time we broadcast presence. The Bridge forgets its UDP counter targets
// when its process restarts, so the device must keep re-announcing.
static int64_t s_last_presence_us;
static const int64_t kPresenceIntervalUs = 15LL * 1000 * 1000;
// EXPERIMENT (2026-10-21): while the home layer is showing, the device must not
// hold a CBDP TCP session next to the radio stream. Set by the framework on
// layer changes; when true, accepted TCP connections are closed immediately so
// the radio stream owns the WiFi RX path. Only UDP discovery + home counters
// run in this state. See docs/CodexBridge-UDP-home-counters-2026-10-21.md.
static volatile bool s_home_only_mode;
void wireless_bridge_set_home_only(bool home_only) { s_home_only_mode = home_only; }
// Radio playback starts right after WiFi; letting it settle first keeps the
// CBDP buffers from stealing the few KB the HTTP client needs.
static int64_t s_wifi_connected_us;
static TaskHandle_t s_task_handle;
static StaticTask_t s_task_tcb;
extern uint8_t *xiaozhi_bridge_stack(size_t *size);

static esp_err_t save_token(const char *token)
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_str(handle, NVS_TOKEN_KEY, token);
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return result;
}

// Drop a token the Bridge has told us is no longer usable (revoked or
// rotated). The next HELLO then carries pairing_state=request again.
static void clear_token(void)
{
    s_token[0] = '\0';
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_erase_key(handle, NVS_TOKEN_KEY);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
}

static void load_token(void)
{
    nvs_handle_t handle = 0;
    size_t length = sizeof(s_token);
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        result = nvs_get_str(handle, NVS_TOKEN_KEY, s_token, &length);
        nvs_close(handle);
    }
    if (result != ESP_OK) s_token[0] = '\0';
}

static void random_bytes(uint8_t *data, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        data[index] = (uint8_t)(esp_random() & 0xFFU);
    }
}

static bool send_frame_locked(const uint8_t *frame, size_t length);

static bool socket_send_all(int fd, const uint8_t *data, size_t length)
{
    // SO_SNDTIMEO bounds each send() call, and on a busy link send() can still
    // report EAGAIN once the buffer fills. That is back-pressure, not a broken
    // session.
    //
    // Audio is a real-time stream, so a chunk must NOT wait seconds here: the
    // capture loop runs at 16 kHz and a long retry made it fall behind the
    // microphone, which starved the recording. Chunks therefore get a short
    // budget and the transmitter drops the frame on persistent back-pressure.
    // Terminal frames (AUDIO_END/SEND_END/CLOSE) keep a long budget because
    // losing one strands the host-side transaction.
    int64_t budget_us = 2000000LL;
    if (length >= CBDP_FIXED_HEADER_BYTES) {
        const uint8_t type = data[3];
        if (type == CBDP_MT_AUDIO_END || type == CBDP_MT_SEND_END ||
            type == CBDP_MT_CLOSE) {
            budget_us = 10000000LL;
        } else if (type == CBDP_MT_AUDIO_CHUNK || type == CBDP_MT_SEND_CHUNK) {
            // Keep this short. The receive loop shares this lock, so a chunk
            // that spins here also stops us draining the host's own events; if
            // that fills the host's send buffer it stops reading our audio and
            // the two sides deadlock. A brief attempt, then drop the frame:
            // s_capture_offset only advances on success, so the stream the host
            // sees stays contiguous.
            budget_us = 5000LL;
        }
    }
    const int64_t deadline = esp_timer_get_time() + budget_us;
    size_t offset = 0U;
    while (offset < length) {
        const int sent = send(fd, data + offset, length - offset, 0);
        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (esp_timer_get_time() < deadline) {
                vTaskDelay(1);
                continue;
            }
        }
        return false;
    }
    return true;
}

bool wireless_bridge_send_frame(const uint8_t *frame, size_t length)
{
    if (frame == NULL || length < CBDP_FIXED_HEADER_BYTES || length > NETWORK_FRAME_MAX) {
        return false;
    }
    return send_frame_locked(frame, length);
}

static bool send_frame_locked(const uint8_t *frame, size_t length)
{
    // A blocking send can hold the socket lock for up to SO_SNDTIMEO. Waiting
    // less than that here made the audio path drop frames (and lose AUDIO_END)
    // whenever a slower peer stalled an in-band ACK send, so wait longer than
    // the socket timeout before giving up.
    if (s_client_lock == NULL ||
        xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "send frame type=0x%02X: socket lock busy", (unsigned)frame[3]);
        return false;
    }
    const bool authenticated = s_client_authenticated;
    const int fd = s_client_fd;
    const bool ok = authenticated && fd >= 0 && socket_send_all(fd, frame, length);
    xSemaphoreGive(s_client_lock);
    if (!ok && frame[3] != CBDP_MT_AUDIO_CHUNK) {
        ESP_LOGW(TAG, "send frame type=0x%02X failed (auth=%d fd=%d errno=%d)",
                 (unsigned)frame[3], (int)authenticated, fd, errno);
    }
    if (!ok) return false;
    return ok;
}

static void client_replace(int new_fd)
{
    if (s_client_lock == NULL) {
        if (new_fd >= 0) close(new_fd);
        return;
    }
    if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        if (new_fd >= 0) close(new_fd);
        return;
    }
    if (s_client_fd >= 0) {
        struct linger linger = { .l_onoff = 1, .l_linger = 0 };
        setsockopt(s_client_fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
        close(s_client_fd);
    }
    s_client_fd = new_fd;
    s_client_authenticated = false;
    s_session_ready = false;
    s_last_rx_us = 0;
    s_last_ping_us = 0;
    s_client_connected_us = new_fd >= 0 ? esp_timer_get_time() : 0;
    memset(&s_session, 0, sizeof(s_session));
    cbdp_fragment_reset(&s_fragment_assembly);
    s_fragment_started_ms = 0U;
    xSemaphoreGive(s_client_lock);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool transmit_hello(int fd)
{
    // 11.8: advertise pairing_state=request only while no usable token exists.
    // A provisioned device sends the byte-identical pre-pairing HELLO.
    const bool request_pairing = (s_token[0] == '\0');
    uint8_t frame[320];
    size_t length = 0U;
    random_bytes(s_client_nonce, sizeof(s_client_nonce));
    if (!cbdp_build_hello(frame, sizeof(frame), &length, s_request_id++,
                          s_client_nonce, request_pairing)) {
        return false;
    }
    return socket_send_all(fd, frame, length);
}

static bool transmit_auth(int fd)
{
    if (s_token[0] == '\0') return false;
    uint8_t frame[256];
    size_t length = 0U;
    if (!cbdp_build_auth(frame, sizeof(frame), &length, &s_session, s_token,
                         s_client_nonce, s_request_id++)) {
        return false;
    }
    return socket_send_all(fd, frame, length);
}

static bool transmit_pong(int fd, uint32_t echo)
{
    uint8_t frame[128];
    size_t length = 0U;
    if (!cbdp_build_pong(frame, sizeof(frame), &length, echo)) return false;
    return send_frame_locked(frame, length);
}

// Section 12/11.1: either side may start PING/PONG once the session is READY.
// The device sends PING when the link is quiet so a healthy idle session is not
// mistaken for a dead peer.
static bool transmit_ping(int fd, uint32_t ping_id)
{
    uint8_t frame[128];
    size_t length = 0U;
    if (!cbdp_build_ping(frame, sizeof(frame), &length, ping_id)) return false;
    return send_frame_locked(frame, length);
}

static bool transmit_discovery_reply(int fd, const struct sockaddr_in *source,
                                     socklen_t source_length, const uint8_t nonce[16])
{
    uint8_t frame[256];
    size_t length = 0U;
    if (!cbdp_build_discovery_reply(frame, sizeof(frame), &length, nonce)) return false;
    return sendto(fd, frame, length, 0, (const struct sockaddr *)source,
                  source_length) == (int)length;
}

// Announce ourselves on the discovery port right after the socket exists.
// CodexBridge runs a presence listener that accepts the same reply frame a
// broadcast gets, and its own comment says the device "carries the low-latency
// path itself". Without this the PC only finds us on its next broadcast/redial
// beat (up to ~15s), which is exactly the "enter Codex is slow / shows
// disconnected" report. Send a few copies so one lost datagram is harmless.
static void announce_presence(int fd)
{
    struct sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(DISCOVERY_PORT);
    target.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    uint8_t frame[256];
    size_t length = 0U;
    if (!cbdp_build_discovery_reply(frame, sizeof(frame), &length, nonce)) return;
    for (int attempt = 0; attempt < 3; ++attempt) {
        (void)sendto(fd, frame, length, 0, (struct sockaddr *)&target,
                     sizeof(target));
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    ESP_LOGI(TAG, "presence announced on UDP %d", DISCOVERY_PORT);
}

static void process_client_frame(const uint8_t *frame, size_t length)
{
    cbdp_frame_header_t header;
    if (!cbdp_frame_validate(frame, length, &header)) return;
    // Any valid inbound frame proves the peer is alive, including PONG/PING.
    s_last_rx_us = esp_timer_get_time();
    const uint8_t *header_tlv = frame + CBDP_FIXED_HEADER_BYTES;
    const uint8_t *payload = frame + header.header_len;

    if (header.message_type == CBDP_MT_WELCOME) {
        if (!cbdp_parse_welcome(header_tlv, header.header_len - CBDP_FIXED_HEADER_BYTES,
                                &s_session)) {
            client_replace(-1);
            return;
        }
        if (s_token[0] != '\0') {
            // Provisioned device: unchanged pre-pairing flow.
            if (!transmit_auth(s_client_fd)) client_replace(-1);
        } else {
            // 11.8: no token yet. Stay connected and wait for the Bridge's
            // PAIRING offer; AUTH here would fail (no HMAC key) and the old
            // code dropped the link, so pairing could never complete.
            // The 10s handshake guard still bounds how long we wait.
            ESP_LOGI(TAG, "WELCOME received; awaiting PAIRING offer");
        }
        return;
    }
    if (header.message_type == CBDP_MT_AUTH_OK) {
        if (!cbdp_parse_auth_ok(header_tlv, header.header_len - CBDP_FIXED_HEADER_BYTES,
                                &s_session)) {
            client_replace(-1);
            return;
        }
        if (s_client_lock != NULL &&
            xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_client_authenticated = true;
            xSemaphoreGive(s_client_lock);
        }
        s_session_ready = true;
        s_last_ping_us = esp_timer_get_time();
        ESP_LOGI(TAG, "CBDP session ready");
        if (s_frame_cb != NULL) {
            s_frame_cb(&header, header_tlv, header.header_len - CBDP_FIXED_HEADER_BYTES,
                       payload, header.payload_len);
        }
        return;
    }
    if (header.message_type == CBDP_MT_PING) {
        const cbdp_tlv_t *ping = cbdp_tlv_find(
            header_tlv, header.header_len - CBDP_FIXED_HEADER_BYTES, CBDP_TLV_PING_ID);
        uint32_t echo = 0U;
        (void)cbdp_tlv_read_u32(ping, &echo);
        if (s_client_fd >= 0) transmit_pong(s_client_fd, echo);
        return;
    }
    // 11.8: PAIRING arrives before AUTH by design, so it must be handled ahead
    // of the authenticated gate below or the offer would be dropped forever.
    if (header.message_type == CBDP_MT_PAIRING) {
        char new_token[CBDP_PAIRING_TOKEN_MAX_BYTES + 1];
        new_token[0] = '\0';
        const size_t tlv_length =
            (size_t)header.header_len - CBDP_FIXED_HEADER_BYTES;
        if (!cbdp_parse_pairing_offer(header_tlv, tlv_length, new_token,
                                      sizeof(new_token))) {
            ESP_LOGW(TAG, "PAIRING offer rejected");
            client_replace(-1);
            return;
        }
        const esp_err_t saved = save_token(new_token);
        if (saved != ESP_OK) {
            ESP_LOGE(TAG, "pairing save failed: %s", esp_err_to_name(saved));
            client_replace(-1);
            return;
        }
        strlcpy(s_token, new_token, sizeof(s_token));
        // Report the request_id echoed by the Bridge so host-side logs line up.
        // Never log the token itself, only its size (handoff section 9).
        uint32_t offer_request_id = 0U;
        (void)cbdp_tlv_read_u32(
            cbdp_tlv_find(header_tlv, tlv_length, CBDP_TLV_REQUEST_ID),
            &offer_request_id);
        ESP_LOGI(TAG, "pairing offer: request_id=%lu token_bytes=%u",
                 (unsigned long)offer_request_id, (unsigned)strlen(s_token));
        ESP_LOGI(TAG, "pairing saved: nvs=ok");
        if (!transmit_auth(s_client_fd)) client_replace(-1);
        return;
    }

    // Token invalidation is handled here rather than in the application layer
    // because it must also work pre-auth: a revoked device can learn its token
    // is dead on the very next handshake, and the session is re-established by
    // this transport, not by coding_bridge.c. Only the stored token changes;
    // the frame still falls through so the app can surface the failure.
    if (header.message_type == CBDP_MT_ERROR &&
        (header.code == CBDP_ERR_UNAUTHORIZED ||
         header.code == CBDP_ERR_TOKEN_ROTATED)) {
        // 11.8: these two mean the stored token itself is unusable (revoked or
        // rotated). Drop it so the next HELLO requests pairing again instead of
        // reconnect-looping forever with a dead token. AUTH_EXPIRED is not
        // included: it asks for a fresh AUTH with the same token.
        ESP_LOGW(TAG, "bridge rejected token (code=0x%04X); clearing local token",
                 (unsigned)header.code);
        clear_token();
    }

    bool authenticated = false;
    if (s_client_lock != NULL &&
        xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        authenticated = s_client_authenticated;
        xSemaphoreGive(s_client_lock);
    }
    if (!authenticated) return;

    if (header.message_type == CBDP_MT_FRAGMENT) {
        const cbdp_tlv_t *crc_tlv = cbdp_tlv_find(
            header_tlv, header.header_len - CBDP_FIXED_HEADER_BYTES,
            CBDP_TLV_PAYLOAD_CRC32);
        uint32_t crc32 = 0U;
        if ((header.flags & CBDP_FLAG_CRC32_PRESENT) != 0U &&
            !cbdp_tlv_read_u32(crc_tlv, &crc32)) {
            cbdp_fragment_reset(&s_fragment_assembly);
            return;
        }
        uint16_t fragment_index = 0U;
        const cbdp_tlv_t *index_tlv = cbdp_tlv_find(
            header_tlv, header.header_len - CBDP_FIXED_HEADER_BYTES,
            CBDP_TLV_FRAGMENT_INDEX);
        if (!cbdp_tlv_read_u16(index_tlv, &fragment_index)) {
            cbdp_fragment_reset(&s_fragment_assembly);
            return;
        }
        if (s_fragment_assembly.active &&
            now_ms() - s_fragment_started_ms > FRAGMENT_TIMEOUT_MS) {
            cbdp_fragment_reset(&s_fragment_assembly);
        }
        const bool was_active = s_fragment_assembly.active;
        bool accepted = false;
        uint8_t *complete_header = NULL;
        uint8_t *complete_payload = NULL;
        size_t complete_header_len = 0U;
        size_t complete_payload_len = 0U;
        accepted = cbdp_fragment_accept(
            &s_fragment_assembly, header_tlv,
            header.header_len - CBDP_FIXED_HEADER_BYTES, header.flags,
            payload, header.payload_len, &complete_header,
            &complete_header_len, &complete_payload, &complete_payload_len);
        if (!was_active && s_fragment_assembly.active) s_fragment_started_ms = now_ms();
        if (accepted && complete_payload != NULL) {
            cbdp_frame_header_t original = {0};
            original.version_major = CBDP_VERSION_MAJOR;
            original.message_type = s_fragment_assembly.original_type;
            original.flags = s_fragment_assembly.original_flags;
            original.header_len = (uint16_t)(CBDP_FIXED_HEADER_BYTES + complete_header_len);
            original.payload_len = (uint32_t)complete_payload_len;
            original.qos = header.qos;
            if (s_frame_cb != NULL) {
                s_frame_cb(&original, complete_header, complete_header_len,
                           complete_payload, complete_payload_len);
            }
        }
        free(complete_header);
        free(complete_payload);
        return;
    }

    if (header.message_type == CBDP_MT_CAPABILITY_UPDATE) {
        (void)cbdp_parse_capability_update(
            header_tlv, header.header_len - CBDP_FIXED_HEADER_BYTES, &s_session);
    }
    if (s_frame_cb != NULL) {
        s_frame_cb(&header, header_tlv, header.header_len - CBDP_FIXED_HEADER_BYTES,
                   payload, header.payload_len);
    }
}

static int create_bound_socket(int type, int port)
{
    int fd = socket(AF_INET, type, IPPROTO_IP);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void network_task(void *argument)
{
    (void)argument;
    int tcp_fd = create_bound_socket(SOCK_STREAM, BRIDGE_PORT);
    int udp_fd = create_bound_socket(SOCK_DGRAM, DISCOVERY_PORT);
    if (tcp_fd < 0 || udp_fd < 0 || listen(tcp_fd, 1) != 0) {
        ESP_LOGE(TAG, "network sockets failed: %d", errno);
        if (tcp_fd >= 0) close(tcp_fd);
        if (udp_fd >= 0) close(udp_fd);
        s_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t input[NETWORK_FRAME_MAX];
    size_t input_length = 0U;
    uint8_t udp_input[NETWORK_FRAME_MAX];
    // The client fd is only ever published by this task, so the receive loop
    // tracks it directly. Taking s_client_lock here every second competed with
    // the audio uplink's send path: under a chunk flood the lock was never free,
    // the loop never reached recv(), and the session died on its own heartbeat
    // timeout in the middle of a recording.
    int client_fd = -1;
    ESP_LOGI(TAG, "CBDP LAN bridge ready on TCP %d", BRIDGE_PORT);

    while (!s_task_stop_requested) {
        if (client_fd >= 0 && s_client_fd != client_fd) {
            // Another path (disconnect/heartbeat lapse) dropped the session.
            client_fd = -1;
            input_length = 0U;
        }
        int client = client_fd;
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(tcp_fd, &read_set);
        FD_SET(udp_fd, &read_set);
        int max_fd = tcp_fd > udp_fd ? tcp_fd : udp_fd;
        if (client >= 0) {
            FD_SET(client, &read_set);
            if (client > max_fd) max_fd = client;
        }
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(max_fd + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0) continue;

        // Presence keep-alive.
        //
        // 2026-10-22: 之前这里只在**每次 WiFi 连上后播报一次**
        // (`!s_presence_announced`)。而 CodexBridge 的首页计数推送器
        // (devices/home_counters.py: HomeCounterSender) 是按 presence 注册来
        // 决定推给谁的 —— Bridge 进程一重启, 内存里的 targets 就清空了。
        // 设备以为"我已经播报过了", 于是**永远不再播报**, 那台设备从此收不到
        // 0x1E 计数帧, 首页角标就冻在最后一次的值上。用户反馈的
        // "电脑上点掉已完成了, 设备还显示 5 个已完成" 就是这个。
        //
        // 改成每 kPresenceIntervalUs 重播一次: 正常情况下只是每 15s 三个 UDP
        // 小包; Bridge 重启后最多 15s 就能重新发现并恢复计数, 不需要重启设备
        // 或断一次 WiFi。
        if (s_wifi_connected &&
            (s_wifi_connected_us == 0 ||
             esp_timer_get_time() - s_wifi_connected_us > 8000000LL) &&
            (!s_presence_announced ||
             esp_timer_get_time() - s_last_presence_us > kPresenceIntervalUs)) {
            s_presence_announced = true;
            s_last_presence_us = esp_timer_get_time();
            announce_presence(udp_fd);
        }

        // Pairing just completed while the home layer was showing. The socket
        // was only kept alive to receive the offer; leaving it up would put a
        // CBDP session next to the radio stream and reintroduce the DMA
        // starvation that home-only mode exists to prevent. Hand it back and
        // let the normal UDP-presence path report this device as online.
        if (client >= 0 && s_session_ready && s_home_only_mode &&
            s_token[0] != 0) {
            ESP_LOGI(TAG, "pairing done on home layer; releasing CBDP socket");
            client_replace(-1);
            input_length = 0U;
            client = -1;
            client_fd = -1;
        }

        // Section 12 liveness. Handshake stages are bounded by 10s, and a
        // READY session tolerates 3 x heartbeat_seconds without any inbound
        // frame before it is considered dead. PING keeps an idle link warm.
        if (client >= 0) {
            const int64_t now_us = esp_timer_get_time();
            if (!s_session_ready) {
                if (s_client_connected_us != 0 &&
                    now_us - s_client_connected_us > HANDSHAKE_TIMEOUT_US) {
                    // With no token a missing offer means "not approved yet",
                    // not a transport failure; say so instead of logging a
                    // generic timeout the user cannot act on.
                    if (s_token[0] == '\0') {
                        ESP_LOGW(TAG, "CBDP pairing offer timed out; still "
                                      "waiting for host approval");
                    } else {
                        ESP_LOGW(TAG, "CBDP handshake timed out");
                    }
                    client_replace(-1);
                    input_length = 0U;
                    client = -1;
                    client_fd = -1;
                }
            } else {
                const uint32_t heartbeat =
                    s_session.heartbeat_seconds != 0U ? s_session.heartbeat_seconds
                                                      : HEARTBEAT_FALLBACK_SECONDS;
                if (s_last_rx_us != 0 &&
                    now_us - s_last_rx_us > (int64_t)heartbeat * 3 * 1000000LL) {
                    ESP_LOGW(TAG, "CBDP heartbeat timed out (%us)", (unsigned)heartbeat);
                    client_replace(-1);
                    input_length = 0U;
                    client = -1;
                    client_fd = -1;
                } else if (now_us - s_last_ping_us > (int64_t)heartbeat * 1000000LL) {
                    if (!transmit_ping(client, s_request_id++)) {
                        client_replace(-1);
                        input_length = 0U;
                        client = -1;
                        client_fd = -1;
                    } else {
                        s_last_ping_us = now_us;
                    }
                }
            }
        }

        if (FD_ISSET(tcp_fd, &read_set)) {
            int accepted = accept(tcp_fd, NULL, NULL);
            // 11.8: an UNPAIRED device must stay reachable on the home layer.
            //
            // Home-only mode exists to keep the CBDP socket out of the WiFi RX
            // path while the radio streams. But it also drops every inbound
            // connection, and the documented first-run flow is exactly
            // "admin approves -> Bridge dials the device on 8765" while the
            // user is looking at the home screen. Refusing that dial means the
            // pairing offer can never be delivered and a factory-fresh device
            // can never be provisioned.
            //
            // Holding one socket until pairing completes is the correct trade:
            // there is no radio stream worth protecting yet, because an
            // unpaired device cannot have come from a working setup. Once a
            // token is stored the normal home-only economy applies again.
            if (accepted >= 0 && s_home_only_mode && s_token[0] != 0) {
                // Drop immediately: the host will retry after its redial
                // cooldown, and the home layer needs no control channel.
                close(accepted);
                accepted = -1;
                ESP_LOGW(TAG, "home-only: refused CBDP TCP while minimized");
            } else if (accepted >= 0 && s_home_only_mode) {
                ESP_LOGI(TAG, "home-only: accepting CBDP so pairing can complete");
            }
            if (accepted >= 0) {
                struct timeval send_timeout = {.tv_sec = 2, .tv_usec = 0};
                setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO,
                           &send_timeout, sizeof(send_timeout));
                // A 16 kHz capture feeds the socket steadily; the default send
                // buffer is small enough that a brief scheduling hiccup on the
                // peer makes every AUDIO_CHUNK fail with EAGAIN and aborts the
                // recording. Ask for a larger buffer so normal jitter is
                // absorbed instead of surfaced as a failed frame.
                int send_buffer = 8192;
                setsockopt(accepted, SOL_SOCKET, SO_SNDBUF,
                           &send_buffer, sizeof(send_buffer));
                // 控制平面不能抢走数据平面的 lwIP 内存。订阅时主机会一次性
                // 重放整段事件积压(实测 32 条 x ~600B, 其中多数还是本设备
                // **未订阅**的其它线程消息)。默认接收缓冲会把 lwIP 的 pbuf/
                // 接收窗口吃光, 正在播的网络电台随即 alloc 失败, 表现为
                // "eagain 连续 10~16 次"、声音停十几秒再重连(真机实测)。
                // 这里把控制 socket 的接收缓冲压到最小, 让 Bridge 侧被 TCP
                // 窗口限速, 突发被摊平; 电台的收包内存优先级因此得到保障。
                int recv_buffer = 2048;
                setsockopt(accepted, SOL_SOCKET, SO_RCVBUF,
                           &recv_buffer, sizeof(recv_buffer));
                int no_delay = 1;
                setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                           &no_delay, sizeof(no_delay));
                client_replace(accepted);
                input_length = 0U;
                client = accepted;
                client_fd = accepted;
                // Protocol 3 / 11.2: in Device Listener Binding the device MUST send
                // HELLO first, immediately after TCP is established. HELLO needs no
                // token; authentication happens later in the AUTH stage. The old code
                // closed the connection whenever no token was stored, which violated
                // that MUST and left the Bridge with no HELLO at all (the desktop was
                // running but the device kept reporting it was disconnected).
                // The token is provisioned out of band into NVS by
                // wireless_bridge_configure(); it is never carried on 8765.
                if (!transmit_hello(accepted)) {
                    client_replace(-1);
                } else if (s_token[0] == 0) {
                    ESP_LOGW(TAG, "HELLO sent but no pairing token stored; "
                                  "AUTH will fail until the token is provisioned");
                }
            }
        }
        if (FD_ISSET(udp_fd, &read_set)) {
            struct sockaddr_in source;
            socklen_t source_length = sizeof(source);
            const int count = recvfrom(udp_fd, udp_input, sizeof(udp_input), 0,
                                       (struct sockaddr *)&source, &source_length);
            if (count > 0) {
                cbdp_frame_header_t header;
                if (cbdp_frame_validate(udp_input, (size_t)count, &header) &&
                    header.message_type == CBDP_MT_HOME_COUNTERS) {
                    // Bridge -> device aggregate counters over UDP; the home
                    // layer holds no TCP session, so this is its only source.
                    (void)coding_bridge_apply_udp_home_counters(
                        udp_input + CBDP_FIXED_HEADER_BYTES,
                        header.header_len - CBDP_FIXED_HEADER_BYTES);
                }
                if (cbdp_frame_validate(udp_input, (size_t)count, &header) &&
                    header.message_type == CBDP_MT_DISCOVERY) {
                    const cbdp_tlv_t *nonce = cbdp_tlv_find(
                        udp_input + CBDP_FIXED_HEADER_BYTES,
                        header.header_len - CBDP_FIXED_HEADER_BYTES,
                        CBDP_TLV_DISCOVERY_NONCE);
                    const uint8_t *nonce_value = NULL;
                    size_t nonce_length = 0U;
                    if (nonce != NULL &&
                        cbdp_tlv_read_bytes(nonce, &nonce_value, &nonce_length) &&
                        nonce_length == 16U && s_wifi_connected) {
                        transmit_discovery_reply(udp_fd, &source, source_length, nonce_value);
                    }
                }
            }
        }
        if (client >= 0 && FD_ISSET(client, &read_set)) {
            const int count = recv(client, input + input_length,
                                   sizeof(input) - input_length, 0);
            if (count <= 0) {
                client_replace(-1);
                input_length = 0U;
                client_fd = -1;
            } else {
                input_length += (size_t)count;
                size_t consumed = 0U;
                unsigned frames_this_batch = 0U;
                while (input_length - consumed >= CBDP_FIXED_HEADER_BYTES) {
                    cbdp_frame_header_t header;
                    if (!cbdp_header_decode(input + consumed, input_length - consumed,
                                            &header)) {
                        consumed = input_length;
                        break;
                    }
                    const size_t frame_length =
                        (size_t)header.header_len + (size_t)header.payload_len;
                    if (frame_length > sizeof(input)) {
                        client_replace(-1);
                        consumed = input_length;
                        client_fd = -1;
                        break;
                    }
                    if (input_length - consumed < frame_length) break;
                    if (cbdp_frame_validate(input + consumed, frame_length, NULL)) {
                        process_client_frame(input + consumed, frame_length);
                        // A reconnect can deliver a large backlog in a single
                        // read. Processing it without yielding starves the
                        // IDLE/LVGL tasks and trips the watchdog, so give the
                        // scheduler a slot every few frames.
                        if (++frames_this_batch >= 4U) {
                            frames_this_batch = 0U;
                            vTaskDelay(1);
                        }
                    }
                    consumed += frame_length;
                }
                if (consumed != 0U) {
                    memmove(input, input + consumed, input_length - consumed);
                    input_length -= consumed;
                }
            }
        }
    }

    close(tcp_fd);
    close(udp_fd);
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

void wireless_bridge_set_wifi_configure(wireless_bridge_wifi_configure_t configure)
{
    s_wifi_configure_cb = configure;
}

void wireless_bridge_set_wifi_state(bool connected, const char *ip)
{
    if (connected && !s_wifi_connected) s_wifi_connected_us = esp_timer_get_time();
    s_wifi_connected = connected;
    if (!connected) s_presence_announced = false;
    strlcpy(s_ip, ip != NULL ? ip : "", sizeof(s_ip));
    if (!connected) client_replace(-1);
    if (s_status_cb != NULL) {
        s_status_cb(connected ? "connected" : "disconnected", s_ip);
    }
}

bool wireless_bridge_is_connected(void) { return s_wifi_connected; }
void wireless_bridge_disconnect(void) { client_replace(-1); }

void wireless_bridge_get_ip(char *buffer, size_t size)
{
    if (buffer == NULL || size == 0U) return;
    strlcpy(buffer, s_ip, size);
}

esp_err_t wireless_bridge_configure(const char *ssid, const char *password,
                                    const char *token)
{
    if (token != NULL && token[0] != '\0') {
        esp_err_t saved = save_token(token);
        if (saved != ESP_OK) return saved;
        strlcpy(s_token, token, sizeof(s_token));
    }
    if (s_wifi_configure_cb == NULL) return ESP_ERR_INVALID_STATE;
    return s_wifi_configure_cb(ssid, password);
}

bool wireless_bridge_session_ready(void)
{
    bool ready = false;
    if (s_client_lock != NULL &&
        xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        ready = s_client_authenticated && s_session.ready;
        xSemaphoreGive(s_client_lock);
    }
    return ready;
}

int wireless_bridge_seconds_since_rx(void)
{
    if (!s_session_ready || s_last_rx_us == 0) return -1;
    const int64_t elapsed = esp_timer_get_time() - s_last_rx_us;
    return elapsed <= 0 ? 0 : (int)(elapsed / 1000000LL);
}

bool wireless_bridge_session_active(void) { return s_session_ready; }

bool wireless_bridge_session_snapshot(cbdp_session_t *session)
{
    if (session == NULL || s_client_lock == NULL ||
        xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    *session = s_session;
    xSemaphoreGive(s_client_lock);
    return true;
}

bool wireless_bridge_token_available(void)
{
    return s_token[0] != '\0';
}

esp_err_t wireless_bridge_init(wireless_bridge_frame_cb_t frame_cb,
                               wireless_bridge_status_cb_t status_cb)
{
    s_frame_cb = frame_cb;
    s_status_cb = status_cb;
    if (s_client_lock == NULL) {
        s_client_lock = xSemaphoreCreateMutex();
        if (s_client_lock == NULL) return ESP_ERR_NO_MEM;
    }
    load_token();
    if (s_task_handle != NULL) return ESP_OK;
    s_task_stop_requested = false;
    size_t stack_size = 0U;
    uint8_t *stack = xiaozhi_bridge_stack(&stack_size);
    if (stack == NULL || stack_size < NETWORK_TASK_STACK) {
        ESP_LOGE(TAG, "idle stack unavailable for bridge (size=%u)", (unsigned)stack_size);
        return ESP_ERR_NO_MEM;
    }
    s_task_handle = xTaskCreateStatic(network_task, "wireless_bridge",
                                      NETWORK_TASK_STACK, NULL, 4,
                                      (StackType_t *)stack, &s_task_tcb);
    if (s_task_handle == NULL) return ESP_ERR_NO_MEM;
    /* ⚠ 每次重建任务都必须重新通告一次。
     * s_presence_announced 原先只在 WiFi 断开时清零, 而完整收音机会 stop/start
     * 这个任务(WiFi 并不中断)。于是回首页重启后标志仍是 true, 设备再也不广播,
     * 电脑端一直不知道它回来了 —— 真机表现为"回首页后 CBDP 会话迟迟不出现"。
     * 这里清零, 并令 s_wifi_connected_us=0(表示"立刻通告"), 让重启路径跳过
     * 开机时那 8 秒等待(那段等待只为给收音机首播让内存)。 */
    s_presence_announced = false;
    s_wifi_connected_us = 0;
    ESP_LOGI(TAG, "CBDP transport ready, token=%s", s_token[0] ? "loaded" : "missing");
    return ESP_OK;
}

void wireless_bridge_stop(void)
{
    s_task_stop_requested = true;
    client_replace(-1);
    for (int index = 0; index < 100 && s_task_handle != NULL; ++index) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_task_handle != NULL) ESP_LOGW(TAG, "network task did not stop in time");
}
