/* display_backend_t backend of the Sharp memory-LCD — LVGL UI in 68x160 portrait:
 *   banner : route (RF/USB) + ▲ if the dongle ACKs, gauge + local voltage
 *   center : LEFT  = layer name in 4 lines (memlcd_couper_nom) + "Ln"
 *            RIGHT = 60 px centered Niphargus logo
 * (No "other half's battery": the user does not want it, and the
 * ACK channel that would have carried it was removed along with it — 2026-09-14.)
 * LVGL renders in 16 bits into a full-screen buffer (full_refresh); the flush
 * thresholds into the 1-bit portrait buffer and memlcd_panel_show() writes it
 * under the radio bus lock. If the radio holds the bus, the flush keeps the
 * image and update() pushes it again on the next tick: no image is lost, it
 * is only delayed by 100 ms.
 *
 * ⚠ boot order: on the left half "display init" precedes radio init,
 * and it is the RADIO that creates the SPI bus (rf_driver, shares_bus_first).
 * Adding the screen device before it yields ESP_ERR_INVALID_STATE (seen at the
 * bench: screen at 602 ms, radio at 652 ms). Attachment is therefore DEFERRED
 * to the first update() that finds the bus; LVGL itself is built right away. */
#include "memlcd_backend.h"
#include "cadence.h"   /* LVGL_* */
#if CONFIG_KASE_VEILLE
#include "veille_task.h"   /* screen hook */
#endif
#include "memlcd_panel.h"
#include "status_display.h"
#include "board.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"
#include "batt_calc.h"
#endif
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
#include "keyboard_config.h"   /* current_layout, default_layout_names */
#endif
#if CONFIG_KASE_KBD_WIRELESS
#include "usb_presence.h"      /* kbd_active_route */
#include "kbd_relay_tx.h"      /* kbd_relay_dongle_vu */
#endif
#if CONFIG_KASE_HALF_LINK_TX
#include "half_link.h"         /* half_link_tx_dongle_vu */
#endif

static const char *TAG = "memlcd_be";
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_unscii_8);
extern const lv_img_dsc_t img_niphargus_60;

/* ── State ────────────────────────────────────────────────────────── */
static bool s_attached;                 /* panel on the bus */
static bool s_built;                    /* LVGL objects built */
static volatile bool s_sleeping;        /* frozen image: no flush, no VCOM */
static volatile bool s_dirty;           /* thresholded image not yet pushed */
/* ⚠ s_fb is written by the LVGL task (flush) and pushed to the panel
 * either by IT or by the display task (relaunch when the bus was taken). Two
 * tasks in memlcd_panel_show at the same time = one's transposition (which
 * starts by CLEARING the panel buffer) under the feet of the other's send:
 * lines were coming out blank ("part of the screen clears", right half,
 * 2026-09-14). A mutex covers threshold + send. */
static SemaphoreHandle_t s_fb_mux;
static uint8_t s_fb[MEMLCD_H * MEMLCD_LINE_BYTES];            /* portrait, 1 = ink */
static lv_color_t s_draw[MEMLCD_W * MEMLCD_H];                 /* full-screen LVGL render */
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_drv;
static lv_disp_t *s_disp;
static memlcd_model_t s_shown;          /* last drawn model */

/* ── LVGL objects ─────────────────────────────────────────────────── */
static lv_obj_t *s_l_route, *s_bar, *s_l_volt, *s_l_nom[MEMLCD_NOM_LIGNES], *s_l_couche;

#define Y_BANDEAU_FIN 31
#define Y_CENTRE      (Y_BANDEAU_FIN + 1)
#define H_CENTRE      (MEMLCD_H - Y_CENTRE)

