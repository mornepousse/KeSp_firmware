#pragma once
/* oled_kpm.h — Sliding KPM (keys per minute) window shared between
 * the HOME screen (KPM bar) and the STATS screen (sparkline).
 *
 * Model: circular window of OLED_KPM_WINDOW seconds.  Every second,
 * oled_kpm_tick() pushes the keystroke counter into the history and
 * resets it to zero.  oled_kpm_value() returns the sum of the last 60
 * seconds (= KPM over the sliding minute).
 */
#include <stdint.h>

#define OLED_KPM_WINDOW     60u    /* seconds in the sliding window */
#define OLED_KPM_SAMPLE_MS  1000u  /* duration of one sample (ms) */
#define OLED_KPM_MAX        400u   /* ceiling for the bar / sparkline */

/* Resets everything to zero (called from oled_screens_reset). */
void oled_kpm_reset(void);

/* Increments the current counter (called on every keystroke). */
void oled_kpm_notify_keypress(void);

/* Advances the window's clock (called from oled_screens_tick).
 * Does nothing if less than one second has elapsed since the last tick. */
void oled_kpm_tick(uint32_t now_ms);

/* Returns the current KPM (sum of the last OLED_KPM_WINDOW samples). */
uint32_t oled_kpm_value(void);

/* Returns a pointer to the raw circular history (OLED_KPM_WINDOW entries).
 * Useful for the sparkline in the STATS screen.  Read-only; valid until the
 * next call to oled_kpm_reset(). */
const uint32_t *oled_kpm_history(void);
