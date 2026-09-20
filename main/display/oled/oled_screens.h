#pragma once
/* oled_screens.h — OLED screen manager driven by oled_nav.
 *
 * Usage (from status_display / oled_backend — wired in Task 13):
 *   oled_screens_reset(now_ms)        — on init / on wake
 *   oled_screens_tick(now_ms)         — every frame, LVGL lock held by the caller
 *   oled_screens_layer_changed(now)   — forwards OLED_EV_LAYER_CHANGED to oled_nav
 *   oled_screens_disp_key(now)        — forwards OLED_EV_DISP_KEY to oled_nav
 *   oled_screens_activity(now)        — forwards OLED_EV_ACTIVITY to oled_nav
 */
#include <stdint.h>

/* Initializes navigation and destroys the current screen if it exists.
 * Call with the LVGL lock held. */
void oled_screens_reset(uint32_t now_ms);

/* Advances the KPM, detects a change of active screen (nav), build/destroy as needed,
 * then calls update() on the current screen.
 * Must be called with the LVGL lock held. */
void oled_screens_tick(uint32_t now_ms);

/* Navigation events (forwarded to oled_nav_event). */
/* Arms the boot splash (call ONCE, at real startup). */
void oled_screens_boot(uint32_t now_ms);
void oled_screens_layer_changed(uint32_t now_ms);
void oled_screens_disp_key(uint32_t now_ms);
void oled_screens_activity(uint32_t now_ms);
