#pragma once
#include <stdint.h>

/* Grace to leave the keyboard_button driver, recreated on wake, before
 * concluding that a captured key was released "before the first scan".
 *
 * The driver only reports a PRESSED after `debounce_ticks` consecutive
 * scans spaced by `interval_us` (keyboard_button.c, CALL_EVENT_CB on
 * change only). The old code waited `vTaskDelay(1)` believing it was
 * waiting 10 ms — but a bare tick waits until the NEXT tick boundary,
 * which is anything between ~0 and 10 ms: when the phase fell badly, the
 * driver had not finished its debounce, its silence was taken for a
 * release, and a HELD key was released at the dongle 20 ms after wake
 * (bench 2026-09-13: Super held → tap of Super seen by the host → launcher
 * opened → Super+F lost).
 *
 * Formula: debounce + 2 scans (task startup, timer phase)
 * + 10 ms of creation margin, floor 10 ms (never less than before),
 * ceiling 50 ms (a silent driver does not delay the wake beyond that). The
 * caller exits BEFORE the deadline as soon as the driver has spoken: the
 * grace only costs its full price if the key was actually released.
 *
 * Pure, tested on host (test/test_wake_grace.c). */
static inline uint32_t wake_grace_ms(uint32_t debounce_ticks, uint32_t interval_us)
{
    uint32_t anti_rebond_ms = ((debounce_ticks + 2u) * interval_us + 999u) / 1000u;
    uint32_t ms = anti_rebond_ms + 10u;
    if (ms < 10u) ms = 10u;
    if (ms > 50u) ms = 50u;
    return ms;
}
