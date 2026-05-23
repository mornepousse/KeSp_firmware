/*
 * eink_lvgl.c — Raw LVGL v8 monochrome integration for SSD1681 e-ink panel.
 *
 * Architecture (set_px_cb + rounder_cb recipe):
 *   - disp_drv.set_px_cb: packs each LVGL-rendered pixel into s_fb via
 *       eink_fb_set_px(s_fb, x, y, color.full). No bulk packing in flush_cb.
 *   - disp_drv.rounder_cb: byte-aligns the dirty area (x1 rounds down to
 *       multiple of 8, x2 rounds up to multiple_of_8 + 7).
 *   - disp_drv.flush_cb: calls eink_push(s_fb) on last fragment; always
 *       calls lv_disp_flush_ready(drv) — LVGL deadlocks without this.
 *   - disp_drv.full_refresh = 0: LVGL renders the invalidated screen in
 *       stripes; set_px_cb packs every pixel into s_fb across all stripes;
 *       is_last fires after the final stripe → eink_push() gets the complete frame.
 *   - Draw buffer: EINK_WIDTH × 10 = 2000 lv_color_t elements (2000 bytes
 *       at LV_COLOR_DEPTH=1). Scratch space only — never interpreted by our code.
 *   - Tick: esp_timer 5 ms → lv_tick_inc(5).
 *   - Handler: eink_lvgl_task (lv_timer_handler loop, prio 3, core 0).
 *
 * Dashboard layout (200×200, 1bpp monochrome):
 *   y=  8   LAYER NAME  (Montserrat 28, centered, bold visual anchor)
 *   y= 50   separator line
 *   y= 62   L * [||||]  (Montserrat 14, left-padded)
 *   y= 80   R * [||||]
 *   y= 98   USB *
 *   y=116   BAT --%     (placeholder, ADC not yet implemented)
 *   y=134   separator line
 *   y=148   version     (Montserrat 14, centered, tag-only: "v3.7.12")
 *
 * Why set_px_cb, not direct draw buffer packing:
 *   At LV_COLOR_DEPTH=1, sizeof(lv_color_t)==1 (1 byte per pixel in the draw
 *   buffer, not 1 bit). A full-frame draw buffer would need 40000 bytes (40 KB).
 *   A 5000-element draw buffer covers only 5000 of the 40000 pixels — undersized.
 *   set_px_cb bypasses the draw buffer layout entirely: LVGL calls it per pixel,
 *   and we pack directly into the 5000-byte s_fb without any intermediate buffer.
 *
 * No esp_lvgl_port used (no esp_lcd panel handle for SSD1681 in this project).
 */

#include "eink_lvgl.h"
#include "eink.h"           /* eink_push, eink_fb_set_px, EINK_WIDTH/HEIGHT/FB_SIZE */
#include "espnow_info.h"    /* g_half_state, g_half_state_mutex, half_state_t */
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"   /* esp_app_get_description() */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "eink_lvgl";

/* ── Packed framebuffer — the only pixel storage (5000 bytes, BSS) ─
 * Layout: row-major, MSB-first, 25 bytes per row.
 * s_fb[row * 25 + col/8]  bit (7 - col%8) = 1→white, 0→black.
 * Written pixel-by-pixel via set_px_cb → eink_fb_set_px().
 * Pushed to SSD1681 RAM by eink_push() in flush_cb on last fragment.
 * Initialized to 0xFF (all white) in eink_lvgl_init(). */
static uint8_t s_fb[EINK_FB_SIZE];

/* ── LVGL draw buffer — scratch space only (2000 bytes, BSS) ────────
 * At LV_COLOR_DEPTH=1, sizeof(lv_color_t)==1 (1 byte per pixel).
 * 2000 elements = 2000 bytes = scratch for 10 rows of 200 pixels.
 * With set_px_cb registered, LVGL does NOT use this buffer to output
 * final pixel data — it calls set_px_cb per pixel instead.
 * The buffer is required by lv_disp_draw_buf_init but never read by us.
 *
 * LVGL v8 requires buf_size_px >= 2 × hor_res (minimum). 2000 >> 400.  */
#define EINK_LVGL_BUF_ROWS   10
static lv_color_t         s_lvgl_buf[EINK_WIDTH * EINK_LVGL_BUF_ROWS];   /* 2000 bytes */
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;

