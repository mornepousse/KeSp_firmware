/* display_backend_t backend of the Sharp memory-LCD — LVGL UI in 68x160 portrait.
 *   LEFT  : the "cave" (memlcd_cave.h / memlcd_cave.c, plan
 *           2026-09-30-left-screen-cave): dark, rock edges, the corner logo,
 *           route and SEEN in words, the water-drop gauge, the chest's status,
 *           the layer name and its flags; the chest's prompt and code take
 *           the whole screen, its browser the lower part. Every layout
 *           decision is pure and host-tested; this file only calls the engine.
 *   RIGHT : the same cave since 2026-09-30 (memlcd_cave_vue_droite, the engine
 *           compiled with MEMLCD_CAVE_DROITE): rock edges, the left's status
 *           band (drop, route icon from its USB and the dongle's ACK, ⇆) and
 *           the large 56 px logo; asleep, the left's logo + zZ.
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
#endif
#include "memlcd_cave.h"       /* the cave: both halves */
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
#include "keyboard_config.h"   /* current_layout, default_layout_names */
#include "matrix_scan.h"       /* last_layer */
#include "key_features.h"      /* caps_word_is_active, osm_peek, osl_get_layer */
#include "usb_hid.h"           /* hid_led_state */
#endif
#if CONFIG_KASE_KBD_WIRELESS
#include "usb_presence.h"      /* kbd_active_route */
#endif
#if CONFIG_KASE_LINK_WIRE
#include "link_uart.h"         /* link_uart_active: TRRS 5 V closed */
#endif
#if CONFIG_KASE_KBD_WIRELESS
#include "kbd_relay_tx.h"      /* kbd_relay_dongle_vu */
#endif
#if CONFIG_KASE_HALF_LINK_TX
#include "half_link.h"         /* half_link_tx_dongle_vu */
#endif
#if !CONFIG_KASE_DEVICE_ROLE_KEYBOARD
#include "usb_presence.h"      /* usb_presence_cable: the right's route icon */
#endif
#if CONFIG_KASE_CHEST_LINK
#include "chest_link.h"        /* chest_link_view, CHEST_VIEW_* */
/* The chest_link.h bits and the memlcd model's MEMLCD_COFFRE_* (memlcd_model.h)
 * are two independent #defines on either side of the display_backend vtable
 * boundary: nothing but this assert stops them from drifting apart. */
_Static_assert(CHEST_VIEW_PRESENT == MEMLCD_COFFRE_PRESENT, "chest_link.h CHEST_VIEW_PRESENT vs memlcd_model.h MEMLCD_COFFRE_PRESENT");
_Static_assert(CHEST_VIEW_BADVER == MEMLCD_COFFRE_BADVER, "chest_link.h CHEST_VIEW_BADVER vs memlcd_model.h MEMLCD_COFFRE_BADVER");
#endif

static const char *TAG = "memlcd_be";
LV_FONT_DECLARE(lv_font_montserrat_14);   /* both: DFU */

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
/* The status task and LVGL run at 1 s on this panel (cadence.h). A
 * cadence.h read without sdkconfig.h would silently fall back to 100 ms. */
_Static_assert(STATUS_DISP_PERIODE_MS == 1000u, "memory-LCD halves: status display at 1 s");
_Static_assert(LVGL_REFR_MS == 1000u, "memory-LCD halves: LVGL refresh at 1 s");

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
    memlcd_cave_build(scr);
    s_built = true;
}

