/* en_label.h — Host-safe dashboard label formatters, extracted from eink_lvgl.c.
 * Pure string functions: no LVGL objects, no FreeRTOS, no hardware deps.
 * Safe to link in host tests.
 *
 * LVGL symbol fallbacks: identical byte sequences to the real LVGL definitions.
 * In firmware builds, en_label.c includes lvgl.h first so these #ifndef guards
 * are never reached. In host builds (TEST_HOST defined), the fallbacks kick in. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef LV_SYMBOL_WIFI
#define LV_SYMBOL_WIFI "\xEF\x87\xAB"   /* FontAwesome U+F1EB — matches lv_symbol_def.h */
#endif
#ifndef LV_SYMBOL_USB
#define LV_SYMBOL_USB  "\xEF\x8a\x87"   /* FontAwesome U+F287 — matches lv_symbol_def.h */
#endif

/* en_build_link_label — build the link quality info-line for the e-ink dashboard.
 * Output: LV_SYMBOL_WIFI " <side> <q255>/255 --%"  when dongle_alive
 *         LV_SYMBOL_WIFI " <side> --/255 --%"       when !dongle_alive
 * side: 'L' or 'R'.  q255: raw link quality 0..255.  buf >= 28 bytes. */
void en_build_link_label(char *buf, size_t bufsz, char side,
                         bool dongle_alive, uint8_t q255);

/* en_build_usb_label — build the USB status info-line for the e-ink dashboard.
 * Output: LV_SYMBOL_USB " on"  / " off"  when dongle_alive (usb_active selects)
 *         LV_SYMBOL_USB " ?"             when !dongle_alive
 * buf >= 16 bytes. */
void en_build_usb_label(char *buf, size_t bufsz,
                        bool dongle_alive, bool usb_active);
