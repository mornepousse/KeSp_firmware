#include "sec_confirm.h"

/* CONCURRENCY MODEL (Phase-2 pentest, 2026-06-11) — why no lock is needed.
 * sec_confirm is a pure module (host-tested, no FreeRTOS). The three entry
 * points run from at most TWO contexts, and the security personalities are
 * mutually exclusive at build (OpenPGP CCID xor OTP-HID), so per build:
 *   - arm()  + poll()  run on the SAME task (ccid_worker in OpenPGP, tud_task
 *     in OTP) — serialized with each other, so the (state,slot) pair is always
 *     written and read consistently.
 *   - authorize() runs on keyboard_task (the K_SEC_CONFIRM keypress), which is
 *     the ONLY cross-task writer. It performs a single PENDING->AUTHORIZED flip
 *     and never touches s_slot.
 * Each field is a byte / aligned word (atomic per-access on the LX7). The only
 * race is authorize() vs poll()'s timeout branch at the 15 s boundary; its
 * worst case is a real touch accepted at T+15.0s instead of rejected (or
 * vice-versa) — benign, since the user physically touched for the armed slot
 * (and the caller re-checks the slot). No touch can be fabricated and no wrong
 * slot can be granted. If a SECOND poll context is ever added (two enabled
 * personalities, or a concurrent admin path), wrap the read-modify-write
 * sections in a portMUX critical section (gated for the host build). */

static sec_confirm_state_t s_state    = SEC_CONFIRM_IDLE;
static uint8_t             s_slot     = 0;
static uint32_t            s_armed_ms = 0;

void sec_confirm_reset(void)
{
    s_state    = SEC_CONFIRM_IDLE;
    s_slot     = 0;
    s_armed_ms = 0;
}

void sec_confirm_arm(uint8_t slot, uint32_t now_ms)
{
    s_state    = SEC_CONFIRM_PENDING;
    s_slot     = slot;
    s_armed_ms = now_ms;
}

void sec_confirm_authorize(void)
{
    if (s_state == SEC_CONFIRM_PENDING)
        s_state = SEC_CONFIRM_AUTHORIZED;
}

sec_confirm_state_t sec_confirm_poll(uint32_t now_ms, uint8_t *out_slot)
{
    if (s_state == SEC_CONFIRM_PENDING &&
        (now_ms - s_armed_ms) >= SEC_CONFIRM_TIMEOUT_MS) {
        s_state = SEC_CONFIRM_IDLE;
        return SEC_CONFIRM_TIMEDOUT;
    }
    if (s_state == SEC_CONFIRM_AUTHORIZED) {
        if (out_slot) *out_slot = s_slot;
        s_state = SEC_CONFIRM_IDLE;
        return SEC_CONFIRM_AUTHORIZED;
    }
    return s_state;
}
