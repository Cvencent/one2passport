// main/wireless_bridge.h
// LAN bridge transport: CBDP v1 over TCP 8765 and UDP discovery on 8764.
//
// This project keeps WiFi ownership in esp-wifi-connect (WifiManager). The
// wireless bridge therefore only owns sockets, discovery, CBDP session
// framing/authentication and token persistence. Application semantics live in
// coding_bridge.c and are delivered through the frame callback below.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codex_bridge_protocol.h"
#include "esp_err.h"

typedef void (*wireless_bridge_frame_cb_t)(const cbdp_frame_header_t *header,
                                           const uint8_t *header_tlv, size_t header_tlv_len,
                                           const uint8_t *payload, size_t payload_len);
typedef void (*wireless_bridge_status_cb_t)(const char *status, const char *ip);
typedef esp_err_t (*wireless_bridge_wifi_configure_t)(const char *ssid,
                                                      const char *password);

esp_err_t wireless_bridge_init(wireless_bridge_frame_cb_t frame_cb,
                               wireless_bridge_status_cb_t status_cb);
void wireless_bridge_stop(void);
void wireless_bridge_set_wifi_configure(wireless_bridge_wifi_configure_t configure);
void wireless_bridge_set_wifi_state(bool connected, const char *ip);

/* Send one complete CBDP frame. The buffer already contains the fixed header
 * and its header_len/payload_len fields. */
bool wireless_bridge_send_frame(const uint8_t *frame, size_t length);

esp_err_t wireless_bridge_configure(const char *ssid, const char *password,
                                    const char *token);
bool wireless_bridge_is_connected(void);
void wireless_bridge_get_ip(char *buffer, size_t size);
void wireless_bridge_disconnect(void);

/* Session accessors for coding_bridge.c. */
bool wireless_bridge_session_ready(void);
bool wireless_bridge_session_snapshot(cbdp_session_t *session);
bool wireless_bridge_token_available(void);

/* Seconds since the last valid inbound frame, or -1 when no session is
 * established. Section 12 heartbeat liveness lives in the transport. */
int wireless_bridge_seconds_since_rx(void);

/* Lock-free view of the authenticated-session lifecycle flag. Unlike
 * wireless_bridge_session_ready() this never blocks, so callers may use it to
 * decide whether to tear application state down without risking a false
 * negative when another task briefly holds the socket lock. */
bool wireless_bridge_session_active(void);

#ifdef __cplusplus
}
#endif
