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
    /* 2026-10-22 产品口径(用户明确指定), 优先级从高到低:
     *   有任务运行中                    -> BUSY
     *   没有运行中、有已完成             -> DONE
     *   没有运行中、没有完成、有失败     -> FAIL
     *   都没有                          -> IDLE
     *
     * 旧实现把 failed 判在 running 前面: 只要历史里还剩一个未读失败,
     * 哪怕此刻正有任务在跑, 首页机器人也一直显示"失败" —— 这正是
     * "明明在跑却显示不对"的直接原因。running 必须最先判定。
     * 完成/失败同时存在时按用户给定顺序取 DONE。 */
    if (state->running > 0U) return CODEX_BACKGROUND_BUSY;
    if (state->done > 0U) return CODEX_BACKGROUND_DONE;
    if (state->failed > 0U) return CODEX_BACKGROUND_FAIL;
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
    if (state == NULL) return;
    state->connected = true;
    if (kind == NULL ||
        (strcmp(kind, "status") != 0 && strcmp(kind, "task") != 0)) {
        return;
    }
    // A single status/task record carries its own status word, not an
    // aggregate tally. Trust the word first: "working" means at least one turn
    // is live, and a terminal/idle word clears the count. Only fall back to the
    // caller-supplied running value when the status is unknown, so a legacy
    // caller that supplies an explicit count keeps working.
    if (status != NULL && strcmp(status, "working") == 0) {
        state->running = running > 0U ? running : 1U;
    } else if (status != NULL &&
               (strcmp(status, "idle") == 0 || strcmp(status, "completed") == 0 ||
                strcmp(status, "failed") == 0 || strcmp(status, "interrupted") == 0 ||
                strcmp(status, "done") == 0)) {
        state->running = 0U;
    } else {
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
    /* ⚠ 2026-10-22: 这里原来无条件 `state->running = 0U;` —— 一条终态结果
     * 只代表**那一条**任务结束, 不代表全局没有任务在跑。多任务并发时它会把
     * "运行中"清零, 首页机器人瞬间掉出 BUSY, 正是"明明在跑却显示不对"。
     * running 只由 Bridge 的聚合计数(Bridge 权威)改写。 */
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

/* Single convergence entry point for every state-bearing notification the
 * CBDP layer receives. Keeping the dispatch in one place means the home pet,
 * the home badge counters and the background completion alert all update from
 * the same rules no matter which transport or event carried the update, and a
 * replayed immutable event stays idempotent.
 *
 * kind:
 *   "state"  -- aggregate counters (running/done/failed) carried by one notice
 *   "task"   -- a single task record's live status
 *   "status" -- a live status record
 *   "result" -- an immutable terminal record, deduplicated by event identity
 *
 * message_id is required for "result" (it is the dedup key); running is only
 * consulted for "state" / "task" / "status".
 * Returns true when the notice changed or newly recorded state. */
static inline bool codex_background_apply_notice(
    codex_background_state_t *state,
    const char *kind,
    const char *message_id,
    const char *status,
    uint32_t running
)
{
    if (state == NULL || kind == NULL) return false;
    if (strcmp(kind, "result") == 0) {
        return codex_background_apply_result(state, message_id, status);
    }
    if (strcmp(kind, "state") == 0) {
        // Aggregate tally: the notice already carries the authoritative count,
        // so record it verbatim.
        const bool changed = !state->connected || state->running != running;
        state->connected = true;
        state->running = running;
        return changed;
    }
    if (strcmp(kind, "task") == 0 || strcmp(kind, "status") == 0) {
        const bool changed = !state->connected || state->running != running;
        codex_background_apply(state, kind, status, running);
        return changed;
    }
    return false;
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