/* ── Dynamic layer-name label ────────────────────────────────────
 * Initialized to "KaSe" in eink_lvgl_init(). Uses Montserrat 28 (bold anchor).
 * Updated to g_half_state.layer_name when bit 0 notify fires.
 * Falls back to "KaSe" if layer_name[0] == '\0' (unpaired / no packet yet). */
static lv_obj_t *s_label_layer = NULL;

/* ── Status dashboard labels ────────────────────────────────────
 * All use Montserrat 14 (default font). Initialized in eink_lvgl_init().
 * Updated on bit-1 notify or timeout. Separators are static lv_line objects. */
static lv_obj_t *s_label_link_l   = NULL;   /* "L * [||||]" */
static lv_obj_t *s_label_link_r   = NULL;   /* "R * [||||]" */
static lv_obj_t *s_label_usb      = NULL;   /* "USB *"       */
static lv_obj_t *s_label_bat      = NULL;   /* "BAT --%"  (placeholder) */

/* Tracks whether the task is currently showing the "dongle lost" degraded view.
 * Guards against re-invalidating every 50 ms loop iteration when the dongle is
 * persistently absent (which would cause continuous 1.5 s e-ink refreshes). */
static bool s_showing_degraded = false;

/* ── Tick timer ─────────────────────────────────────────────────────
 * Fires every 5 ms → lv_tick_inc(5). Does NOT trigger redraws. */
static esp_timer_handle_t s_tick_timer = NULL;

/* ── Task handle — exposed for xTaskNotify from espnow_info.c ───
 * Set in eink_lvgl_start() when the task is created.
 * NULL until eink_lvgl_start() is called (checked before every notify). */
static TaskHandle_t s_eink_task_handle = NULL;

TaskHandle_t eink_get_task_handle(void)
{
    return s_eink_task_handle;
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(5);
}

/* ── set_px_cb — called by LVGL for every rendered pixel ────────────
 * Signature (LVGL v8 monochrome convention):
 *   void set_px_cb(lv_disp_drv_t *drv, uint8_t *buf, lv_coord_t buf_w,
 *                  lv_coord_t x, lv_coord_t y, lv_color_t color, lv_opa_t opa)
 *
 * We ignore buf and buf_w — s_fb is the authoritative framebuffer.
 * Polarity: color.full == 1 = white (lv_color_white), color.full == 0 = black.
 * SSD1681 native: bit=1→white, bit=0→black. Mapping is direct, no inversion. */
static void eink_lvgl_set_px_cb(lv_disp_drv_t *drv,
                                 uint8_t *buf,
                                 lv_coord_t buf_w,
                                 lv_coord_t x,
                                 lv_coord_t y,
                                 lv_color_t color,
                                 lv_opa_t opa)
{
    (void)drv;
    (void)opa;   /* 1bpp: any coverage is opaque */
    /* Store the rendered pixel into LVGL's own draw buffer at the RELATIVE
     * position. The mapping into the full-screen s_fb happens in flush_cb,
     * which receives the area's ABSOLUTE coordinates (rounder_cb only reports
     * the invalidated region bounds, not each stripe's origin). */
    ((lv_color_t *)buf)[(int)y * (int)buf_w + (int)x] = color;
}

/* ── rounder_cb — align dirty area to byte boundaries ───────────────
 * SSD1681 RAM is byte-granular. eink_fb_set_px writes individual bits
 * within bytes. LVGL's dirty area must be byte-aligned so that no partial
 * byte is left stale when LVGL renders a stripe and calls flush_cb.
 *
 * With full_refresh=1, LVGL always sends [0,0]–[199,199], which is already
 * aligned. The rounder_cb is still registered as defensive measure for
 * future partial-refresh mode (Plan Bricks-4+). */
static void eink_lvgl_rounder_cb(lv_disp_drv_t *drv, lv_area_t *area)
{
    (void)drv;
    /* Round x1 DOWN to the nearest multiple of 8 */
    area->x1 = (area->x1 / 8) * 8;
    /* Round x2 UP to the last pixel of the byte containing x2 */
    area->x2 = (area->x2 / 8) * 8 + 7;
    if (area->x2 >= EINK_WIDTH) area->x2 = EINK_WIDTH - 1;
}

