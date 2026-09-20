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
 * costs nothing for the first keystroke, it only delays the
 * timers — hence the activity window, wider than the longest
 * of them. */


static inline uint32_t kbd_cadence_attente_ms(uint32_t now_ms, uint32_t derniere_activite_ms,
                                              bool usb_present, bool test_matrice)
{
    if (usb_present || test_matrice) return KBD_CADENCE_ACTIF_MS;
    if ((uint32_t)(now_ms - derniere_activite_ms) < KBD_CADENCE_FENETRE_MS) return KBD_CADENCE_ACTIF_MS;
    return KBD_CADENCE_REPOS_MS;
}
