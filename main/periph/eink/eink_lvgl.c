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
 * Dashboard layout (200×200, 1bpp) — fastfetch style, monospace unscii_8:
 *   left ~44 px : keyboard icon (drawn with LVGL primitives, no asset)
 *   right x=48  : kase@dongle / sep / lay / L / R / usb / set / net / mem /
 *                 flash / soc / fw  (key:value, 12 px pitch)
 *   Dynamic lines (lay/L/R/usb/mem) updated by eink_lvgl_task; the rest are
 *   static local stats gathered once at init. Battery = "--%" until ADC lands.
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
#include "esp_system.h"     /* esp_get_free_heap_size */
#include "esp_app_desc.h"   /* esp_app_get_description() */
#include "esp_chip_info.h"  /* esp_chip_info — SoC model + cores */
#include "esp_flash.h"      /* esp_flash_get_size — chip flash size */
#include "esp_partition.h"  /* esp_partition iteration — flash footprint */
#include "driver/temperature_sensor.h"  /* CPU temp */
#include "rf_pairing.h"     /* rf_pairing_load_set_id_half, rf_derive_wifi_ch */
#include "board.h"          /* BOARD_NRF_ADDR_SUFFIX (this half's slot fallback) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

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
static lv_obj_t *s_label_link_l   = NULL;   /* "L   : 248/255 --%" */
static lv_obj_t *s_label_link_r   = NULL;   /* "R   : 250/255 --%" */
static lv_obj_t *s_label_usb      = NULL;   /* "usb : on"          */
static lv_obj_t *s_label_mem      = NULL;   /* "mem : 207K 42C" (heap + CPU temp) */

/* CPU temperature sensor (installed in eink_lvgl_init, read in the task). */
static temperature_sensor_handle_t s_tsens = NULL;

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

/* Pairing-splash data — set by eink_lvgl_show_paired(), consumed on bit 0x04.
 * volatile: written from half_pairing_task, read from eink_lvgl_task. */
static volatile uint16_t s_paired_set_id = 0;
static volatile uint8_t  s_paired_slot   = 0;

TaskHandle_t eink_get_task_handle(void)
{
    return s_eink_task_handle;
}

void eink_lvgl_show_paired(uint16_t set_id, uint8_t slot)
{
    s_paired_set_id = set_id;
    s_paired_slot   = slot;
    if (s_eink_task_handle != NULL) {
        xTaskNotify(s_eink_task_handle, 0x04, eSetBits);   /* bit 2 = pairing splash */
    }
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

/* ── Fastfetch info-line formatters (unscii_8 monospace, ~19 cols) ──────────
 * Battery is a placeholder "--%" until the ADC feature lands (L+R slots reserved). */

/* build_link_label() — "L   : 248/255 --%" (up) / "L   : --/255 --%" (dongle absent).
 * side: 'L'/'R'. q255: raw 0..255 link quality (0 = link down). buf >= 20 bytes. */
static void build_link_label(char *buf, char side, bool dongle_alive, uint8_t q255)
{
    if (!dongle_alive) snprintf(buf, 20, "%c   : --/255 --%%", side);
    else               snprintf(buf, 20, "%c   : %u/255 --%%", side, (unsigned)q255);
}

/* build_usb_label() — "usb : on" / "off" / "?". buf >= 12 bytes. */
static void build_usb_label(char *buf, bool dongle_alive, bool usb_active)
{
    const char *s = dongle_alive ? (usb_active ? "on" : "off") : "?";
    snprintf(buf, 12, "usb : %s", s);
}

/* build_mem_label() — "mem : 207K 42C" (heap free + CPU temp).
 * Quantized: heap to nearest 8 KB, temp to integer °C → text rarely changes →
 * e-ink rarely repaints. temp_c may be INT16_MIN if the sensor read failed. */
static void build_mem_label(char *buf, uint32_t heap_free, int temp_c)
{
    uint32_t k8 = ((heap_free / 1024u) / 8u) * 8u;   /* nearest 8 KB (floor) */
    if (temp_c > -100 && temp_c < 200)
        snprintf(buf, 20, "mem : %luK %dC", (unsigned long)k8, temp_c);
    else
        snprintf(buf, 20, "mem : %luK --C", (unsigned long)k8);
}

/* ── Penguin mascot — cycles one pose per refresh ───────────────────────────
 * 4 poses, ALL the same 5-line bounding box: the body (.--., / || \, ^  ^) is
 * fixed, only the eyes (blink/wide) and flippers move, so the penguin animates
 * IN PLACE without shifting. Pure ASCII (unscii_8 has full ASCII coverage). */
static const char *const s_penguins[] = {
    " .--.\n(o..o)\n(>  <)\n/ || \\\n ^  ^",   /* idle           */
    " .--.\n(-..-)\n(>  <)\n/ || \\\n ^  ^",   /* blink          */
    " .--.\n(o..o)\n( >< )\n/ || \\\n ^  ^",   /* flippers in    */
    " .--.\n(O..O)\n(<  >)\n/ || \\\n ^  ^",   /* wide, flap out */
};
#define PENGUIN_COUNT (sizeof(s_penguins) / sizeof(s_penguins[0]))
static lv_obj_t *s_label_penguin = NULL;
static uint8_t   s_penguin_idx   = 0;

/* make_line() — create a left-aligned unscii_8 (monospace) black label at (x,y). */
static lv_obj_t *make_line(lv_obj_t *scr, int x, int y, const char *text)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, LV_PART_MAIN);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* ── render_paired_splash() — one-shot "PAIRED" confirmation screen ──
 * Builds a fresh full-screen screen and loads it. The dashboard screen stays in
 * memory but hidden; we never restore it (the half reboots right after pairing),
 * so leaving it is harmless and keeps s_label_* pointers valid.
 * Rendered by the next lv_timer_handler() pass (≈1.5 s on e-ink).
 * Layout (200×200, all centered): "PAIRED" (M28) / OK glyph (M28) /
 * "set 0xXXXX" (M14 default) / "side L|R" (M14 default). */
