/* oled_kpm.c — sliding KPM window (extracted from the old oled_backend). */
#include "oled_kpm.h"
#include <string.h>

static uint32_t s_count;                        /* keystrokes since the last sample */
static uint32_t s_history[OLED_KPM_WINDOW];    /* circular history (values/s) */
static int      s_idx;                          /* next write index */
static uint32_t s_current_kpm;                 /* sum of the last OLED_KPM_WINDOW */
static uint32_t s_last_sample_ms;              /* timestamp of the last sample */

void oled_kpm_reset(void)
{
    s_count          = 0;
    s_idx            = 0;
    s_current_kpm    = 0;
    s_last_sample_ms = 0;
    memset(s_history, 0, sizeof(s_history));
}

void oled_kpm_notify_keypress(void)
{
    s_count++;
}

void oled_kpm_tick(uint32_t now_ms)
{
    /* First call after reset: anchor the time counter. */
    if (s_last_sample_ms == 0) {
        s_last_sample_ms = now_ms;
        return;
    }

    if ((now_ms - s_last_sample_ms) < OLED_KPM_SAMPLE_MS) return;

    /* Push the current sample into the circular window. */
    s_history[s_idx] = s_count;
    s_idx            = (s_idx + 1) % (int)OLED_KPM_WINDOW;
    s_count          = 0;
    s_last_sample_ms = now_ms;

    /* Recompute the KPM: sum of the whole window. */
    uint32_t total = 0;
    for (int i = 0; i < (int)OLED_KPM_WINDOW; i++) total += s_history[i];
    s_current_kpm = total;
}

uint32_t oled_kpm_value(void)
{
    return s_current_kpm;
}

const uint32_t *oled_kpm_history(void)
{
    return s_history;
}
