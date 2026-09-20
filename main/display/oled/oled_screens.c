/* oled_screens.c — OLED screen manager driven by oled_nav.
 *
 * Static registry of the 5 screens (SPLASH, HOME, LAYER, STATS, TAMA).
 * On each tick: compares the screen requested by oled_nav_active() with
 * the current screen; if different -> destroy + build; then update().
 * The LVGL lock is held by the caller (no lvgl_port_lock here).
 *
 * oled_make_card() is defined here (taken from the old oled_backend.c)
 * and exported via screens/oled_screen.h so the screens can
 * call it in their build().
 */
#include "oled_screens.h"
#include "oled_nav.h"
#include "oled_kpm.h"
#include "screens/oled_screen.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

/* ── Screen externs ──────────────────────────────────────────────── */
extern const oled_screen_t screen_splash;
extern const oled_screen_t screen_home;
extern const oled_screen_t screen_stats;

/* ── Registre statique ──────────────────────────────────────────────── */
static const oled_screen_t *s_screens[OLED_SCR_COUNT] = {
    [OLED_SCR_SPLASH] = &screen_splash,
    [OLED_SCR_HOME]   = &screen_home,
    [OLED_SCR_STATS]  = &screen_stats,
};

static oled_screen_id_t s_current = OLED_SCR_COUNT; /* no screen built */

/* ── oled_make_card (moved from the old oled_backend) ──────────── */

lv_obj_t *oled_make_card(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_opa(card, LV_OPA_0, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0); /* black = pixel ON = lit */
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/* ── Manager ────────────────────────────────────────────────────────── */

void oled_screens_reset(uint32_t now_ms)
{
    if (s_current < OLED_SCR_COUNT)
        s_screens[s_current]->destroy();
    s_current = OLED_SCR_COUNT;

    oled_kpm_reset();
    oled_nav_init(now_ms);
}

void oled_screens_tick(uint32_t now_ms)
{
    oled_kpm_tick(now_ms);

    oled_screen_id_t id = oled_nav_active(now_ms);

    /* Rebuild only on screen change (§3.3). */
    if (id != s_current) {
        if (s_current < OLED_SCR_COUNT)
            s_screens[s_current]->destroy();
        /* Clean surface before the build: some renderers (tama_render) do not
         * delete their own LVGL objects in destroy() -> without this
         * clean, a leftover from the previous screen would remain on top of
         * the new one. Guarantees a clean slate regardless of the outgoing screen. */
        lv_obj_clean(lv_scr_act());
        lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);  /* no scrollbar/pan */
        s_screens[id]->build(lv_scr_act());
        s_current = id;
    }

    s_screens[s_current]->update();
}

void oled_screens_boot(uint32_t now_ms)          { oled_nav_event(OLED_EV_BOOT,          now_ms); }
void oled_screens_layer_changed(uint32_t now_ms) { oled_nav_event(OLED_EV_LAYER_CHANGED, now_ms); }
void oled_screens_disp_key(uint32_t now_ms)      { oled_nav_event(OLED_EV_DISP_KEY,     now_ms); }
void oled_screens_activity(uint32_t now_ms)      { oled_nav_event(OLED_EV_ACTIVITY,      now_ms); }
