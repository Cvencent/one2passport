#include <assert.h>

#include "codex_background_state.h"

int main(void)
{
    codex_background_state_t state = {0};

    codex_background_apply(&state, "status", "working", 0);
    assert(state.connected);
    assert(state.running == 1U);
    assert(codex_background_pet_state(&state) == CODEX_BACKGROUND_BUSY);

    codex_background_apply(&state, "status", "idle", 0);
    assert(state.running == 0U);
    assert(codex_background_pet_state(&state) == CODEX_BACKGROUND_IDLE);

    codex_background_apply(&state, "task", "working", 1);
    assert(state.running == 1U);
    assert(codex_background_pet_state(&state) == CODEX_BACKGROUND_BUSY);

    codex_background_apply(&state, "task", "idle", 0);
    assert(state.running == 0U);

    assert(codex_background_apply_result(&state, "result:t1:1", "completed"));
    assert(!codex_background_apply_result(&state, "result:t1:1", "completed"));
    assert(state.done == 1U);
    assert(codex_background_pet_state(&state) == CODEX_BACKGROUND_DONE);

    assert(codex_background_mark_read(&state, "result:t1:1"));
    assert(!codex_background_mark_read(&state, "result:t1:1"));
    assert(state.done == 0U);
    assert(codex_background_pet_state(&state) == CODEX_BACKGROUND_IDLE);

    assert(codex_background_apply_result(&state, "result:t2:2", "failed"));
    assert(state.failed == 1U);
    assert(codex_background_pet_state(&state) == CODEX_BACKGROUND_FAIL);

    assert(codex_background_mark_read(&state, "result:t2:2"));
    assert(state.failed == 0U);
    assert(codex_background_pet_state(&state) == CODEX_BACKGROUND_IDLE);

    codex_background_disconnect(&state);
    assert(!state.connected);
    assert(codex_background_pet_state(&state) == CODEX_BACKGROUND_OFFLINE);

    codex_background_state_t fresh = {0};
    assert(codex_background_apply_result(&fresh, "result:first", "completed"));
    assert(fresh.done == 1U);
    assert(codex_background_mark_read(&fresh, "result:first"));
    assert(fresh.done == 0U);

    /* The CBDP layer must drive one convergence helper for every state-bearing
     * notification, so the home pet/badge and the background completion alert
     * work even while the full Codex page is closed. */
    codex_background_state_t routed = {0};
    assert(codex_background_apply_notice(&routed, "state", NULL, NULL, 2U));
    assert(routed.running == 2U);
    assert(routed.connected);
    assert(codex_background_pet_state(&routed) == CODEX_BACKGROUND_BUSY);
    assert(codex_background_apply_notice(&routed, "state", NULL, NULL, 0U));
    assert(routed.running == 0U);
    assert(codex_background_apply_notice(&routed, "result", "evt:1", "completed", 0U));
    assert(routed.done == 1U);
    assert(codex_background_pet_state(&routed) == CODEX_BACKGROUND_DONE);
    /* A replay of the same immutable result must not double count or re-alert. */
    assert(!codex_background_apply_notice(&routed, "result", "evt:1", "completed", 0U));
    assert(routed.done == 1U);
    assert(codex_background_apply_notice(&routed, "result", "evt:2", "failed", 0U));
    assert(routed.failed == 1U);
    assert(codex_background_pet_state(&routed) == CODEX_BACKGROUND_FAIL);
    /* A notice without an identity cannot be tracked; it must be rejected
     * rather than silently inflating the unread badge. */
    assert(!codex_background_apply_notice(&routed, "result", "", "completed", 0U));
    assert(routed.done == 1U);
    assert(!codex_background_apply_notice(&routed, "unknown", NULL, NULL, 0U));

    return 0;
}
