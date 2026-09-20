/* Controllable host clock backend — see host_clock.h.
 * Defines the test runner's single esp_timer_get_time(). */
#include "host_clock.h"
#include "esp_timer.h"

static int64_t s_now_us = 0;

int64_t esp_timer_get_time(void)       { return s_now_us; }
void    host_clock_reset(void)         { s_now_us = 0; }
void    host_clock_advance_ms(uint32_t ms) { s_now_us += (int64_t)ms * 1000; }
