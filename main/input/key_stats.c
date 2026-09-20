/* Keystroke statistics and bigram tracking with NVS persistence */
#include "key_stats.h"
#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "keyboard_config.h"
#ifndef TEST_HOST
#include "nvs_utils.h"
#include "nvs.h"
#include "freertos/task.h"
#endif
#include "freertos/FreeRTOS.h"

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
#ifndef TEST_HOST
static uint32_t key_stats_last_saved_total = 0;
static TickType_t key_stats_last_save_tick = 0;
#endif

/* ── Bigram data ─────────────────────────────────────────────────── */

uint16_t bigram_stats[NUM_KEYS][NUM_KEYS] = {0};
uint32_t bigram_total = 0;
static int16_t last_key_idx = -1;
#ifndef TEST_HOST
#endif

/* ── Record a keypress ───────────────────────────────────────────── */

void key_stats_record_press(uint8_t row, uint8_t col)
{
#if defined(CONFIG_KASE_KEY_STATS) && !CONFIG_KASE_KEY_STATS
    (void)row; (void)col; return;   /* no stats on the halves (Kconfig) */
#endif
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

#ifndef TEST_HOST
void save_key_stats(void)
{
#if defined(CONFIG_KASE_KEY_STATS) && !CONFIG_KASE_KEY_STATS
    return;   /* never write stats to NVS on a half */
#endif
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
#if defined(CONFIG_KASE_KEY_STATS) && !CONFIG_KASE_KEY_STATS
    return;   /* nothing to load: a half doesn't count */
#endif
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

/* Bigram persistence removed — the counters live in RAM, for the duration of a
 * session.
 *
 * The blob is 8450 bytes (uint16_t[NUM_KEYS][NUM_KEYS]) and its write started
 * from key_stats_check_save(), called by the display task. But an NVS write
 * disables the instruction cache for the duration of the flash operation: all
 * code running from flash stops, regardless of its priority. The scan tick
 * survives it (the gptimer callback is IRAM_ATTR) but key processing and
 * HID sending do not. This was the only path by which the display — despite
 * being priority 2, below the scan (5), HID sending (4) and processing (3) —
 * could steal time from typing.
 *
 * What remains: the counting, reset_bigram_stats(), and the CDC commands
 * KS_CMD_BIGRAMS_BIN / _TEXT / _RESET, which answer on the current session.
 * What is lost: statistics start over from zero on every restart. */
void save_bigram_stats(void) {}

/* Counterpart of save_bigram_stats(): nothing to reload. Do NOT re-read the old
 * NVS blob — since nobody writes it anymore, it would stay frozen forever and
 * mask the session's counting. */
void load_bigram_stats(void) {}

void key_stats_check_save(void)
{
#if defined(CONFIG_KASE_KEY_STATS) && !CONFIG_KASE_KEY_STATS
    return;
#endif
    uint32_t diff = key_stats_total - key_stats_last_saved_total;
    TickType_t elapsed = xTaskGetTickCount() - key_stats_last_save_tick;

    if (diff >= KEY_STATS_SAVE_THRESHOLD || (diff > 0 && elapsed >= pdMS_TO_TICKS(KEY_STATS_SAVE_INTERVAL_MS)))
        save_key_stats();

    /* Bigrams are no longer persisted (see save_bigram_stats). The key_stats
     * blob is still written here: it has NUM_KEYS entries, not NUM_KEYS², so
     * a much shorter write. */
}
#else
void save_key_stats(void)    {}
void load_key_stats(void)    {}
void save_bigram_stats(void) {}
void load_bigram_stats(void) {}
void key_stats_check_save(void) {}
#endif
