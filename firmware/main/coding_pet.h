#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CODING_PET_IDLE = 0,
    CODING_PET_BUSY,
    CODING_PET_DONE,
    CODING_PET_FAIL,
    CODING_PET_OFFLINE,
    CODING_PET_ATTENTION,
} coding_pet_state_t;

typedef struct {
    bool connected;
    bool synced;
    uint32_t running;
    uint32_t done;
    uint32_t failed;
} coding_pet_t;

static inline void coding_pet_update(coding_pet_t *pet, uint32_t running,
                                     uint32_t done, uint32_t failed)
{
    pet->synced = true;
    pet->running = running;
    pet->done = done;
    pet->failed = failed;
}

static inline coding_pet_state_t coding_pet_state(const coding_pet_t *pet)
{
    /* 与 codex_background_pet_state() 保持同一优先级: 运行中 > 完成 > 失败. */
    if (pet->connected && pet->running) return CODING_PET_BUSY;
    if (pet->done) return CODING_PET_DONE;
    if (pet->failed) return CODING_PET_FAIL;
    return pet->connected ? CODING_PET_IDLE : CODING_PET_OFFLINE;
}