/* ── Model: what the sources say right now ───────────────────────── */
static void lire_modele(memlcd_model_t *m)
{
    memset(m, 0, sizeof *m);
    m->batt_local_dv = 0xFF;
    m->batt_pct = 0xFF;
#if CONFIG_KASE_LINK_WIRE
    m->lien_5v = link_uart_active() ? 1 : 0;
#endif
#if CONFIG_KASE_BATT_SENSE
    { uint8_t dv = batt_sense_dv(); m->batt_local_dv = dv ? dv : 0xFF; m->batt_local_chg = batt_sense_charging(); m->batt_niveau = batt_sense_niveau();
      m->batt_pct = batt_sense_pct(); }   /* the percentage is batt_sense's DISPLAYED one (mV curve, filtered, 5 % steps) */
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
    m->couche    = memlcd_couche_affichee(current_layout, last_layer);
    if (m->couche >= LAYERS) m->couche = 0;
    strncpy(m->nom, default_layout_names[m->couche], sizeof m->nom - 1);
    m->caps_word   = caps_word_is_active();
    m->osm         = osm_peek();
    { int8_t osl = osl_get_layer(); m->osl = osl < 0 ? MEMLCD_OSL_AUCUNE : (uint8_t)osl; }
    /* Caps Lock is the HOST's LED: known over USB only. In RF the dongle gets
     * the LED report and nothing brings it back, and the last USB value would
     * be stale — shown on the USB route only. */
    m->caps_lock   = !m->route_rf && (hid_led_state & HID_LED_CAPS_LOCK);
#if CONFIG_KASE_CHEST_LINK
    { chest_view_t v; chest_link_view(&v);
      /* Ages the SNAPSHOT on our own clock read before mapping it into the
       * model (review I2): the link task only rebuilds the view on a round
       * it actually gets to read the chest (chest_task's `continue` skips
       * chest_view_build entirely on a bus-busy/corrupt/absent round), so a
       * visible code could otherwise stay frozen on the panel long past its
       * real deadline whenever reads stall. chest_view_age is pure and
       * wrap-safe; esp_timer_get_time()/1000 is the SAME millisecond clock
       * chest_link.c's own round uses for `now` (chest_task, `now =
       * (uint32_t)(esp_timer_get_time() / 1000)`) — the two must agree for
       * the deadline comparison to mean anything. */
      chest_view_age(&v, (uint32_t)(esp_timer_get_time() / 1000));
      memlcd_model_set_coffre(m, &v); }
#endif
#else
    m->is_left   = 0;
    m->osl       = MEMLCD_OSL_AUCUNE;
    /* The route icon: the plug while its USB is up (its keys still go by
     * radio), else the waves, filled while the dongle acknowledges. */
    {   bool vu = false;
#if CONFIG_KASE_HALF_LINK_TX
        vu = half_link_tx_dongle_vu();
#endif
        memlcd_droite_route(m, usb_presence_cable(), vu); }
#endif
}

/* The cave draws everything (the left's memlcd_cave_vue, the right's
 * memlcd_cave_vue_droite — MEMLCD_CAVE_DROITE picks it at compile time), the
 * percentage being batt_sense's DISPLAYED one (batt_sense_pct: mV curve, EMA,
 * 5 % steps, never up on battery), the same on both halves. 0xFF reads "?". */
static void dessiner(const memlcd_model_t *m)
{
    memlcd_cave_draw(m, m->batt_pct);
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
        /* LVGL tick and task sleep at 1 s (cadence.h): nothing is animated
         * on this status panel, and every wake at rest shortens the calm
         * automatic light sleep needs. A model change shows within ~1-2 s. */
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
     * light sleep. LVGL_REFR_MS (1 s) is enough for a status screen. */
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
static void memlcd_before_sleep(void);
static bool memlcd_init(void)
{
    if (!lvgl_pret()) return false;
    try_attach();              /* succeeds if the radio is already initialized (right half) */
#if CONFIG_KASE_VEILLE
    /* Sleep (B7): frozen image and VCOM suspended while asleep; on wake,
     * flags only — the screen task pushes the image again on its tick. */
    static const veille_hook_t hook = { "screen", memlcd_sleep, memlcd_wake, memlcd_before_sleep };
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
    /* Rendered NOW (lv_refr_now → flush → panel), on this task: LVGL's own
     * refresh timer runs at 1 s with a 1 s tick (cadence.h) and could show a
     * change up to 2 s late — a layer tapped and released looked stuck
     * (Mae, 2026-09-26). The reactivity is now the caller's cadence: 100 ms
     * on USB, 1 s on battery (status_disp_periode_ms). */
    if (memlcd_model_diff(&s_shown, &m)) {
        if (lvgl_port_lock(50)) { s_shown = m; dessiner(&m); lv_refr_now(s_disp); lvgl_port_unlock(); }
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
/* Last image before sleep, drawn NOW (lv_refr_now → flush → panel) while the
 * SPI bus is still free: the radio's dormir takes the bus lock for the whole
 * sleep. The model is re-read, so a ⇆ or route that changed since the last
 * 1 s tick is up to date, and zZ says the image is frozen. On wake the
 * model re-read has veille = 0: the diff redraws without zZ. Coming from
 * light sleep into deep sleep the screen is already asleep: nothing to do. */
static void memlcd_before_sleep(void)
{
    if (!s_disp || s_sleeping || !s_built || !s_attached) return;
    if (!lvgl_port_lock(100)) return;
    memlcd_model_t m; lire_modele(&m);
    m.batt_local_dv = s_shown.batt_local_dv;   /* keep the stabilized voltage */
    m.veille = 1;
    s_shown = m;
    dessiner(&m);
    lv_refr_now(s_disp);
    lvgl_port_unlock();
}
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
