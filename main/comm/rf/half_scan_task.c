/*
 * half_scan_task.c — KaSe half firmware scan task (stub).
 *
 * Contains half_diff_emit (pure logic, host-testable) plus the stub task.
 * Full implementation is added in Task 8.
 */

#include "half_scan_task.h"
#include "rf_packet.h"   /* rf_bitmap_set, RF_HALF_ROWS, RF_HALF_COLS */

/* In production: include keyboard_button.h for the real keyboard_btn_data_t.
 * In TEST_HOST: keyboard_btn_data_t is defined in half_scan_task.h. */
#ifndef TEST_HOST
#include "keyboard_button.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "half_scan";
#endif /* TEST_HOST */

/* ────────────────────────────────────────────────────────────── */
/* half_diff_emit — pure bitmap diff helper (also host-tested).   */
/* keyboard_btn_data_t is either the real type (production) or    */
/* the stub typedef (TEST_HOST).                                  */
/* ────────────────────────────────────────────────────────────── */
void half_diff_emit(
    uint8_t *bitmap,
    const keyboard_btn_data_t *pressed,  uint32_t press_cnt,
    const keyboard_btn_data_t *released, uint32_t release_cnt,
    void (*emit_cb)(uint8_t row, uint8_t col, bool pressed, void *ctx),
    void *ctx)
{
    /* Process releases first (matches keyboard convention) */
    for (uint32_t i = 0; i < release_cnt; i++) {
        uint8_t row = released[i].input_index;
        uint8_t col = released[i].output_index;
        if (row < RF_HALF_ROWS && col < RF_HALF_COLS) {
            rf_bitmap_set(bitmap, row, col, false);
            if (emit_cb) emit_cb(row, col, false, ctx);
        }
    }
    /* Then presses */
    for (uint32_t i = 0; i < press_cnt; i++) {
        uint8_t row = pressed[i].input_index;
        uint8_t col = pressed[i].output_index;
        if (row < RF_HALF_ROWS && col < RF_HALF_COLS) {
            rf_bitmap_set(bitmap, row, col, true);
            if (emit_cb) emit_cb(row, col, true, ctx);
        }
    }
}

#ifndef TEST_HOST
static void half_scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "half_scan_task: STUB — awaiting full implementation");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void half_scan_task_start(void)
{
    ESP_LOGI(TAG, "half_scan_task_start (stub)");
    xTaskCreatePinnedToCore(half_scan_task, "half_scan", 4096, NULL, 10, NULL, 0);
}
#endif /* TEST_HOST */
