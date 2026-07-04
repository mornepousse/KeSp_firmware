/* en_label.c — Pure string formatters for the e-ink dashboard.
 * See en_label.h for the contract.
 *
 * Compiled in both firmware (CONFIG_KASE_HAS_EINK boards) and host test builds.
 * No LVGL objects, no FreeRTOS, no hardware deps beyond <stdio.h>. */

/* In firmware builds, include lvgl.h first so LVGL's LV_SYMBOL_* definitions
 * take precedence over the fallbacks defined in en_label.h. */
#ifndef TEST_HOST
#include "lvgl.h"
#endif

#include "en_label.h"
#include <stdio.h>

void en_build_link_label(char *buf, size_t bufsz, char side,
                         bool dongle_alive, uint8_t q255)
{
    if (!dongle_alive)
        snprintf(buf, bufsz, LV_SYMBOL_WIFI " %c --/255 --%%", side);
    else
        snprintf(buf, bufsz, LV_SYMBOL_WIFI " %c %u/255 --%%", side, (unsigned)q255);
}

void en_build_usb_label(char *buf, size_t bufsz,
                        bool dongle_alive, bool usb_active)
{
    const char *s = dongle_alive ? (usb_active ? "on" : "off") : "?";
    snprintf(buf, bufsz, LV_SYMBOL_USB " %s", s);
}
