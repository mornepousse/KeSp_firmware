#include "oled_nav.h"

static oled_screen_id_t s_resting;
static uint32_t s_splash_until;

void oled_nav_init(uint32_t now_ms) {
    (void)now_ms;
    s_resting      = OLED_SCR_HOME;
    s_splash_until = 0;   /* no splash on wake — armed by OLED_EV_BOOT */
}

oled_screen_id_t oled_nav_active(uint32_t now_ms) {
    if (now_ms < s_splash_until) return OLED_SCR_SPLASH;
    return s_resting;
}

static oled_screen_id_t next_resting(oled_screen_id_t r) {
    return (r == OLED_SCR_HOME) ? OLED_SCR_STATS : OLED_SCR_HOME;
}

void oled_nav_event(oled_nav_event_t ev, uint32_t now_ms) {
    switch (ev) {
    case OLED_EV_BOOT:
        s_splash_until = now_ms + OLED_NAV_SPLASH_MS;
        break;
    case OLED_EV_DISP_KEY:
        s_resting      = next_resting(s_resting);
        s_splash_until = 0;   /* cut the splash */
        break;
    case OLED_EV_LAYER_CHANGED:
    case OLED_EV_ACTIVITY:
        break;   /* no more idle-tama → these events no longer have any effect */
    }
}
