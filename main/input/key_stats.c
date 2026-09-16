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
    (void)row; (void)col; return;   /* pas de stats sur les moitiés (Kconfig) */
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
    return;   /* jamais d'écriture NVS de stats sur une moitié */
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
    return;   /* rien à charger : une moitié ne compte pas */
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

/* Persistance des bigrams retirée — les compteurs vivent en RAM, le temps d'une
 * session.
 *
 * Le blob fait 8450 octets (uint16_t[NUM_KEYS][NUM_KEYS]) et son écriture partait
 * de key_stats_check_save(), appelée par la tâche d'affichage. Or une écriture
 * NVS désactive le cache d'instructions le temps de l'opération flash : tout code
 * exécuté depuis la flash s'arrête, quelle que soit sa priorité. Le tick de scan
 * y survit (le callback gptimer est IRAM_ATTR) mais le traitement des touches et
 * l'envoi HID, non. C'était le seul chemin par lequel l'affichage — pourtant en
 * priorité 2, sous le scan (5), l'envoi HID (4) et le traitement (3) — pouvait
 * voler du temps à la frappe.
 *
 * Ce qui reste : le comptage, reset_bigram_stats(), et les commandes CDC
 * KS_CMD_BIGRAMS_BIN / _TEXT / _RESET, qui répondent sur la session courante.
 * Ce qui est perdu : les statistiques repartent de zéro à chaque redémarrage. */
void save_bigram_stats(void) {}

/* Pendant de save_bigram_stats() : rien à recharger. Ne PAS relire l'ancien blob
 * NVS — plus personne ne l'écrivant, il resterait figé à jamais et masquerait le
 * comptage de la session. */
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

    /* Les bigrams ne sont plus persistés (voir save_bigram_stats). Le blob de
     * key_stats reste écrit ici : il fait NUM_KEYS entrées et non NUM_KEYS², donc
     * une écriture bien plus courte. */
}
#else
void save_key_stats(void)    {}
void load_key_stats(void)    {}
void save_bigram_stats(void) {}
void load_bigram_stats(void) {}
void key_stats_check_save(void) {}
#endif
