#include "matrix_scan.h"
#include "keyboard_config.h"
#include <esp_log.h>
#include <stdint.h>
#include <string.h>
#include "keyboard_button.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "round_ui.h"
#include "nvs_utils.h"
#include "nvs.h"  /* ESP_ERR_NVS_NOT_ENOUGH_SPACE */

#define TAG "MATRIX_SHIM"

#ifndef KEY_STATS_SAVE_THRESHOLD
#define KEY_STATS_SAVE_THRESHOLD      100
#endif
#ifndef KEY_STATS_SAVE_INTERVAL_MS
#define KEY_STATS_SAVE_INTERVAL_MS    60000
#endif
#ifndef BIGRAM_SAVE_THRESHOLD
#define BIGRAM_SAVE_THRESHOLD         100
#endif
#ifndef BIGRAM_SAVE_INTERVAL_MS
#define BIGRAM_SAVE_INTERVAL_MS       120000
#endif

// Define variables expected by other code
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t (*matrix_states[])[MATRIX_ROWS][MATRIX_COLS] = { &MATRIX_STATE, &SLAVE_MATRIX_STATE };
#define MAX_REPORT_KEYS  6       /* HID boot protocol: max 6 simultaneous keys */

uint8_t keycodes[MAX_REPORT_KEYS];
uint8_t current_press_row[MAX_REPORT_KEYS];
uint8_t current_press_col[MAX_REPORT_KEYS];
uint8_t current_press_stat[MAX_REPORT_KEYS];
uint8_t stat_matrix_changed = 0;
uint8_t last_layer = 0;
uint8_t current_layout = 0;
uint8_t is_layer_changed = 0;
uint32_t last_activity_time_ms = 0;

/* Key usage statistics */
uint32_t key_stats[MATRIX_ROWS][MATRIX_COLS] = {0};
uint32_t key_stats_total = 0;
static uint32_t key_stats_last_saved_total = 0;  /* Track when to save */
static TickType_t key_stats_last_save_tick = 0;

/* Bigram tracking */
uint16_t bigram_stats[NUM_KEYS][NUM_KEYS] = {0};
uint32_t bigram_total = 0;
static int16_t last_key_idx = -1;  /* -1 = no previous key */
static uint32_t bigram_last_saved_total = 0;
static TickType_t bigram_last_save_tick = 0;
static bool bigram_save_disabled = false;  /* Set on NVS_NOT_ENOUGH_SPACE to stop retrying */

static keyboard_btn_handle_t s_kbd = NULL;
static uint8_t prev_matrix_state[MATRIX_ROWS][MATRIX_COLS];  /* For KPM: track new keypresses */


static void keyboard_btn_cb(keyboard_btn_handle_t kbd_handle, keyboard_btn_report_t kbd_report, void *user_data)
{
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
            ESP_LOGI(TAG, "  Key[%d]: row=%d col=%d", valid_count, in_idx, out_idx);
            if (in_idx < MATRIX_ROWS && out_idx < MATRIX_COLS) {
                MATRIX_STATE[in_idx][out_idx] = 1;
                /* Count NEW keypresses (wasn't pressed before) */
                if (!prev_matrix_state[in_idx][out_idx]) {
                    new_keypresses++;
                    /* Update key statistics */
                    key_stats[in_idx][out_idx]++;
                    key_stats_total++;
                    /* Update bigram */
                    int16_t curr_idx = in_idx * MATRIX_COLS + out_idx;
                    if (last_key_idx >= 0 && last_key_idx < NUM_KEYS) {
                        if (bigram_stats[last_key_idx][curr_idx] < UINT16_MAX) {
                            bigram_stats[last_key_idx][curr_idx]++;
                            bigram_total++;
                        }
                    }
                    last_key_idx = curr_idx;
                }
                current_press_row[filled] = in_idx;
                current_press_col[filled] = out_idx;
                current_press_stat[filled] = 1;
                filled++;
            }
        }
    }
    
    /* Notify KPM tracker of new keypresses */
    for (uint8_t k = 0; k < new_keypresses; k++) {
        round_ui_notify_keypress();
    }
    
    /* Save current state for next comparison */
    memcpy(prev_matrix_state, MATRIX_STATE, sizeof(MATRIX_STATE));

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
#if defined(BOARD_MATRIX_COL2ROW)
    // outputs = cols, inputs = rows
    const int cols_map[MATRIX_COLS] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows_map[MATRIX_ROWS] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };
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

