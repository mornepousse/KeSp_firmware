#pragma once
/*
 * eink_lvgl.h — Raw LVGL v8 integration for SSD1681 e-ink (KaSe half).
 *
 * Monochrome driver recipe: set_px_cb + rounder_cb + flush_cb.
 * - set_px_cb packs each pixel directly into the 5000-byte s_fb (MSB-first, bit=1→white).
 * - rounder_cb byte-aligns the dirty area (x to multiples of 8).
 * - flush_cb calls eink_push(s_fb) on the last flush fragment, then lv_disp_flush_ready().
 * - Draw buffer: 2000-byte stripe (EINK_WIDTH × 10 rows) — scratch only, never interpreted.
 *
 * Uses raw LVGL (no esp_lvgl_port). Rationale: SSD1681 has no esp_lcd driver
 * in this project; a direct set_px_cb is simpler and self-contained.
 *
 * Call order in half_scan_task (after eink_init() returns true):
 *   eink_lvgl_init();    // lv_init, draw buf, disp drv, tick timer, screen content
 *   eink_lvgl_start();   // create eink_lvgl_task (prio 3, core 0)
 *
 * Thread safety: for v1 (static screen), only eink_lvgl_task touches LVGL.
 * Plan Bricks-4 (dynamic content) must add a mutex before calling lv_label_set_text
 * from an ESP-NOW callback.
 */

/* Initialize LVGL, register 1bpp display driver, create static screen content.
 * Must be called AFTER eink_init() returns true and BEFORE eink_lvgl_start(). */
void eink_lvgl_init(void);

/* Create the LVGL handler task (eink_lvgl_task, prio 3, stack 4096, core 0).
 * Replaces the old eink_task polling loop. */
void eink_lvgl_start(void);
