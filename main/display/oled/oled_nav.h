#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    OLED_SCR_SPLASH, OLED_SCR_HOME, OLED_SCR_STATS, OLED_SCR_COUNT
} oled_screen_id_t;

typedef enum {
    OLED_EV_BOOT, OLED_EV_LAYER_CHANGED, OLED_EV_DISP_KEY, OLED_EV_ACTIVITY
} oled_nav_event_t;

#define OLED_NAV_SPLASH_MS  2000u

/* Resets the state machine. resting=HOME. Does NOT ARM the splash (only
   OLED_EV_BOOT arms it, on a real startup only) -> no splash on
   wake/refresh. */
void oled_nav_init(uint32_t now_ms);
void oled_nav_event(oled_nav_event_t ev, uint32_t now_ms);
oled_screen_id_t oled_nav_active(uint32_t now_ms);
