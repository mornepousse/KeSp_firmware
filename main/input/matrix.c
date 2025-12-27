#include "matrix.h"
#include <esp_log.h>
#include <stdint.h>
#include <string.h>

#define TAG "MATRIX_SHIM"

// Define variables expected by other code
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
#include "matrix.h"
#include <esp_log.h>
#include <stdint.h>
#include <string.h>
#include "keyboard_button.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "MATRIX_SHIM"

// Define variables expected by other code
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t (*matrix_states[])[MATRIX_ROWS][MATRIX_COLS] = { &MATRIX_STATE, &SLAVE_MATRIX_STATE };
uint8_t keycodes[6];
uint8_t current_press_row[6];
uint8_t current_press_col[6];
uint8_t current_press_stat[6];
uint8_t stat_matrix_changed = 0;
uint8_t last_layer = 0;
uint8_t current_layout = 0;
uint8_t is_layer_changed = 0;
uint32_t last_activity_time_ms = 0;

static keyboard_btn_handle_t s_kbd = NULL;

static void keyboard_btn_cb(keyboard_btn_handle_t kbd_handle, keyboard_btn_report_t kbd_report, void *user_data)
{
    // Clear matrix state
    memset(MATRIX_STATE, 0, sizeof(MATRIX_STATE));
    // Reset current press arrays
    for (int i = 0; i < 6; i++) {
        current_press_row[i] = 255;
        current_press_col[i] = 255;
        current_press_stat[i] = 0;
        keycodes[i] = 0;
    }

    // Fill from key_data (pressed keys)
    uint8_t filled = 0;
    if (kbd_report.key_pressed_num > 0 && kbd_report.key_data) {
        for (uint32_t i = 0; i < kbd_report.key_pressed_num && filled < 6; i++) {
            uint8_t out_idx = kbd_report.key_data[i].output_index;
            uint8_t in_idx = kbd_report.key_data[i].input_index;
            if (in_idx < MATRIX_ROWS && out_idx < MATRIX_COLS) {
                MATRIX_STATE[in_idx][out_idx] = 1;
                current_press_row[filled] = in_idx;
                current_press_col[filled] = out_idx;
                current_press_stat[filled] = 1;
                filled++;
            }
        }
    }

    stat_matrix_changed = 1;
    last_activity_time_ms = esp_timer_get_time() / 1000;

    extern TaskHandle_t keyboard_task_handle; // defined in keyboard_manager.c
    if (keyboard_task_handle != NULL) {
        xTaskNotifyGive(keyboard_task_handle);
    }
}

void rtc_matrix_deinit(void)
{
    ESP_LOGI(TAG, "rtc_matrix_deinit (shim)");
    if (s_kbd) {
        keyboard_button_delete(s_kbd);
        s_kbd = NULL;
    }
}

void matrix_setup(void)
{
    ESP_LOGI(TAG, "matrix_setup (shim)");
    memset(MATRIX_STATE, 0, sizeof(MATRIX_STATE));
    memset(SLAVE_MATRIX_STATE, 0, sizeof(SLAVE_MATRIX_STATE));

    // Build gpio arrays from keyboard_config defines
    static int output_gpios[MATRIX_COLS];
    static int input_gpios[MATRIX_ROWS];
#if defined(COL2ROW)
    // outputs = cols, inputs = rows
    const int cols_map[MATRIX_COLS] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows_map[MATRIX_ROWS] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };
#else
    const int cols_map[MATRIX_COLS] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows_map[MATRIX_ROWS] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };
#endif
    for (int i = 0; i < MATRIX_COLS; i++) output_gpios[i] = cols_map[i];
    for (int i = 0; i < MATRIX_ROWS; i++) input_gpios[i] = rows_map[i];

    keyboard_btn_config_t cfg = {0};
    cfg.output_gpios = output_gpios;
    cfg.input_gpios = input_gpios;
    cfg.output_gpio_num = MATRIX_COLS;
    cfg.input_gpio_num = MATRIX_ROWS;
    cfg.active_level = 1; // common wiring: active high for inputs
    cfg.debounce_ticks = 2; // small debounce
    cfg.ticks_interval = 1000; // sampling interval in us -> 1 kHz
    cfg.enable_power_save = false;
    cfg.priority = 5;
    cfg.core_id = 0;

    esp_err_t res = keyboard_button_create(&cfg, &s_kbd);
    if (res == ESP_OK && s_kbd != NULL) {
        ESP_LOGI(TAG, "keyboard_button created: handle=%p", s_kbd);
        keyboard_btn_cb_config_t cb = {0};
        cb.event = KBD_EVENT_PRESSED;
        cb.callback = keyboard_btn_cb;
        cb.user_data = NULL;
        esp_err_t r2 = keyboard_button_register_cb(s_kbd, cb, NULL);
        if (r2 == ESP_OK) {
            ESP_LOGI(TAG, "keyboard_button callback registered");
        } else {
            ESP_LOGW(TAG, "keyboard_button_register_cb failed: %d", r2);
        }
    } else {
        ESP_LOGW(TAG, "keyboard_button_create failed: %d", res);
    }
}

/* legacy scan functions removed: keyboard_button delivers state via callback */

/* rtc_matrix_setup, matrix_irq_setup and matrix_irq_deinit removed —
 * keyboard_button manages RTC/IRQ behavior and callbacks.
 */

void layer_changed(void)
{
    is_layer_changed = 1;
}

uint32_t get_last_activity_time_ms(void)
{
    return last_activity_time_ms;
}