static void trait(lv_obj_t *parent, lv_coord_t y)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, y); lv_obj_set_size(o, MEMLCD_W, 1);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static lv_obj_t *texte(lv_obj_t *parent, const lv_font_t *f, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static void construire(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Banner: route + ▲ (Montserrat 14 carries the symbols), gauge on the right,
     * voltage below in UNSCII 8. */
    s_l_route = texte(scr, &lv_font_montserrat_14, 3, 2);
    s_l_volt  = texte(scr, &lv_font_unscii_8, 3, 20);
    s_bar = lv_bar_create(scr);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, 10, 24); lv_obj_set_pos(s_bar, MEMLCD_W - 14, 3);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_border_color(s_bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    trait(scr, Y_BANDEAU_FIN);

    /* Center: the full height below the banner */
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
    /* 3 lines of 18 px + "Ln" 14 px below, the block centered vertically */
    const lv_coord_t bloc = MEMLCD_NOM_LIGNES * 18 + 14 + 8;
    const lv_coord_t y0 = Y_CENTRE + (H_CENTRE - bloc) / 2;
    for (int i = 0; i < MEMLCD_NOM_LIGNES; i++) {
        s_l_nom[i] = texte(scr, &lv_font_montserrat_14, 0, y0 + i * 18);
        lv_obj_set_width(s_l_nom[i], MEMLCD_W);
        lv_obj_set_style_text_align(s_l_nom[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    s_l_couche = texte(scr, &lv_font_unscii_8, 0, y0 + MEMLCD_NOM_LIGNES * 18 + 8);
    lv_obj_set_width(s_l_couche, MEMLCD_W);
    lv_obj_set_style_text_align(s_l_couche, LV_TEXT_ALIGN_CENTER, 0);
#else
    lv_obj_t *img = lv_img_create(scr);
    lv_img_set_src(img, &img_niphargus_60);
    lv_obj_set_style_img_recolor(img, lv_color_black(), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_pos(img, (MEMLCD_W - 60) / 2, Y_CENTRE + (H_CENTRE - 60) / 2);   /* centered */
#endif
    s_built = true;
}

/* ── Model: what the sources say right now ───────────────────────── */
static void lire_modele(memlcd_model_t *m)
{
    memset(m, 0, sizeof *m);
    m->batt_local_dv = 0xFF;
#if CONFIG_KASE_BATT_SENSE
    { uint8_t dv = batt_sense_dv(); m->batt_local_dv = dv ? dv : 0xFF; m->batt_local_chg = batt_sense_charging(); m->batt_niveau = batt_sense_niveau(); }
    /* Low / critical: the voltage stays displayed as-is (no
     * blinking — one more redraw every 2 s for nothing, request from
     * 2026-09-19); the alert is the thickened gauge border. */
#endif
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
    m->is_left   = 1;
#if CONFIG_KASE_KBD_WIRELESS
    m->route_rf  = (kbd_active_route() == KBD_OUT_RF);
    m->dongle_vu = kbd_relay_dongle_vu();
#endif
    m->couche    = current_layout;
    strncpy(m->nom, default_layout_names[current_layout], sizeof m->nom - 1);
#else
    m->is_left   = 0;
    m->route_rf  = 1;
#if CONFIG_KASE_HALF_LINK_TX
    m->dongle_vu = half_link_tx_dongle_vu();
#endif
#endif
}

static void tension(char *out, size_t n, uint8_t dv, uint8_t chg)
{
    if (dv == 0xFF) snprintf(out, n, "?");
    else snprintf(out, n, "%u.%uV%s", dv / 10, dv % 10, chg == 2 ? " #" : (chg == 1 ? " +" : ""));
}

static void dessiner(const memlcd_model_t *m)
{
    char buf[24];
    lv_label_set_text_fmt(s_l_route, "%s%s", m->route_rf ? "RF" : "USB", m->dongle_vu ? " " LV_SYMBOL_UP : "");
    tension(buf, sizeof buf, m->batt_local_dv, m->batt_local_chg);
    lv_label_set_text(s_l_volt, buf);
    uint8_t pct = 0;
#if CONFIG_KASE_BATT_SENSE
    pct = (m->batt_local_dv == 0xFF) ? 0 : batt_soc_pct(m->batt_local_dv);
#endif
    lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
    /* Low battery: gauge border thickened (a background/level inversion
     * made a full bar unreadable — bench 2026-09-19). */
    lv_obj_set_style_border_width(s_bar, m->batt_niveau ? 2 : 1, LV_PART_MAIN);
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
    char lignes[MEMLCD_NOM_LIGNES][MEMLCD_NOM_BUF];
    memlcd_couper_nom(m->nom, lignes);
    for (int i = 0; i < MEMLCD_NOM_LIGNES; i++) lv_label_set_text(s_l_nom[i], lignes[i]);
    lv_label_set_text_fmt(s_l_couche, "L%u", (unsigned)m->couche);
#endif
}

/* ── LVGL → panel ─────────────────────────────────────────────────── */
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    if (s_fb_mux) xSemaphoreTake(s_fb_mux, portMAX_DELAY);   /* held <= ~15 ms by the other task */
    /* full_refresh: the area is the whole screen, px is row by row, 68 wide */
    for (lv_coord_t y = area->y1; y <= area->y2; y++) {
        uint8_t *row = &s_fb[y * MEMLCD_LINE_BYTES];
        for (lv_coord_t x = area->x1; x <= area->x2; x++, px++) {
            if (lv_color_brightness(*px) < 128) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
            else                                row[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));
        }
    }
    if (lv_disp_flush_is_last(drv)) {
        bool ok = !s_sleeping && s_attached && memlcd_panel_show(s_fb);
        s_dirty = !ok;                                   /* pushed again by update() */
    }
    if (s_fb_mux) xSemaphoreGive(s_fb_mux);
    lv_disp_flush_ready(drv);
}

static bool lvgl_pret(void)
{
    if (s_disp) return true;
    if (!s_fb_mux) s_fb_mux = xSemaphoreCreateMutex();
    if (!lv_is_initialized()) {
        /* LVGL tick at 50 ms, task asleep for up to 500 ms: nothing is
         * animated on this panel, and every task wake at rest costs a
         * frequency ramp-up (DFS). A model change is pushed on the next
         * tick of update() (100 ms), unchanged. */
        const lvgl_port_cfg_t cfg = { .task_priority = 2, .task_stack = 6144, .task_affinity = 0,
                                      .task_max_sleep_ms = LVGL_TASK_MAX_SLEEP_MS, .timer_period_ms = LVGL_TICK_MS };
        if (lvgl_port_init(&cfg) != ESP_OK) { ESP_LOGE(TAG, "lvgl_port_init KO"); return false; }
    }
    if (!lvgl_port_lock(200)) return false;
    lv_disp_draw_buf_init(&s_draw_buf, s_draw, NULL, MEMLCD_W * MEMLCD_H);
    lv_disp_drv_init(&s_drv);
    s_drv.hor_res = MEMLCD_W; s_drv.ver_res = MEMLCD_H;
    s_drv.flush_cb = flush_cb; s_drv.draw_buf = &s_draw_buf; s_drv.full_refresh = 1;
    s_disp = lv_disp_drv_register(&s_drv);
    /* The LVGL refresh timer runs at 30 ms by default, even when
     * nothing changes: 33 wakes per second that would forbid automatic
     * light sleep. 200 ms is enough for a status screen (update() invalidates
     * at most every 100 ms). */
    if (s_disp) { lv_timer_t *t = _lv_disp_get_refr_timer(s_disp); if (t) lv_timer_set_period(t, LVGL_REFR_MS); }
    lvgl_port_unlock();
    return s_disp != NULL;
}

static bool try_attach(void)
{
    if (s_attached) return true;
    if (memlcd_panel_init() != ESP_OK) return false;   /* bus not there yet: retry on the next tick */
    s_attached = true;
    memlcd_panel_clear();
    ESP_LOGI(TAG, "panel attached (radio bus ready)");
    s_dirty = true;                                    /* push what LVGL has already rendered */
    return true;
}

/* ── Vtable ───────────────────────────────────────────────────────── */
static void memlcd_sleep(void);
static void memlcd_wake(void);
static bool memlcd_init(void)
{
    if (!lvgl_pret()) return false;
    try_attach();              /* succeeds if the radio is already initialized (right half) */
#if CONFIG_KASE_VEILLE
    /* Sleep (B7): frozen image and VCOM suspended while asleep; on wake,
     * flags only — the screen task pushes the image again on its tick. */
    static const veille_hook_t hook = { "screen", memlcd_sleep, memlcd_wake };
    veille_hook_enregistrer(&hook);
#endif
    return true;               /* never "KO": the attachment happens on the first update() */
}

static void memlcd_refresh_all(void)
{
    if (!s_disp || !lvgl_port_lock(200)) return;
    construire();
    lire_modele(&s_shown);
    dessiner(&s_shown);
    lvgl_port_unlock();
}

static void memlcd_update(void)
{
    if (!s_disp) return;
    try_attach();
    if (s_sleeping) return;
    if (!s_built) { memlcd_refresh_all(); return; }
    memlcd_model_t m; lire_modele(&m);
    /* Voltage: stabilized 30 s (memlcd_batt_aff_step, pure) — filters
     * ADC oscillation without ever freezing a slow drift. */
    static memlcd_batt_aff_t s_baff; static bool s_baff_init;
    if (!s_baff_init) { memlcd_batt_aff_init(&s_baff); s_baff_init = true; }
    m.batt_local_dv = memlcd_batt_aff_step(&s_baff, m.batt_local_dv,
                                           (uint32_t)(esp_timer_get_time() / 1000), 30000);
    if (memlcd_model_diff(&s_shown, &m)) {
        if (lvgl_port_lock(50)) { s_shown = m; dessiner(&m); lvgl_port_unlock(); }
    }
    if (s_dirty && s_attached) {
        static uint16_t s_refus;
        if (s_fb_mux && xSemaphoreTake(s_fb_mux, pdMS_TO_TICKS(20)) != pdTRUE) return;   /* the flush takes care of it */
        if (!s_dirty) { xSemaphoreGive(s_fb_mux); return; }                             /* pushed in the meantime */
        if (memlcd_panel_show(s_fb)) { if (s_refus >= 5) ESP_LOGW(TAG, "image pushed after %u refusals (bus busy)", (unsigned)s_refus); s_refus = 0; s_dirty = false; }
        else if (++s_refus == 20) ESP_LOGW(TAG, "20 refusals in a row: the radio bus is not freeing up for the screen");
        xSemaphoreGive(s_fb_mux);
        return;
    }
    /* VCOM keepalive ~1 Hz when nothing is being written, whatever the call
     * cadence (100 ms on the left half, MEMLCD_DROITE_PERIODE_MS on the
     * right half): timestamped, not counted. */
    static uint32_t s_vcom_ms;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(now_ms - s_vcom_ms) >= 1000u) { s_vcom_ms = now_ms; memlcd_panel_vcom_tick(); }
}

/* The LVGL tick is a periodic esp_timer (50 ms) WITHOUT skip_unhandled_events:
 * after a light sleep the esp_timer task (high priority) replays every missed
 * period BEFORE the sleep task's first instruction after wake — 13 500 events
 * after an 11-minute sleep, long enough for the key that woke the board to be
 * released before the wake capture reads the rows ("lines at exit=0x0", bench
 * 2026-09-21). Stopped for the sleep, restarted at wake: LVGL's own timers
 * (refresh) do not replay. */
static void memlcd_sleep(void) { s_sleeping = true; lvgl_port_stop(); }   /* frozen image, no more VCOM, no tick */
static void memlcd_wake(void)
{
    lvgl_port_resume();
    s_sleeping = false;
    s_dirty = true;                                                /* the image comes back right away */
    if (s_disp && lvgl_port_lock(50)) { lv_obj_invalidate(lv_scr_act()); lvgl_port_unlock(); }
    /* Nothing more here: this hook runs in the wake sequence, BEFORE the
     * key capture, and the SPI bus still belongs to the radio (lock held
     * until its own hook). The screen task pushes the image on its next tick. */
}
static void memlcd_noop(void) {}
static void memlcd_show_dfu(void)
{
    if (!s_disp || !lvgl_port_lock(100)) return;
    lv_obj_clean(lv_scr_act()); s_built = false;
    lv_obj_t *l = texte(lv_scr_act(), &lv_font_montserrat_14, 0, 70);
    lv_obj_set_width(l, MEMLCD_W); lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "DFU");
    lvgl_port_unlock();
}

const display_backend_t memlcd_display_backend = {
    .init = memlcd_init, .update_layer = memlcd_update, .update = memlcd_update,
    .refresh_all = memlcd_refresh_all, .sleep = memlcd_sleep, .wake = memlcd_wake,
    .notify_mouse = memlcd_noop, .notify_keypress = memlcd_noop,
    .notify_display_key = memlcd_noop, .show_dfu = memlcd_show_dfu,
};