/* ── flush_cb — called by lv_timer_handler after each rendered stripe ─
 * set_px_cb writes pixels into LVGL's own draw buffer at coords RELATIVE to the
 * stripe. Here we map that buffer (color_p) into the full-screen s_fb using the
 * stripe's ABSOLUTE coordinates from `area` (the only reliable source of the
 * stripe origin — rounder_cb only reports the invalidated region's bounds).
 * eink_push is triggered on the last fragment of the refresh.
 *
 * CRITICAL: lv_disp_flush_ready(drv) MUST be called unconditionally — or LVGL
 * deadlocks waiting for the ready signal before issuing the next render. */
static void eink_lvgl_flush_cb(lv_disp_drv_t *drv,
                                const lv_area_t *area,
                                lv_color_t *color_p)
{
    /* Map this stripe's pixels into the full-screen s_fb using the area's
     * ABSOLUTE coordinates. color_p is LVGL's draw buffer (1 byte/px at
     * LV_COLOR_DEPTH=1), row-major with stride = area width. */
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    for (int ry = 0; ry < h; ry++) {
        for (int rx = 0; rx < w; rx++) {
            int v = color_p[ry * w + rx].full;   /* 1 = white, 0 = black */
            /* This SSD1681 panel maps RAM-X=0 to the PHYSICAL RIGHT edge, so the
             * framebuffer is displayed horizontally mirrored. Reverse the X axis
             * here (col → WIDTH-1-col) so logical screen coords land correctly.
             * (Verified on hardware: a raw left bar appeared on the right edge.) */
            eink_fb_set_px(s_fb, (EINK_WIDTH - 1) - (area->x1 + rx),
                           area->y1 + ry, v);
        }
    }

    if (lv_disp_flush_is_last(drv)) {
        /* Whole frame assembled in s_fb — push to the SSD1681 panel. */
        eink_push(s_fb);
    }

    /* Acknowledge to LVGL — MUST be unconditional */
    lv_disp_flush_ready(drv);
}

/*
 * build_link_label() — format a link-line string into buf (min 16 bytes).
 * side: 'L' or 'R'. dongle_alive: false = show '?' override.
 * ASCII-only glyphs: '*' up, '-' down, '?' unknown, '|' bar, '.' empty.
 * Output example: "L * [|||.]" (13 chars + NUL, well within 16 bytes).
 */
static void build_link_label(char *buf, char side,
                              bool dongle_alive, bool link_up, uint8_t bars)
{
    const char dot = dongle_alive ? (link_up ? '*' : '-') : '?';
    bars = (bars > 4) ? 4 : bars;   /* clamp */
    char bstr[7];   /* "[||||]" + NUL */
    bstr[0] = '[';
    for (int i = 0; i < 4; i++) bstr[1 + i] = (i < (int)bars) ? '|' : '.';
    bstr[5] = ']';
    bstr[6] = '\0';
    snprintf(buf, 16, "%c %c %s", side, dot, bstr);
}

/*
 * build_usb_label() — format the USB status line into buf (min 10 bytes).
 */
static void build_usb_label(char *buf, bool dongle_alive, bool usb_active)
{
    const char dot = dongle_alive ? (usb_active ? '*' : '-') : '?';
    snprintf(buf, 10, "USB %c", dot);
}

/* ── LVGL handler task ──────────────────────────────────────────────
 * lv_timer_handler() drives rendering + flush. Returns ms until next
 * LVGL timer fires (capped at 50 ms to remain responsive).
 * Priority 3 — lower than rf_rx_task and half_scan_task; e-ink latency
 * is irrelevant compared to key event latency.
 *
 * Trap 4: stack starts at 4096. Log HWM after first render; bump to 6144
 * in eink_lvgl_start() if < 512 bytes free (128 FreeRTOS words). */
