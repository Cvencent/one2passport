#pragma once

/* 实现在 coding_bridge.c(C),调用方 main.cc 是 C++。
 * 缺了 extern "C" 会在链接期报 undefined reference to `coding_bridge_init()`。 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "coding_flow.h"
#include "esp_err.h"

esp_err_t coding_bridge_init(void);
esp_err_t coding_bridge_init_usb_only(void);
esp_err_t coding_bridge_start_wireless(void);
void coding_bridge_stop_wireless(void);
void coding_bridge_set_foreground_ui(bool active);
bool coding_bridge_foreground_ui_active(void);
bool coding_bridge_send_read_ack(const char *message_id);
bool coding_bridge_capability_enabled(const char *capability);
void coding_bridge_send_record_start(void);
void coding_bridge_send_record_stop(void);
void coding_bridge_cancel_record(void);
bool coding_bridge_send_voice(const char *text);
void coding_bridge_send_approval(coding_decision_t decision);
void coding_bridge_send_cancel(void);
void coding_bridge_request_projects(void);
void coding_bridge_cancel_catalog(void);
void coding_bridge_confirm_selection(void);
void coding_bridge_select_project(const char *id);
void coding_bridge_select_thread(const char *id);
/* Title of a thread already seen in threads.list, or NULL when unknown. Lets
 * the picker complete a selection without waiting for a thread.read reply. */
const char *coding_bridge_thread_title(const char *id);
void coding_bridge_create_thread(void);
void coding_bridge_request_review(void);
bool coding_bridge_result_seen(const char *thread_id, const char *turn_id);
bool coding_bridge_is_connected(void);

#ifdef __cplusplus
}
#endif
