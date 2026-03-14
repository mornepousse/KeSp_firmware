#include "matrix.h"
#include <esp_log.h>
#include <stdint.h>
#include <string.h>
#include "keyboard_button.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "round_ui.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "MATRIX_SHIM"
#define STORAGE_NAMESPACE "storage"

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

/* Key usage statistics */
uint32_t key_stats[MATRIX_ROWS][MATRIX_COLS] = {0};
uint32_t key_stats_total = 0;
static uint32_t key_stats_last_saved_total = 0;  /* Track when to save */
static TickType_t key_stats_last_save_tick = 0;

static keyboard_btn_handle_t s_kbd = NULL;
static uint8_t prev_matrix_state[MATRIX_ROWS][MATRIX_COLS];  /* For KPM: track new keypresses */

// Anti-ghosting: detect and filter phantom keys
// Ghost keys form a "rectangle" pattern - if 3 corners are pressed, the 4th is a ghost
static bool is_ghost_key(keyboard_btn_report_t *report, uint8_t check_idx) {
    if (report->key_pressed_num < 2) return false;
    
    uint8_t check_row = report->key_data[check_idx].input_index;
    uint8_t check_col = report->key_data[check_idx].output_index;
    
    // For each other key, check if there's a potential ghost rectangle
    for (uint32_t i = 0; i < report->key_pressed_num; i++) {
        if (i == check_idx) continue;
        uint8_t row_i = report->key_data[i].input_index;
        uint8_t col_i = report->key_data[i].output_index;
        
        // Skip if same row or same column (no rectangle possible)
        if (row_i == check_row || col_i == check_col) continue;
        
        // We have two keys that form diagonal corners of a rectangle
        // Check if the other two corners are also pressed (ghost condition)
        bool corner1 = false, corner2 = false;
        for (uint32_t j = 0; j < report->key_pressed_num; j++) {
            if (j == check_idx || j == i) continue;
            uint8_t row_j = report->key_data[j].input_index;
            uint8_t col_j = report->key_data[j].output_index;
            
            // Corner at (check_row, col_i)
            if (row_j == check_row && col_j == col_i) corner1 = true;
            // Corner at (row_i, check_col)
            if (row_j == row_i && col_j == check_col) corner2 = true;
        }
        
        // If both other corners exist, this forms a ghost rectangle
        // The key with highest column index on the same row is likely the ghost
        if (corner1 || corner2) {
            // Simple heuristic: if this key appeared AFTER another key on same row, it's ghost
            for (uint32_t k = 0; k < check_idx; k++) {
                if (report->key_data[k].input_index == check_row) {
                    return true; // This key came after another on same row = likely ghost
                }
            }
        }
    }
    return false;
}

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

    // Fill from key_data (pressed keys), filtering ghosts
    uint8_t filled = 0;
    uint8_t valid_count = 0;
    uint8_t new_keypresses = 0;  /* Count new keys for KPM */
    if (kbd_report.key_pressed_num > 0 && kbd_report.key_data) {
        for (uint32_t i = 0; i < kbd_report.key_pressed_num && filled < 6; i++) {
            uint8_t out_idx = kbd_report.key_data[i].output_index;
            uint8_t in_idx = kbd_report.key_data[i].input_index;
            
            // Skip ghost keys
            if (kbd_report.key_pressed_num > 1 && is_ghost_key(&kbd_report, i)) {
                ESP_LOGW(TAG, "  GHOST filtered: row=%d col=%d", in_idx, out_idx);
                continue;
            }
            
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
#if defined(COL2ROW)
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
    cfg.debounce_ticks = 1; // Higher debounce to filter ghost keys
    cfg.ticks_interval = 1000; // 8ms scan interval (slower)
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
    nvs_handle_t my_handle;
    esp_err_t err;
    
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS for key_stats!", esp_err_to_name(err));
        return;
    }
    
    /* Save the stats array */
    err = nvs_set_blob(my_handle, "key_stats", key_stats, sizeof(key_stats));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) writing key_stats!", esp_err_to_name(err));
        nvs_close(my_handle);
        return;
    }
    
    /* Save the total count */
    err = nvs_set_u32(my_handle, "key_stats_tot", key_stats_total);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) writing key_stats_total!", esp_err_to_name(err));
    }
    
    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing key_stats!", esp_err_to_name(err));
    }
    
    nvs_close(my_handle);
    key_stats_last_saved_total = key_stats_total;
    key_stats_last_save_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "Key stats saved (total: %lu)", (unsigned long)key_stats_total);
}

void load_key_stats(void)
{
    nvs_handle_t my_handle;
    esp_err_t err;
    
    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No NVS for key_stats (%s), starting fresh", esp_err_to_name(err));
        return;
    }
    
    /* Load the stats array */
    size_t required_size = sizeof(key_stats);
    err = nvs_get_blob(my_handle, "key_stats", key_stats, &required_size);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Key stats loaded from NVS");
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved key_stats found, starting fresh");
    } else {
        ESP_LOGE(TAG, "Error (%s) reading key_stats!", esp_err_to_name(err));
    }
    
    /* Load the total count */
    err = nvs_get_u32(my_handle, "key_stats_tot", &key_stats_total);
    if (err != ESP_OK) {
        /* Recalculate total from array */
        key_stats_total = 0;
        for (int r = 0; r < MATRIX_ROWS; r++) {
            for (int c = 0; c < MATRIX_COLS; c++) {
                key_stats_total += key_stats[r][c];
            }
        }
    }
    
    nvs_close(my_handle);
    key_stats_last_saved_total = key_stats_total;
    key_stats_last_save_tick = xTaskGetTickCount();
    //ESP_LOGI(TAG, "Key stats total: %lu", (unsigned long)key_stats_total);
}

/**
 * @brief Check if stats should be saved (call periodically)
 * Saves every 100 keypresses OR every 60 seconds if there are changes
 */
void key_stats_check_save(void)
{
    uint32_t diff = key_stats_total - key_stats_last_saved_total;
    TickType_t elapsed = xTaskGetTickCount() - key_stats_last_save_tick;
    
    /* Save if 100+ new keypresses, or 60s elapsed with changes */
    if (diff >= 100 || (diff > 0 && elapsed >= pdMS_TO_TICKS(60000))) {
        save_key_stats();
    }
}
