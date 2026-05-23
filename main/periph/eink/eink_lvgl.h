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
 * Thread safety: LVGL calls are confined to eink_lvgl_task only.
 * ESP-NOW callbacks write to g_half_state (under mutex) then xTaskNotify —
 * eink_lvgl_task wakes, reads g_half_state, calls lv_label_set_text safely.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Initialize LVGL, register 1bpp display driver, create static screen content.
 * Must be called AFTER eink_init() returns true and BEFORE eink_lvgl_start(). */
void eink_lvgl_init(void);

/* Create the LVGL handler task (eink_lvgl_task, prio 3, stack 4096, core 0).
 * Replaces the old eink_task polling loop. */
void eink_lvgl_start(void);

/* Returns the FreeRTOS task handle of the eink_lvgl_task, or NULL if not started.
 * Used by espnow_info.c to wake the task when layer/state changes.
 * Call xTaskNotify(eink_get_task_handle(), 0x01, eSetBits) — bit 0 = layer/state event.
 * Always guard with: if (h != NULL) before calling xTaskNotify. */
TaskHandle_t eink_get_task_handle(void);