/* Key statistics functions */
uint32_t get_key_stats(uint8_t row, uint8_t col)
{
    if (row < MATRIX_ROWS && col < MATRIX_COLS) {
        return key_stats[row][col];
    }
    return 0;
}

void reset_key_stats(void)
{
    memset(key_stats, 0, sizeof(key_stats));
    key_stats_total = 0;
    save_key_stats();  /* Persist the reset */
    ESP_LOGI(TAG, "Key statistics reset and saved");
}

uint32_t get_key_stats_max(void)
{
    uint32_t max = 0;
    for (int r = 0; r < MATRIX_ROWS; r++) {
        for (int c = 0; c < MATRIX_COLS; c++) {
            if (key_stats[r][c] > max) {
                max = key_stats[r][c];
            }
        }
    }
    return max;
}

/* ============ Key Stats NVS Persistence ============ */

void save_key_stats(void)
{
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "key_stats", key_stats,
                                              sizeof(key_stats), "key_stats_tot", key_stats_total);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save key_stats: %s", esp_err_to_name(err));
        return;
    }
    key_stats_last_saved_total = key_stats_total;
    key_stats_last_save_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "Key stats saved (total: %lu)", (unsigned long)key_stats_total);
}

void load_key_stats(void)
{
    nvs_load_blob_with_total(STORAGE_NAMESPACE, "key_stats", key_stats,
                              sizeof(key_stats), "key_stats_tot", &key_stats_total);

    if (key_stats_total == 0) {
        for (int r = 0; r < MATRIX_ROWS; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                key_stats_total += key_stats[r][c];
    }
    key_stats_last_saved_total = key_stats_total;
    key_stats_last_save_tick = xTaskGetTickCount();
}

/**
 * @brief Check if stats should be saved (call periodically)
 * Saves every 100 keypresses OR every 60 seconds if there are changes
 */
void key_stats_check_save(void)
{
    uint32_t diff = key_stats_total - key_stats_last_saved_total;
    TickType_t elapsed = xTaskGetTickCount() - key_stats_last_save_tick;

    if (diff >= KEY_STATS_SAVE_THRESHOLD || (diff > 0 && elapsed >= pdMS_TO_TICKS(KEY_STATS_SAVE_INTERVAL_MS))) {
        save_key_stats();
    }

    /* Also check bigram save (skip if NVS is too small) */
    if (!bigram_save_disabled) {
        uint32_t bg_diff = bigram_total - bigram_last_saved_total;
        TickType_t bg_elapsed = xTaskGetTickCount() - bigram_last_save_tick;
        if (bg_diff >= BIGRAM_SAVE_THRESHOLD || (bg_diff > 0 && bg_elapsed >= pdMS_TO_TICKS(BIGRAM_SAVE_INTERVAL_MS))) {
            save_bigram_stats();
        }
    }
}

/* ============ Bigram Stats ============ */

void save_bigram_stats(void)
{
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "bigram_stats", bigram_stats,
                                              sizeof(bigram_stats), "bigram_total", bigram_total);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
            ESP_LOGW(TAG, "NVS too small for bigram_stats (%u bytes), disabling save", (unsigned)sizeof(bigram_stats));
            bigram_save_disabled = true;
        } else {
            ESP_LOGE(TAG, "Failed to save bigram_stats: %s", esp_err_to_name(err));
        }
        return;
    }
    bigram_last_saved_total = bigram_total;
    bigram_last_save_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "Bigram stats saved (total: %lu)", (unsigned long)bigram_total);
}

void load_bigram_stats(void)
{
    nvs_load_blob_with_total(STORAGE_NAMESPACE, "bigram_stats", bigram_stats,
                              sizeof(bigram_stats), "bigram_total", &bigram_total);

    /* If total wasn't saved, recalculate from array */
    if (bigram_total == 0) {
        for (int i = 0; i < NUM_KEYS; i++)
            for (int j = 0; j < NUM_KEYS; j++)
                bigram_total += bigram_stats[i][j];
    }
    bigram_last_saved_total = bigram_total;
    bigram_last_save_tick = xTaskGetTickCount();
}

void reset_bigram_stats(void)
{
    memset(bigram_stats, 0, sizeof(bigram_stats));
    bigram_total = 0;
    last_key_idx = -1;
    save_bigram_stats();
    ESP_LOGI(TAG, "Bigram statistics reset and saved");
}

uint16_t get_bigram_stats_max(void)
{
    uint16_t max = 0;
    for (int i = 0; i < NUM_KEYS; i++)
        for (int j = 0; j < NUM_KEYS; j++)
            if (bigram_stats[i][j] > max)
                max = bigram_stats[i][j];
    return max;
}