static void eink_lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "eink_lvgl_task started");

    for (;;) {
        /* Run LVGL timers (drives pending invalidations from previous notify). */
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms == 0 || sleep_ms > 50) sleep_ms = 50;

        /* Wait for LVGL timer OR notify (bit 0 = layer, bit 1 = status). */
        uint32_t notify_val = 0;
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &notify_val,
                                              pdMS_TO_TICKS(sleep_ms));

        /* ── Bit 0: layer/state changed ──────────────────────────── */
        if (notified == pdTRUE && (notify_val & 0x01)) {
            char name_copy[17] = {0};
            if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                memcpy(name_copy, g_half_state.layer_name, 16);
                xSemaphoreGive(g_half_state_mutex);
            }
            name_copy[16] = '\0';
            if (name_copy[0] == '\0') memcpy(name_copy, "KaSe", 5);

            if (s_label_layer != NULL && lv_obj_is_valid(s_label_layer)) {
                lv_label_set_text(s_label_layer, name_copy);
                lv_obj_invalidate(s_label_layer);
                ESP_LOGI(TAG, "eink: layer -> '%s'", name_copy);
            }
        }

        /* ── Dongle-link timeout check (runs every loop, ~50 ms) ──
         * Compute dongle_alive independently of whether a notify fired.
         * This catches persistent absence without requiring a notify. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        bool dongle_alive = false;
        bool link_left  = false;
        bool link_right = false;
        bool usb_active = false;
        uint8_t sig_left  = 0;
        uint8_t sig_right = 0;

        if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            dongle_alive = (now_ms - g_half_state.last_status_ms) < 15000u;
            link_left    = g_half_state.link_left;
            link_right   = g_half_state.link_right;
            usb_active   = g_half_state.usb_active;
            sig_left     = g_half_state.sig_left;
            sig_right    = g_half_state.sig_right;
            xSemaphoreGive(g_half_state_mutex);
        }

        /* ── Bit 1: status changed (on_status notified) ──────────── */
        bool update_status = (notified == pdTRUE && (notify_val & 0x02));

        /* ── Degraded state transition: dongle lost / recovered ───── */
        bool newly_degraded  = (!dongle_alive && !s_showing_degraded);
        bool newly_recovered = ( dongle_alive &&  s_showing_degraded);
        if (newly_degraded) {
            s_showing_degraded = true;
            update_status = true;   /* force label update to '?' */
        }
        if (newly_recovered) {
            s_showing_degraded = false;
            update_status = true;   /* force label update from '?' to real values */
        }

        /* ── Update status labels if needed ───────────────────────── */
        if (update_status) {
            char lbuf[16], rbuf[16], ubuf[10];
            build_link_label(lbuf, 'L', dongle_alive, link_left,  sig_left);
            build_link_label(rbuf, 'R', dongle_alive, link_right, sig_right);
            build_usb_label(ubuf, dongle_alive, usb_active);

            if (s_label_link_l != NULL && lv_obj_is_valid(s_label_link_l)) {
                lv_label_set_text(s_label_link_l, lbuf);
                lv_obj_invalidate(s_label_link_l);
            }
            if (s_label_link_r != NULL && lv_obj_is_valid(s_label_link_r)) {
                lv_label_set_text(s_label_link_r, rbuf);
                lv_obj_invalidate(s_label_link_r);
            }
            if (s_label_usb != NULL && lv_obj_is_valid(s_label_usb)) {
                lv_label_set_text(s_label_usb, ubuf);
                lv_obj_invalidate(s_label_usb);
            }
            /* s_label_bat is static "--" until ADC is implemented; no update needed */
            ESP_LOGI(TAG, "eink: status -> %s  %s  %s (alive=%d)",
                     lbuf, rbuf, ubuf, dongle_alive);
        }

        /* Stack headroom check — log once */
        static bool s_stack_checked = false;
        if (!s_stack_checked) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "eink_lvgl_task stack HWM: %u words free", (unsigned)hwm);
            if (hwm < 128) {
                ESP_LOGW(TAG, "STACK LOW -- bump eink_lvgl_task stack to 6144");
            }
            s_stack_checked = true;
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────*/

