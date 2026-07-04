/* Host stub for esp_timer.h — provides the esp_timer_get_time() declaration so
 * production modules that read the monotonic clock (e.g. tap_hold.c) compile and
 * link host-side. The DEFINITION lives in test_tap_hold.c as a controllable
 * clock (g_now_us) so timing tests can advance time deterministically. */
#pragma once
#include <stdint.h>

int64_t esp_timer_get_time(void);
