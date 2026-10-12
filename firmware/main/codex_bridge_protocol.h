#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define CODEX_BRIDGE_LINE_MAX 768
#define CODEX_BRIDGE_BODY_MAX 4096
#define CODEX_BRIDGE_ID_MAX 80
#define CODEX_BRIDGE_CAPABILITY_MAX 8
#define CODEX_BRIDGE_CAPABILITY_LEN 16
#define CODEX_BRIDGE_CHUNK_MAX 32

typedef struct {
    char message_id[CODEX_BRIDGE_ID_MAX];
    int total;
    char *chunks[CODEX_BRIDGE_CHUNK_MAX];
} codex_bridge_parser_t;

typedef struct {
    char type[24];
    char message_id[CODEX_BRIDGE_ID_MAX];
    char thread_id[CODEX_BRIDGE_ID_MAX];
    char kind[16];
    char status[24];
    char title[161];
    char *body;
    int seq;
    int total;
    unsigned read_count;
    bool ignored;
    char capabilities[CODEX_BRIDGE_CAPABILITY_MAX][CODEX_BRIDGE_CAPABILITY_LEN];
    size_t capability_count;
} codex_bridge_message_t;

typedef bool (*codex_bridge_send_line_t)(const char *line, size_t length);

void codex_bridge_protocol_init(void);
const char *codex_bridge_protocol_device_id(void);
const char *const *codex_bridge_protocol_capabilities(size_t *count);
bool codex_bridge_protocol_accept_line(
    const char *line,
    codex_bridge_send_line_t send_line,
    const char *device_id
);
bool codex_bridge_protocol_feed(
    codex_bridge_parser_t *parser,
    const char *line,
    size_t length,
    codex_bridge_message_t *message
);
void codex_bridge_parser_reset(codex_bridge_parser_t *parser);
void codex_bridge_protocol_reset_message(codex_bridge_message_t *message);
bool codex_bridge_protocol_has_capability(
    const codex_bridge_message_t *message,
    const char *capability
);
bool codex_bridge_protocol_send_read_ack(
    codex_bridge_send_line_t send_line,
    const char *message_id
);

void codex_read_tracker_reset(void);
void codex_read_tracker_visit(
    const char *message_id,
    const char *result_message_id,
    unsigned now_ms
);
bool codex_read_tracker_take_due(
    const char *message_id,
    const char *result_message_id,
    unsigned now_ms
);
void codex_read_tracker_rearm(
    const char *message_id,
    const char *result_message_id,
    unsigned now_ms
);
