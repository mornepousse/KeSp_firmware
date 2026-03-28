/* Keystroke statistics and bigram tracking with NVS persistence */
#include "key_stats.h"
#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "keyboard_config.h"
#include "nvs_utils.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "KEY_STATS";

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


/* ── Key stats data ──────────────────────────────────────────────── */

uint32_t key_stats[MATRIX_ROWS][MATRIX_COLS] = {0};
uint32_t key_stats_total = 0;
static uint32_t key_stats_last_saved_total = 0;
static TickType_t key_stats_last_save_tick = 0;

/* ── Bigram data ─────────────────────────────────────────────────── */

uint16_t bigram_stats[NUM_KEYS][NUM_KEYS] = {0};
uint32_t bigram_total = 0;
static int16_t last_key_idx = -1;
static uint32_t bigram_last_saved_total = 0;
static TickType_t bigram_last_save_tick = 0;
static bool bigram_save_disabled = false;

/* ── Record a keypress ───────────────────────────────────────────── */

void key_stats_record_press(uint8_t row, uint8_t col)
{
    if (row >= MATRIX_ROWS || col >= MATRIX_COLS) return;

    key_stats[row][col]++;
    key_stats_total++;

    int16_t curr_idx = row * MATRIX_COLS + col;
    if (last_key_idx >= 0 && last_key_idx < NUM_KEYS) {
        if (bigram_stats[last_key_idx][curr_idx] < UINT16_MAX) {
            bigram_stats[last_key_idx][curr_idx]++;
            bigram_total++;
        }
    }
    last_key_idx = curr_idx;
}

/* ── Query ───────────────────────────────────────────────────────── */

uint32_t get_key_stats_val(uint8_t row, uint8_t col)
{
    if (row < MATRIX_ROWS && col < MATRIX_COLS)
        return key_stats[row][col];
    return 0;
}

uint32_t get_key_stats_max(void)
{
    uint32_t max = 0;
    for (int r = 0; r < MATRIX_ROWS; r++)
        for (int c = 0; c < MATRIX_COLS; c++)
            if (key_stats[r][c] > max)
                max = key_stats[r][c];
    return max;
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

/* ── Reset ───────────────────────────────────────────────────────── */

void reset_key_stats(void)
{
    memset(key_stats, 0, sizeof(key_stats));
    key_stats_total = 0;
    save_key_stats();
    ESP_LOGI(TAG, "Key statistics reset and saved");
}

void reset_bigram_stats(void)
{
    memset(bigram_stats, 0, sizeof(bigram_stats));
    bigram_total = 0;
    last_key_idx = -1;
    save_bigram_stats();
    ESP_LOGI(TAG, "Bigram statistics reset and saved");
}

/* ── NVS persistence ─────────────────────────────────────────────── */

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
    if (bigram_total == 0) {
        for (int i = 0; i < NUM_KEYS; i++)
            for (int j = 0; j < NUM_KEYS; j++)
                bigram_total += bigram_stats[i][j];
    }
    bigram_last_saved_total = bigram_total;
    bigram_last_save_tick = xTaskGetTickCount();
}

void key_stats_check_save(void)
{
    uint32_t diff = key_stats_total - key_stats_last_saved_total;
    TickType_t elapsed = xTaskGetTickCount() - key_stats_last_save_tick;

    if (diff >= KEY_STATS_SAVE_THRESHOLD || (diff > 0 && elapsed >= pdMS_TO_TICKS(KEY_STATS_SAVE_INTERVAL_MS)))
        save_key_stats();

    if (!bigram_save_disabled) {
        uint32_t bg_diff = bigram_total - bigram_last_saved_total;
        TickType_t bg_elapsed = xTaskGetTickCount() - bigram_last_save_tick;
        if (bg_diff >= BIGRAM_SAVE_THRESHOLD || (bg_diff > 0 && bg_elapsed >= pdMS_TO_TICKS(BIGRAM_SAVE_INTERVAL_MS)))
            save_bigram_stats();
    }
}
