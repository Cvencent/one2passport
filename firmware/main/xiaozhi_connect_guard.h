#pragma once

#include <stdbool.h>

/*
 * Small ownership guard shared by XiaoZhi connect/cancel paths.
 *
 * The callers serialize every operation with their own critical section, so
 * these helpers intentionally stay lock-free and allocation-free.  A cancel
 * request can arrive while a connect task owns the handle; in that case the
 * owner observes the cancelled flag in finish(), while an idle caller may
 * close immediately.
 */
typedef struct {
    bool active;
    bool cancelled;
} xz_connect_guard_t;

static inline bool xz_connect_guard_begin(xz_connect_guard_t *guard)
{
    if (guard == NULL || guard->active) return false;
    guard->active = true;
    guard->cancelled = false;
    return true;
}

static inline bool xz_connect_guard_finish(xz_connect_guard_t *guard)
{
    if (guard == NULL || !guard->active) return true;
    const bool cancelled = guard->cancelled;
    guard->active = false;
    guard->cancelled = false;
    return cancelled;
}

static inline bool xz_connect_guard_is_active(const xz_connect_guard_t *guard)
{
    return guard != NULL && guard->active;
}

static inline bool xz_connect_guard_is_cancelled(const xz_connect_guard_t *guard)
{
    return guard != NULL && guard->cancelled;
}

static inline bool xz_connect_guard_request_cancel(xz_connect_guard_t *guard)
{
    if (guard == NULL) return true;
    guard->cancelled = true;
    return !guard->active;
}

static inline void xz_connect_guard_reset(xz_connect_guard_t *guard)
{
    if (guard == NULL) return;
    guard->active = false;
    guard->cancelled = false;
}
