/* Controllable monotonic clock for host tests. Backs
 * esp_timer_get_time() (defined in host_clock.c — a single symbol for the
 * whole runner) so that timing-based modules (tap_hold, tap_dance, …) can be
 * driven deterministically: host_clock_reset() then host_clock_advance_ms(). */
#pragma once
#include <stdint.h>

void host_clock_reset(void);              /* reset the clock to 0 */
void host_clock_advance_ms(uint32_t ms);  /* advance by ms milliseconds */
