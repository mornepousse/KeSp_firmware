#pragma once
/* Per-key debounce of the matrix driver — pure state machine, one call per
 * key per scan (keyboard_button.c), host-tested (test/test_kbd_debounce.c).
 *
 * Two modes (keyboard_btn_config_t.debounce_eager_press, set from
 * CONFIG_KASE_DEBOUNCE_EAGER_PRESS):
 *
 *  - SYMMETRIC (eager = false; V1/V2/V2D, the historical driver, kept scan
 *    for scan — the test replays the old loop as its oracle): a change, press
 *    or release, is reported after `ticks` consecutive scans that disagree
 *    with the reported level; one agreeing scan restarts the count.
 *
 *  - EAGER PRESS / DEFERRED RELEASE (eager = true; Niphargus halves), QMK's
 *    asym_eager_defer_pk: a PRESS is reported on the FIRST scan that reads the
 *    key down, then the key ignores its contacts for `ticks` scans (lockout:
 *    the press chatter cannot turn into a release + re-press). A RELEASE is
 *    reported after `ticks` consecutive up scans — a down scan in between
 *    cancels it (false alarm), exactly like the symmetric release. With
 *    ticks = 2 at 5 ms: the press costs 0 ms of debounce instead of 5-10, the
 *    release keeps its 10 ms of stability.
 *
 * Failure mode of the eager press, by design: it cannot know the next scan,
 * so a single-scan glitch on a key at rest (EMI, a line at threshold) is
 * reported — a phantom tap held 2*ticks-1 scans. QMK accepts the same
 * trade-off; the symmetric mode filters it. The sleep wake capture keeps its
 * own two-pass AND, so a phantom WAKE still types nothing. A release bounce
 * longer than the release window (ticks up scans, 10 ms) would read as a
 * re-press: there is no lockout after a release (that is what keeps a fast
 * real re-press immediate); the dongle counts re-presses < 30 ms
 * (RF_STATUS[43..46]) to see it on the bench.
 *
 * No absolute scan counter: `cnt` only runs inside a window, a key held for
 * hours never wraps anything. */
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t level;   /* reported level: 1 = pressed */
    uint8_t cnt;     /* scans counted in the current window */
    uint8_t lock;    /* eager: lockout running after a reported press */
} kbd_db_key_t;

typedef enum { KBD_DB_NONE = 0, KBD_DB_PRESS, KBD_DB_RELEASE } kbd_db_event_t;

static inline kbd_db_event_t kbd_db_step(kbd_db_key_t *k, bool raw_down, uint32_t ticks, bool eager)
{
    if (!eager) {
        if (raw_down != (bool)k->level) {
            if (++k->cnt >= ticks) {
                k->cnt = 0;
                k->level = raw_down;
                return raw_down ? KBD_DB_PRESS : KBD_DB_RELEASE;
            }
        } else {
            k->cnt = 0;
        }
        return KBD_DB_NONE;
    }

    if (k->lock) {                       /* lockout after an eager press */
        if (++k->cnt < ticks) return KBD_DB_NONE;
        k->lock = 0;                     /* over: this scan is evaluated */
        k->cnt = 0;
    }
    if (raw_down == (bool)k->level) {    /* agrees: a pending release was a false alarm */
        k->cnt = 0;
        return KBD_DB_NONE;
    }
    if (raw_down) {                      /* press: at once, then lockout */
        k->level = 1;
        k->lock = 1;
        k->cnt = 0;
        return KBD_DB_PRESS;
    }
    if (++k->cnt >= ticks) {             /* release: after `ticks` up scans */
        k->level = 0;
        k->cnt = 0;
        return KBD_DB_RELEASE;
    }
    return KBD_DB_NONE;
}
