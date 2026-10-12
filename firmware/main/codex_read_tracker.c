#include "codex_bridge_protocol.h"

#include <stdio.h>
#include <string.h>

#define CODEX_READ_TRACKER_DELAY_MS 2000

static char s_message_id[CODEX_BRIDGE_ID_MAX];
static char s_result_message_id[CODEX_BRIDGE_ID_MAX];
static unsigned s_visit_ms;
static bool s_reported;

static const char *safe_id(const char *message_id)
{
    return message_id == NULL ? "" : message_id;
}

static bool identity_matches(const char *message_id, const char *result_message_id)
{
    return strcmp(s_message_id, safe_id(message_id)) == 0 &&
           strcmp(s_result_message_id, safe_id(result_message_id)) == 0;
}

static bool identity_empty(const char *message_id, const char *result_message_id)
{
    return safe_id(message_id)[0] == '\0' && safe_id(result_message_id)[0] == '\0';
}

void codex_read_tracker_reset(void)
{
    s_message_id[0] = '\0';
    s_result_message_id[0] = '\0';
    s_visit_ms = 0;
    s_reported = false;
}

void codex_read_tracker_visit(
    const char *message_id,
    const char *result_message_id,
    unsigned now_ms
)
{
    if (identity_empty(message_id, result_message_id)) {
        codex_read_tracker_reset();
        return;
    }
    if (identity_matches(message_id, result_message_id)) return;
    snprintf(s_message_id, sizeof(s_message_id), "%s", safe_id(message_id));
    snprintf(s_result_message_id, sizeof(s_result_message_id), "%s", safe_id(result_message_id));
    s_visit_ms = now_ms;
    s_reported = false;
}

bool codex_read_tracker_take_due(
    const char *message_id,
    const char *result_message_id,
    unsigned now_ms
)
{
    if (identity_empty(message_id, result_message_id) || s_reported ||
        !identity_matches(message_id, result_message_id)) {
        return false;
    }
    if ((unsigned)(now_ms - s_visit_ms) < CODEX_READ_TRACKER_DELAY_MS) return false;
    s_reported = true;
    return true;
}

void codex_read_tracker_rearm(
    const char *message_id,
    const char *result_message_id,
    unsigned now_ms
)
{
    if (identity_empty(message_id, result_message_id) ||
        !identity_matches(message_id, result_message_id)) {
        return;
    }
    s_visit_ms = now_ms;
    s_reported = false;
}