static void render_paired_splash(uint16_t set_id, uint8_t slot)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *l_title = lv_label_create(scr);
    lv_label_set_text(l_title, "PAIRED");
    lv_obj_set_style_text_color(l_title, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l_title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_width(l_title, EINK_WIDTH);
    lv_obj_set_style_text_align(l_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(l_title, 0, 30);

    lv_obj_t *l_ok = lv_label_create(scr);
    lv_label_set_text(l_ok, LV_SYMBOL_OK);   /* bundled in built-in Montserrat fonts */
    lv_obj_set_style_text_color(l_ok, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l_ok, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_width(l_ok, EINK_WIDTH);
    lv_obj_set_style_text_align(l_ok, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(l_ok, 0, 78);

    lv_obj_t *l_set = lv_label_create(scr);
    lv_label_set_text_fmt(l_set, "set 0x%04X", set_id);
    lv_obj_set_style_text_color(l_set, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_width(l_set, EINK_WIDTH);
    lv_obj_set_style_text_align(l_set, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(l_set, 0, 128);

    lv_obj_t *l_side = lv_label_create(scr);
    lv_label_set_text(l_side, (slot == 0x01) ? "side L" : "side R");
    lv_obj_set_style_text_color(l_side, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_width(l_side, EINK_WIDTH);
    lv_obj_set_style_text_align(l_side, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(l_side, 0, 152);

    lv_scr_load(scr);        /* makes scr the active screen → rendered next pass */
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
                char laybuf[24];
                snprintf(laybuf, sizeof(laybuf), "lay : %s", name_copy);
                lv_label_set_text(s_label_layer, laybuf);
                lv_obj_invalidate(s_label_layer);
                ESP_LOGI(TAG, "eink: layer -> '%s'", name_copy);
            }
        }

        /* ── Bit 2: pairing splash (one-shot, stays until reboot) ──── */
        if (notified == pdTRUE && (notify_val & 0x04)) {
            ESP_LOGI(TAG, "eink: PAIRED splash set=0x%04X slot=0x%02X",
                     s_paired_set_id, s_paired_slot);
            render_paired_splash(s_paired_set_id, s_paired_slot);
            continue;   /* skip dashboard updates; next lv_timer_handler renders splash */
        }

        /* ── Dongle-link timeout check (runs every loop, ~50 ms) ──
         * Compute dongle_alive independently of whether a notify fired.
         * This catches persistent absence without requiring a notify. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        bool dongle_alive = false;
        bool usb_active = false;
        uint8_t sig_left  = 0;   /* raw 0..255 link quality (rf_signal_q255) */
        uint8_t sig_right = 0;

        if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            dongle_alive = (now_ms - g_half_state.last_status_ms) < 15000u;
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
            char lbuf[20], rbuf[20], ubuf[12], mbuf[20];
            build_link_label(lbuf, 'L', dongle_alive, sig_left);
            build_link_label(rbuf, 'R', dongle_alive, sig_right);
            build_usb_label(ubuf, dongle_alive, usb_active);

            /* mem line: heap free + CPU temp (quantized in build_mem_label). */
            int temp_c = INT16_MIN;
            if (s_tsens != NULL) {
                float t = 0.0f;
                if (temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK) temp_c = (int)t;
            }
            build_mem_label(mbuf, (uint32_t)esp_get_free_heap_size(), temp_c);

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
            if (s_label_mem != NULL && lv_obj_is_valid(s_label_mem)) {
                lv_label_set_text(s_label_mem, mbuf);
                lv_obj_invalidate(s_label_mem);
            }
            /* Cycle the penguin one pose per refresh (animates in place). */
            if (s_label_penguin != NULL && lv_obj_is_valid(s_label_penguin)) {
                s_penguin_idx = (uint8_t)((s_penguin_idx + 1) % PENGUIN_COUNT);
                lv_label_set_text(s_label_penguin, s_penguins[s_penguin_idx]);
                lv_obj_invalidate(s_label_penguin);
            }
            ESP_LOGI(TAG, "eink: status -> %s  %s  %s  %s peng=%u (alive=%d)",
                     lbuf, rbuf, ubuf, mbuf, (unsigned)s_penguin_idx, dongle_alive);
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

    /* ── CPU temperature sensor (best-effort) ─────────────────────── */
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    } else {
        s_tsens = NULL;
    }

    /* ── Fastfetch-style dashboard (200×200, monospace unscii_8) ────
     * Left ~44 px: keyboard icon (drawn). Right column x=48: info lines.
     * Static lines (set/net/flash/soc/fw + header/sep) drawn once here;
     * dynamic lines (lay/L/R/usb/mem) updated by eink_lvgl_task. */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* ── Gather local stats (all static after boot) ──────────────── */
    uint8_t  slot   = BOARD_NRF_ADDR_SUFFIX;
    uint16_t set_id = rf_pairing_load_set_id_half(BOARD_NRF_ADDR_SUFFIX, &slot);
    uint8_t  wifi_ch  = (set_id == 0) ? 6 : rf_derive_wifi_ch(set_id);
    uint8_t  nrf_base = (uint8_t)(80 + 2 * (set_id % 20));   /* L=base, R=base+1 */

    esp_chip_info_t chip;  esp_chip_info(&chip);

    uint32_t flash_size = 0;  esp_flash_get_size(NULL, &flash_size);
    uint32_t flash_used = 0;
    esp_partition_iterator_t pit = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                      ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (pit) {
        const esp_partition_t *p = esp_partition_get(pit);
        uint32_t end = p->address + p->size;
        if (end > flash_used) flash_used = end;
        pit = esp_partition_next(pit);
    }
    esp_partition_iterator_release(pit);

    const esp_app_desc_t *desc = esp_app_get_description();
    char ver_short[24];
    strncpy(ver_short, desc->version, sizeof(ver_short) - 1);
    ver_short[sizeof(ver_short) - 1] = '\0';
    { char *p = ver_short; if (*p == 'v') p++; while (*p && *p != '-') p++; *p = '\0'; }

    /* ── Penguin mascot (left column, beside the L/R/usb rows). Cycles a
     *    pose per refresh via eink_lvgl_task. */
    s_label_penguin = make_line(scr, 2, 60, s_penguins[0]);

    /* ── Info column (x=52, unscii_8, 16 px pitch → fills the height) ──
     * Keys padded to align the colons. Static lines drawn once; dynamic
     * lines (lay/L/R/usb/mem) updated by eink_lvgl_task. */
    const int X = 52;
    make_line(scr, X,   4, "kase@dongle");
    make_line(scr, X,  20, "-----------------");
    s_label_layer  = make_line(scr, X,  36, "lay : KaSe");
    s_label_link_l = make_line(scr, X,  52, "L   : --/255 --%");
    s_label_link_r = make_line(scr, X,  68, "R   : --/255 --%");
    s_label_usb    = make_line(scr, X,  84, "usb : ?");
    char b[40];
    snprintf(b, sizeof(b), "set : 0x%04X", set_id);              make_line(scr, X, 100, b);
    snprintf(b, sizeof(b), "net : ch%u %u/%u", wifi_ch, nrf_base, nrf_base + 1);
    make_line(scr, X, 116, b);
    s_label_mem    = make_line(scr, X, 132, "mem : --K --C");
    snprintf(b, sizeof(b), "fls : %luM %luM",
             (unsigned long)(flash_size / (1024UL * 1024UL)),
             (unsigned long)((flash_used + 1024UL * 1024UL - 1) / (1024UL * 1024UL)));
    make_line(scr, X, 148, b);
    snprintf(b, sizeof(b), "soc : ESP32-S3 x%d", chip.cores);    make_line(scr, X, 164, b);
    snprintf(b, sizeof(b), "fw  : %s", ver_short);               make_line(scr, X, 180, b);

    ESP_LOGI(TAG, "eink fastfetch dashboard: set=0x%04X wifi_ch=%u nrf=%u/%u flash=%luM/%luM fw=%s",
             set_id, wifi_ch, nrf_base, nrf_base + 1,
             (unsigned long)(flash_size / (1024UL*1024UL)),
             (unsigned long)((flash_used + 1024UL*1024UL - 1) / (1024UL*1024UL)), ver_short);
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