void eink_lvgl_init(void)
{
    /* Initialize LVGL core */
    lv_init();

    /* Pre-fill s_fb with all-white (0xFF) before first render.
     * set_px_cb will overwrite individual bits as LVGL renders.
     * Without this, pixels LVGL does not explicitly paint are undefined. */
    memset(s_fb, 0xFF, EINK_FB_SIZE);

    /* Draw buffer: 2000-element stripe, single buffer (no double-buffer for e-ink).
     * Size = EINK_WIDTH × EINK_LVGL_BUF_ROWS = 200 × 10 = 2000 lv_color_t elements
     * = 2000 bytes at LV_COLOR_DEPTH=1. This is scratch space only;
     * set_px_cb bypasses it and writes directly to s_fb. */
    lv_disp_draw_buf_init(&s_draw_buf, s_lvgl_buf, NULL,
                          EINK_WIDTH * EINK_LVGL_BUF_ROWS);

    /* Display driver */
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.flush_cb     = eink_lvgl_flush_cb;
    s_disp_drv.set_px_cb    = eink_lvgl_set_px_cb;   /* per-pixel packing into s_fb */
    s_disp_drv.rounder_cb   = eink_lvgl_rounder_cb;  /* byte-align dirty area */
    s_disp_drv.hor_res      = EINK_WIDTH;    /* 200 */
    s_disp_drv.ver_res      = EINK_HEIGHT;   /* 200 */
    /* full_refresh MUST be 0 here: full_refresh=1 requires a FULL-SCREEN draw
     * buffer (hor_res×ver_res = 40000 px). Our stripe buffer is only 2000 px,
     * so full_refresh=1 breaks the render path and set_px_cb never paints → blank.
     * With full_refresh=0 + set_px_cb, LVGL renders the invalidated screen in
     * ~20 stripes; set_px_cb packs every pixel into s_fb across all stripes;
     * is_last fires after the final stripe → eink_push() gets the complete frame. */
    s_disp_drv.full_refresh = 0;
    lv_disp_drv_register(&s_disp_drv);

    ESP_LOGI(TAG, "LVGL init OK, display registered (%dx%d, 1bpp, set_px_cb)",
             EINK_WIDTH, EINK_HEIGHT);

    /* Tick timer: 5 ms period → lv_tick_inc(5) */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, 5 * 1000));   /* µs */

    /* ── Full-screen status dashboard ──────────────────────────────
     * Layout (200×200 px, all coords absolute, top-left origin):
     *
     *   y=  8  LAYER NAME  — Montserrat 28, centered (visual anchor)
     *   y= 48  separator   — "--------------------" Montserrat 14, centered
     *   y= 62  L link line — "L * [||||]"  Montserrat 14, left-padded x=16
     *   y= 80  R link line — "R * [||||]"  Montserrat 14, left-padded x=16
     *   y= 98  USB line    — "USB *"        Montserrat 14, left-padded x=16
     *   y=116  BAT line    — "BAT --%"      Montserrat 14, left-padded x=16
     *   y=134  separator   — "--------------------" Montserrat 14, centered
     *   y=150  version     — "vX.Y.Z" (tag only), Montserrat 14, centered
     *
     * Fonts: Montserrat 28 (layer) + Montserrat 14 (everything else).
     * Both are enabled in sdkconfig (CONFIG_LV_FONT_MONTSERRAT_28=y,
     * CONFIG_LV_FONT_MONTSERRAT_14=y).
     *
     * Created here so LVGL renders on the first lv_timer_handler() call.
     * Static labels invalidate once at creation; dynamic labels (layer,
     * link_l, link_r, usb) are invalidated explicitly by eink_lvgl_task. */
    lv_obj_t *scr = lv_scr_act();

    /* White background → s_fb byte = 0xFF (all bits set) via set_px_cb.
     * lv_color_white() has .full == 1 at LV_COLOR_DEPTH=1 → bit set = white. */
    lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* ── Layer name (Montserrat 28, centered) ───────────────────────
     * Prominent visual anchor at y=8. Initial "KaSe" shown until first
     * EN_INFO_LAYER packet. Updated by eink_lvgl_task on bit-0 notify. */
    s_label_layer = lv_label_create(scr);
    lv_label_set_text(s_label_layer, "KaSe");
    lv_obj_set_style_text_color(s_label_layer, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_label_layer, &lv_font_montserrat_28, LV_PART_MAIN);
    /* Stretch to full width so lv_obj_align centers the text within the screen */
    lv_obj_set_width(s_label_layer, EINK_WIDTH);
    lv_obj_set_style_text_align(s_label_layer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(s_label_layer, 0, 8);

    /* ── Top separator (static, Montserrat 14) ──────────────────────
     * 20 dashes spans ~160 px at 8 px/char — leaves comfortable margins. */
    lv_obj_t *sep_top = lv_label_create(scr);
    lv_label_set_text(sep_top, "--------------------");
    lv_obj_set_style_text_color(sep_top, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_width(sep_top, EINK_WIDTH);
    lv_obj_set_style_text_align(sep_top, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(sep_top, 0, 48);

    /* ── L link + signal (Montserrat 14, left-padded) ────────────── */
    s_label_link_l = lv_label_create(scr);
    lv_label_set_text(s_label_link_l, "L - [....]");
    lv_obj_set_style_text_color(s_label_link_l, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(s_label_link_l, 16, 62);

    /* ── R link + signal (Montserrat 14, left-padded) ────────────── */
    s_label_link_r = lv_label_create(scr);
    lv_label_set_text(s_label_link_r, "R - [....]");
    lv_obj_set_style_text_color(s_label_link_r, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(s_label_link_r, 16, 80);

    /* ── USB status (Montserrat 14, left-padded) ─────────────────── */
    s_label_usb = lv_label_create(scr);
    lv_label_set_text(s_label_usb, "USB -");
    lv_obj_set_style_text_color(s_label_usb, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(s_label_usb, 16, 98);

    /* ── Battery (Montserrat 14, left-padded) — PLACEHOLDER ─────────
     * ADC not yet implemented. Shows "BAT --%" until batt_pct is wired up.
     * When ADC is ready: lv_label_set_text_fmt(s_label_bat, "BAT %3u%%", pct). */
    s_label_bat = lv_label_create(scr);
    lv_label_set_text(s_label_bat, "BAT --%");
    lv_obj_set_style_text_color(s_label_bat, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(s_label_bat, 16, 116);

    /* ── Bottom separator (static, Montserrat 14) ───────────────────*/
    lv_obj_t *sep_bot = lv_label_create(scr);
    lv_label_set_text(sep_bot, "--------------------");
    lv_obj_set_style_text_color(sep_bot, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_width(sep_bot, EINK_WIDTH);
    lv_obj_set_style_text_align(sep_bot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(sep_bot, 0, 134);

    /* ── Firmware version (Montserrat 14, centered, tag-only) ───────
     * Full git-describe string ("v3.7.12-87-gdc47652-dirty") overflows the
     * 200 px screen at 14 px font (~8 px/char × 26 chars ≈ 208 px).
     * Truncate to the version tag only: stop at the first '-' after "v".
     *
     * esp_app_desc_t.version is app_version from CMakeLists / git describe.
     * It is NUL-terminated and at most 32 chars (IDF_VER_MAX_LEN). */
    const esp_app_desc_t *desc = esp_app_get_description();
    char ver_short[32];
    strncpy(ver_short, desc->version, sizeof(ver_short) - 1);
    ver_short[sizeof(ver_short) - 1] = '\0';
    /* Find the second hyphen (first '-' after the 'v' prefix + digits).
     * "v3.7.12-87-gdc47652-dirty" → stop after "v3.7.12". */
    char *p = ver_short;
    if (*p == 'v') p++;                  /* skip leading 'v' — stays in place */
    while (*p && *p != '-') p++;         /* skip digits/dots to first '-' */
    *p = '\0';                           /* truncate: "v3.7.12-87-g..." → "v3.7.12" */

    lv_obj_t *label_ver = lv_label_create(scr);
    lv_label_set_text(label_ver, ver_short);
    lv_obj_set_style_text_color(label_ver, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_width(label_ver, EINK_WIDTH);
    lv_obj_set_style_text_align(label_ver, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(label_ver, 0, 150);

    ESP_LOGI(TAG, "eink dashboard created: layer=Montserrat28 status=Montserrat14");
    ESP_LOGI(TAG, "firmware version display: '%s' (full: '%s')", ver_short, desc->version);
}

void eink_lvgl_start(void)
{
    /* Trap 4: start at 4096, check HWM after first render, bump to 6144 if needed */
    BaseType_t ret = xTaskCreatePinnedToCore(
        eink_lvgl_task, "eink_lvgl",
        4096,   /* bytes — monitor HWM and bump to 6144 if < 512 bytes free */
        NULL,
        3,      /* priority: lower than rf_rx_task and half_scan_task */
        &s_eink_task_handle,   /* save handle — used by eink_get_task_handle() */
        0);     /* core 0 */
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed for eink_lvgl_task");
        s_eink_task_handle = NULL;
    }
}
