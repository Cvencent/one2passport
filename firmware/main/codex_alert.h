#pragma once

#include <stdbool.h>
#include <stdint.h>

/* One finished Codex task is reported to the firmware through several
 * independent channels, and they do not arrive together:
 *
 *   - the terminal turn event (arrives immediately)
 *   - the aggregate done/failed counters, which only ring after the settle
 *     window because the unread numbers oscillate while other clients read
 *   - the foreground UI update, driven by whichever transport delivered first
 *
 * Each channel used to play its own sound, so a single completion produced two
 * or three tones. This gate lets exactly one of them through per completion.
 *
 * The decision is a pure function so it can be unit tested on the host.
 */
typedef struct {
    int64_t claimed_us; /* 0 means "nothing claimed yet" */
} codex_alert_gate_t;

/* Must exceed the aggregate-counter settle window (4s in coding_bridge.c),
 * because that channel legitimately rings several seconds after the
 * terminal turn event for the same completion. Two genuinely separate
 * completions landing inside this window are announced as one; that is a
 * deliberate trade for never triple-ringing a single task. */
#define CODEX_ALERT_DEDUP_US (6LL * 1000 * 1000)

/* Returns true when this caller wins the right to ring, and records the claim.
 * Returns false when another channel already rang for the same completion. */
static inline bool codex_alert_claim(codex_alert_gate_t *gate, int64_t now_us)
{
    if (gate == NULL) return false;
    if (gate->claimed_us != 0 &&
        now_us - gate->claimed_us < CODEX_ALERT_DEDUP_US) {
        return false;
    }
    gate->claimed_us = now_us;
    return true;
}
