/* V2D wireless RF light-sleep + matrix-keypress wake.
 * See docs/superpowers/specs/2026-06-28-v2d-rf-light-sleep-design.md.
 * Only meaningful on CONFIG_KASE_KBD_WIRELESS. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Pure trigger predicate (host-testable): sleep only on the RF path, after the
 * idle window has elapsed. On USB (route not RF) we never sleep. */
static inline bool v2d_should_sleep(bool route_is_rf, uint32_t idle_ms,
                                    uint32_t threshold_ms)
{
    return route_is_rf && idle_ms >= threshold_ms;
}

#ifndef TEST_HOST
/* Quiesce all subsystems, enter light-sleep, and restore on a matrix keypress.
 * Blocks until woken. Call from the keyboard task context. */
void v2d_sleep_enter(void);
#endif
