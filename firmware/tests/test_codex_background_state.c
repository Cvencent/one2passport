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

    return 0;
}
