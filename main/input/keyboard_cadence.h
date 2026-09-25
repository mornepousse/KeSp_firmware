#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cadence.h"   /* KBD_CADENCE_* */

/* Keyboard task cadence (pure logic, tested on host).
 *
 * vTaskKeyboard's loop only needs its 10 ms to keep timers alive: tap-hold
 * and tap-dance (200 ms), leader (1000 ms), matrix test mode, and remote
 * fusion when the left half types over USB. At rest, none of that is
 * running — and yet a 10 ms loop (a tick at 100 Hz) wakes the processor a
 * hundred times a second and prevents ESP-IDF's automatic light sleep,
 * which requires CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP (3) ticks with
 * nobody around: measured on the bench on 2026-09-16, SLEEP mode 92% of
 * idle time and light_sleep_counts = 0.
 *
 * A matrix change notifies the task (xTaskNotifyGive): the long wait
 * costs nothing for the first keystroke, it only delays the timers.
 *
 * So the 10 ms is kept only while a timer is ACTUALLY waiting on the clock
 * (2026-09-25). It used to be a 1.5 s window after ANY activity, "in case":
 * typing every ~200 ms, the window never closed and the half never slept
 * while typing — ~20 mA across a working day, measured. The four consumers
 * of the tick, inventoried in keyboard_task.c's loop: tap-hold undecided
 * (tap_hold_pending), tap-dance counting (tap_dance_pending), leader
 * sequence (leader_is_active), macro queued (played on the next turn).
 * Combos do not need it (evaluated on matrix changes only), nor do one-shot
 * or caps word (no timeout). A new timed feature MUST join that list. */
static inline uint32_t kbd_cadence_attente_ms(bool usb_present, bool test_matrice,
                                              bool minuteur_en_cours)
{
    if (usb_present || test_matrice || minuteur_en_cours) return KBD_CADENCE_ACTIF_MS;
    return KBD_CADENCE_REPOS_MS;
}
