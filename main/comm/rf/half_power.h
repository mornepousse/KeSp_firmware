#ifndef HALF_POWER_H
#define HALF_POWER_H

#include <stdint.h>

/* Power state of a keyboard half, by recent keyboard activity. */
typedef enum {
    HALF_POWER_ACTIVE = 0,   /* recently typed: full rate */
    HALF_POWER_THROTTLE,     /* idle a few seconds: reduced rate */
    HALF_POWER_SLEEP,        /* idle long: deep rate (real light-sleep in Phase 2) */
} half_power_state_t;

/* Idle thresholds in milliseconds (tunable). */
#define HALF_POWER_T_THROTTLE_MS  3000u
#define HALF_POWER_T_SLEEP_MS     30000u

/* Pure: decide the power state from elapsed idle time.
 * idle = now_ms - last_activity_ms. Caller passes monotonic ms counters; the
 * unsigned subtraction handles the ~49-day wrap correctly, so do NOT add a
 * `now < last` guard — it would break wrap correctness.
 * idle <  T_THROTTLE  -> ACTIVE
 * idle <  T_SLEEP     -> THROTTLE
 * else                -> SLEEP */
half_power_state_t half_power_next(uint32_t last_activity_ms, uint32_t now_ms);

/* Pure: heartbeat TX divisor for a state — send the heartbeat every Nth 100 ms
 * tick. ACTIVE=1 (100 ms), THROTTLE=5 (500 ms), SLEEP=10 (1 s). */
uint8_t half_power_hb_divisor(half_power_state_t state);

#endif /* HALF_POWER_H */
