#include "matrix_scan.h"
#include "keyboard_task.h"
#include "key_stats.h"
#include "keyboard_config.h"
#include <esp_log.h>
#include <stdint.h>
#include <string.h>
#include "keyboard_button.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_display.h"

#define TAG "MATRIX_SCAN"

// Define variables expected by other code
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t (*matrix_states[])[MATRIX_ROWS][MATRIX_COLS] = { &MATRIX_STATE, &SLAVE_MATRIX_STATE };
#define MAX_REPORT_KEYS  6       /* HID boot protocol: max 6 simultaneous keys */

uint8_t keycodes[MAX_REPORT_KEYS];
uint8_t current_press_row[MAX_REPORT_KEYS];
uint8_t current_press_col[MAX_REPORT_KEYS];
uint8_t current_press_stat[MAX_REPORT_KEYS];
volatile uint8_t stat_matrix_changed = 0;
uint8_t last_layer = 0;
uint8_t current_layout = 0;
volatile uint8_t is_layer_changed = 0;
volatile uint32_t last_activity_time_ms = 0;

static keyboard_btn_handle_t s_kbd = NULL;
static uint8_t prev_matrix_state[MATRIX_ROWS][MATRIX_COLS];  /* For KPM: track new keypresses */


static void keyboard_btn_cb(keyboard_btn_handle_t kbd_handle, keyboard_btn_report_t kbd_report, void *user_data)
{
    ESP_LOGI(TAG, "CB: pressed=%lu", (unsigned long)kbd_report.key_pressed_num);
    // Clear matrix state
    memset(MATRIX_STATE, 0, sizeof(MATRIX_STATE));
    // Reset current press arrays
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i] = INVALID_KEY_POS;
        current_press_col[i] = INVALID_KEY_POS;
        current_press_stat[i] = 0;
        keycodes[i] = 0;
    }

    // Fill from key_data (pressed keys), filtering ghosts
    uint8_t filled = 0;
    uint8_t valid_count = 0;
    uint8_t new_keypresses = 0;  /* Count new keys for KPM */
    if (kbd_report.key_pressed_num > 0 && kbd_report.key_data) {
        for (uint32_t i = 0; i < kbd_report.key_pressed_num && filled < MAX_REPORT_KEYS; i++) {
            uint8_t out_idx = kbd_report.key_data[i].output_index;
            uint8_t in_idx = kbd_report.key_data[i].input_index;
            
            // Ghost filtering disabled: each key has its own diode,
            // so ghosting cannot occur. The filter caused stuck keys
            // by not recording ghost-filtered keys in MATRIX_STATE.
            
            valid_count++;
            if (in_idx < MATRIX_ROWS && out_idx < MATRIX_COLS) {
                MATRIX_STATE[in_idx][out_idx] = 1;
                /* Count NEW keypresses (wasn't pressed before) */
                if (!prev_matrix_state[in_idx][out_idx]) {
                    new_keypresses++;
                    key_stats_record_press(in_idx, out_idx);
                }
                current_press_row[filled] = in_idx;
                current_press_col[filled] = out_idx;
                current_press_stat[filled] = 1;
                filled++;
            }
        }
    }
    
    /* Notify display of new keypresses (for KPM tracking) */
    for (uint8_t k = 0; k < new_keypresses; k++) {
        status_display_notify_keypress();
    }
    
    /* Save current state for next comparison */
    memcpy(prev_matrix_state, MATRIX_STATE, sizeof(MATRIX_STATE));

    stat_matrix_changed = 1;
    last_activity_time_ms = esp_timer_get_time() / 1000;

    /* keyboard_task_handle declared in keyboard_task.h */
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
#if defined(BOARD_MATRIX_COL2ROW)
    const int cols_map[MATRIX_COLS] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows_map[MATRIX_ROWS] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };

    /* Reset all matrix GPIOs — clears any function set by ROM bootloader
       (e.g. UART0 on GPIO43/44 which conflicts with V2 cols 7/8) */
    for (int i = 0; i < MATRIX_COLS; i++) gpio_reset_pin(cols_map[i]);
    for (int i = 0; i < MATRIX_ROWS; i++) gpio_reset_pin(rows_map[i]);
#else
    const int cols_map[MATRIX_COLS] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows_map[MATRIX_ROWS] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };
#endif
    ESP_LOGI(TAG, "Cols (outputs): ");
    for (int i = 0; i < MATRIX_COLS; i++) {
        output_gpios[i] = cols_map[i];
        ESP_LOGI(TAG, "  COL%d = GPIO%d", i, output_gpios[i]);
    }
    ESP_LOGI(TAG, "Rows (inputs): ");
    for (int i = 0; i < MATRIX_ROWS; i++) {
        input_gpios[i] = rows_map[i];
        ESP_LOGI(TAG, "  ROW%d = GPIO%d", i, input_gpios[i]);
    }

    keyboard_btn_config_t cfg = {0};
    cfg.output_gpios = output_gpios;
    cfg.input_gpios = input_gpios;
    cfg.output_gpio_num = MATRIX_COLS;
    cfg.input_gpio_num = MATRIX_ROWS;
    cfg.active_level = 1; // Active HIGH 
    cfg.debounce_ticks = BOARD_DEBOUNCE_TICKS;
    cfg.ticks_interval = BOARD_MATRIX_SCAN_INTERVAL_US;
    cfg.enable_power_save = false;
    cfg.priority = 5;
    cfg.core_id = 0;

    esp_err_t res = keyboard_button_create(&cfg, &s_kbd);
    if (res == ESP_OK && s_kbd != NULL) {
        ESP_LOGI(TAG, "keyboard_button created: handle=%p", s_kbd);
        
        // KBD_EVENT_PRESSED is called for all changes (press AND release)
        keyboard_btn_cb_config_t cb_pressed = {0};
        cb_pressed.event = KBD_EVENT_PRESSED;
        cb_pressed.callback = keyboard_btn_cb;
        cb_pressed.user_data = NULL;
        esp_err_t r1 = keyboard_button_register_cb(s_kbd, cb_pressed, NULL);
        
        if (r1 == ESP_OK) {
            ESP_LOGI(TAG, "keyboard_button callback registered");
        } else {
            ESP_LOGW(TAG, "keyboard_button_register_cb failed: %d", r1);
        }
    } else {
        ESP_LOGW(TAG, "keyboard_button_create failed: %d", res);
    }
}

void layer_changed(void)
{
    is_layer_changed = 1;
}

uint32_t get_last_activity_time_ms(void)
{
    return last_activity_time_ms;
}

