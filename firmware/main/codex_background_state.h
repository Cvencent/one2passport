#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CODEX_BACKGROUND_RESULT_MAX 16

typedef enum {
    CODEX_BACKGROUND_OFFLINE = 0,
    CODEX_BACKGROUND_IDLE,
    CODEX_BACKGROUND_BUSY,
    CODEX_BACKGROUND_DONE,
    CODEX_BACKGROUND_FAIL,
} codex_background_pet_t;

typedef enum {
    CODEX_BACKGROUND_RESULT_DONE_UNREAD = 0,
    CODEX_BACKGROUND_RESULT_FAILED_UNREAD,
    CODEX_BACKGROUND_RESULT_READ,
} codex_background_result_state_t;

typedef struct {
    uint32_t hash;
    bool occupied;
    uint8_t state;
} codex_background_result_t;

typedef struct {
    bool connected;
    uint32_t running;
    uint32_t done;
    uint32_t failed;
    codex_background_result_t results[CODEX_BACKGROUND_RESULT_MAX];
    uint8_t next_result;
} codex_background_state_t;

static inline uint32_t codex_background_hash(const char *text)
{
    uint32_t hash = 2166136261u;
    if (text == NULL) return hash;
    while (*text != '\0') {
        hash ^= (uint8_t)*text++;
        hash *= 16777619u;
    }
    return hash;
}

static inline codex_background_pet_t codex_background_pet_state(
    const codex_background_state_t *state
)
{
    if (state == NULL || !state->connected) return CODEX_BACKGROUND_OFFLINE;
    if (state->failed > 0U) return CODEX_BACKGROUND_FAIL;
    if (state->running > 0U) return CODEX_BACKGROUND_BUSY;
    if (state->done > 0U) return CODEX_BACKGROUND_DONE;
    return CODEX_BACKGROUND_IDLE;
}

static inline void codex_background_result_store(
    codex_background_state_t *state,
    uint32_t hash,
    codex_background_result_state_t result_state
)
{
    if (state == NULL) return;
    for (size_t index = 0; index < CODEX_BACKGROUND_RESULT_MAX; ++index) {
        if (state->results[index].occupied &&
            state->results[index].hash == hash) {
            state->results[index].state = (uint8_t)result_state;
            return;
        }
    }
    const uint8_t slot = state->next_result % CODEX_BACKGROUND_RESULT_MAX;
    state->next_result = (uint8_t)((slot + 1U) % CODEX_BACKGROUND_RESULT_MAX);
    state->results[slot].hash = hash;
    state->results[slot].occupied = true;
    state->results[slot].state = (uint8_t)result_state;
}

static inline bool codex_background_result_seen(
    const codex_background_state_t *state,
    uint32_t hash
)
{
    if (state == NULL) return false;
    for (size_t index = 0; index < CODEX_BACKGROUND_RESULT_MAX; ++index) {
        if (state->results[index].occupied &&
            state->results[index].hash == hash) return true;
    }
    return false;
}

/* Applies status/task messages. Results use codex_background_apply_result()
 * because they also need message-level de-duplication and read tracking. */
static inline void codex_background_apply(
    codex_background_state_t *state,
    const char *kind,
    const char *status,
    uint32_t running
)
{
    (void)status;
    if (state == NULL) return;
    state->connected = true;
    if (kind != NULL && strcmp(kind, "status") == 0) {
        state->running = running;
    }
    if (kind != NULL && strcmp(kind, "task") == 0) {
        state->running = running;
    }
}

/* Returns true only for the first delivery of a result message. The host may
 * rebroadcast the same immutable message on every collector tick; callers use
 * this to avoid duplicate unread counts and duplicate notification sounds. */
static inline bool codex_background_apply_result(
    codex_background_state_t *state,
    const char *message_id,
    const char *status
)
{
    if (state == NULL || message_id == NULL || message_id[0] == '\0') return false;
    state->connected = true;
    state->running = 0U;
    const uint32_t hash = codex_background_hash(message_id);
    if (codex_background_result_seen(state, hash)) return false;
    const bool failed = status != NULL && strcmp(status, "failed") == 0;
    if (failed) {
        if (state->failed < UINT32_MAX) ++state->failed;
        codex_background_result_store(state, hash,
                                      CODEX_BACKGROUND_RESULT_FAILED_UNREAD);
    } else {
        if (state->done < UINT32_MAX) ++state->done;
        codex_background_result_store(state, hash,
                                      CODEX_BACKGROUND_RESULT_DONE_UNREAD);
    }
    return true;
}

/* Clears the local unread badge for one result. The host sends read_state to
 * every other device after any device acknowledges a message. */
static inline bool codex_background_mark_read(
    codex_background_state_t *state,
    const char *message_id
)
{
    if (state == NULL || message_id == NULL || message_id[0] == '\0') return false;
    const uint32_t hash = codex_background_hash(message_id);
    for (size_t index = 0; index < CODEX_BACKGROUND_RESULT_MAX; ++index) {
        codex_background_result_t *result = &state->results[index];
        if (!result->occupied || result->hash != hash) continue;
        if (result->state == CODEX_BACKGROUND_RESULT_DONE_UNREAD) {
            if (state->done > 0U) --state->done;
        } else if (result->state == CODEX_BACKGROUND_RESULT_FAILED_UNREAD) {
            if (state->failed > 0U) --state->failed;
        } else {
            return false;
        }
        result->state = CODEX_BACKGROUND_RESULT_READ;
        return true;
    }
    return false;
}

static inline void codex_background_disconnect(codex_background_state_t *state)
{
    if (state == NULL) return;
    state->connected = false;
    state->running = 0U;
}

static inline void codex_background_reset(codex_background_state_t *state)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
}
