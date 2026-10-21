#pragma once
/* Dongle engine wake — pure decision (test/test_engine_wake.c).
 *
 * The engine task used to sleep a flat 10 ms between rounds, so a fused
 * state pushed by the rx task waited 0-10 ms (5 ms on average) before being
 * played: the largest single term of the keystroke latency after the halves'
 * debounce (2026-10-21). Now dongle_engine_on_matrix() NOTIFIES the task as
 * soon as it queues a transition, and the task blocks only for what is left
 * until its next TIMER round (tap-hold, tap-dance, leader, release on
 * silence) — 10 ms after the previous timer round, whatever frames woke it
 * in between. A frame never delays a timer, a timer never delays a frame.
 *
 * The clock is now_ms() (esp_timer / 1000, truncated to 32 bits): the
 * difference is taken modulo 2^32, so the 49.7-day wrap is invisible. */
#include <stdint.h>
#include <stdbool.h>

/* Timer round period: tap-hold / tap-dance / leader cadence, same as the
 * keyboard task's active cadence. The dongle runs FreeRTOS at 1 kHz with no
 * PM, so a wait of N ms is N ticks exactly. */
#define DONGLE_ENGINE_TIMER_MS 10u

/* Is the timer round due? true once period_ms have elapsed since last_ms. */
static inline bool engine_timer_due(uint32_t last_ms, uint32_t now_ms, uint32_t period_ms)
{
    return (uint32_t)(now_ms - last_ms) >= period_ms;
}

/* How long to block (waiting for a frame notification) before the next timer
 * round: 0 when it is due (or late — never negative, never a full period of
 * catch-up). engine_timer_due() <=> engine_wait_ms() == 0. */
static inline uint32_t engine_wait_ms(uint32_t last_ms, uint32_t now_ms, uint32_t period_ms)
{
    uint32_t elapsed = (uint32_t)(now_ms - last_ms);
    return elapsed >= period_ms ? 0u : period_ms - elapsed;
}
