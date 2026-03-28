/* Combo engine: detect simultaneous key presses */
#include "combo.h"
#include "matrix_scan.h"
#include "keyboard_config.h"
#include "nvs_utils.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "COMBO";

static combo_config_t configs[COMBO_MAX_SLOTS];
static uint8_t resolved_queue[COMBO_MAX_SLOTS];
static uint8_t resolved_count = 0;

/* Track which combos are currently active (both keys held) */
static bool combo_active[COMBO_MAX_SLOTS];

static bool key_in_report(uint8_t row, uint8_t col,
                          const uint8_t press_row[6], const uint8_t press_col[6])
{
    for (int i = 0; i < 6; i++) {
        if (press_row[i] == row && press_col[i] == col)
            return true;
    }
    return false;
}

void combo_init(void)
{
    memset(configs, 0, sizeof(configs));
    memset(combo_active, 0, sizeof(combo_active));
    resolved_count = 0;
}

void combo_set(uint8_t index, const combo_config_t *cfg)
{
    if (index < COMBO_MAX_SLOTS)
        configs[index] = *cfg;
}

const combo_config_t *combo_get(uint8_t index)
{
    if (index >= COMBO_MAX_SLOTS) return NULL;
    return &configs[index];
}

int combo_process(const uint8_t press_row[6], const uint8_t press_col[6])
{
    resolved_count = 0;

    for (int i = 0; i < COMBO_MAX_SLOTS; i++) {
        if (configs[i].result == 0) continue; /* unconfigured slot */

        bool key1 = key_in_report(configs[i].row1, configs[i].col1, press_row, press_col);
        bool key2 = key_in_report(configs[i].row2, configs[i].col2, press_row, press_col);
        bool both = key1 && key2;

        if (both && !combo_active[i]) {
            /* Both keys just pressed together → trigger combo */
            combo_active[i] = true;
            if (resolved_count < COMBO_MAX_SLOTS)
                resolved_queue[resolved_count++] = configs[i].result;
        } else if (!both) {
            combo_active[i] = false;
        }
    }

    return resolved_count;
}

uint8_t combo_consume(void)
{
    if (resolved_count == 0) return 0;
    return resolved_queue[--resolved_count];
}

void combo_save(void)
{
    uint8_t count = 0;
    for (int i = 0; i < COMBO_MAX_SLOTS; i++) {
        if (configs[i].result != 0) count = i + 1;
    }
    nvs_save_blob_with_total(STORAGE_NAMESPACE, "combo_cfg", configs,
                              count * sizeof(combo_config_t), "combo_cnt", count);
    ESP_LOGI(TAG, "Combos saved (%d slots)", count);
}

void combo_load(void)
{
    uint32_t count = 0;
    nvs_load_blob_with_total(STORAGE_NAMESPACE, "combo_cfg", configs,
                              sizeof(configs), "combo_cnt", &count);
    if (count > 0)
        ESP_LOGI(TAG, "Combos loaded (%lu slots)", (unsigned long)count);
}
