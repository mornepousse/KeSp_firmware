/* Combo engine: detect simultaneous key presses with deferral.
   When a key that belongs to any combo is pressed, it is held back
   for up to COMBO_TIMEOUT_MS. If the partner key arrives in time,
   the combo result is emitted and both individual keys are suppressed.
   If the timeout expires, the deferred key is released normally. */
#include "combo.h"
#include "keyboard_config.h"
#include "nvs_utils.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "COMBO";

static combo_config_t configs[COMBO_MAX_SLOTS];

/* ── Deferral state ─────────────────────────────────────────────── */

/* A deferred key: one half of a potential combo, waiting for partner */
typedef struct {
    uint8_t row, col;
    uint8_t keycode;       /* HID keycode to emit if timeout */
    int64_t press_time_us;
    bool    active;
} deferred_key_t;

#define MAX_DEFERRED 4
static deferred_key_t deferred[MAX_DEFERRED];

/* Resolved combos ready to inject */
static uint8_t resolved_queue[COMBO_MAX_SLOTS];
static uint8_t resolved_idx[COMBO_MAX_SLOTS];
static uint8_t resolved_count = 0;

/* Track which combos are currently active (both keys held) */
static bool combo_active[COMBO_MAX_SLOTS];

/* Expired deferred keys to release as normal keypresses */
static uint8_t expired_queue[MAX_DEFERRED];
static uint8_t expired_count = 0;

/* ── Helpers ────────────────────────────────────────────────────── */

static bool key_in_report(uint8_t row, uint8_t col,
                          const uint8_t press_row[6], const uint8_t press_col[6])
{
    for (int i = 0; i < 6; i++) {
        if (press_row[i] == row && press_col[i] == col)
            return true;
    }
    return false;
}

/* Check if a position is part of any configured combo */
static bool is_combo_key(uint8_t row, uint8_t col)
{
    for (int i = 0; i < COMBO_MAX_SLOTS; i++) {
        if (configs[i].result == 0) continue;
        if ((configs[i].row1 == row && configs[i].col1 == col) ||
            (configs[i].row2 == row && configs[i].col2 == col))
            return true;
    }
    return false;
}

static deferred_key_t *find_deferred(uint8_t row, uint8_t col)
{
    for (int i = 0; i < MAX_DEFERRED; i++) {
        if (deferred[i].active && deferred[i].row == row && deferred[i].col == col)
            return &deferred[i];
    }
    return NULL;
}

static deferred_key_t *alloc_deferred(void)
{
    for (int i = 0; i < MAX_DEFERRED; i++) {
        if (!deferred[i].active) return &deferred[i];
    }
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────── */

void combo_init(void)
{
    memset(configs, 0, sizeof(configs));
    memset(deferred, 0, sizeof(deferred));
    memset(combo_active, 0, sizeof(combo_active));
    resolved_count = 0;
    expired_count = 0;
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

/* Check if key is part of an active (fired) combo */
static bool is_active_combo_key(uint8_t row, uint8_t col)
{
    for (int i = 0; i < COMBO_MAX_SLOTS; i++) {
        if (!combo_active[i] || configs[i].result == 0) continue;
        if ((configs[i].row1 == row && configs[i].col1 == col) ||
            (configs[i].row2 == row && configs[i].col2 == col))
            return true;
    }
    return false;
}

bool combo_is_suppressed(uint8_t row, uint8_t col)
{
    return find_deferred(row, col) != NULL || is_active_combo_key(row, col);
}

void combo_defer_key(uint8_t row, uint8_t col, uint8_t keycode)
{
    if (find_deferred(row, col)) return; /* already deferred */
    deferred_key_t *d = alloc_deferred();
    if (!d) return; /* no room, key will pass through normally */
    d->row = row;
    d->col = col;
    d->keycode = keycode;
    d->press_time_us = esp_timer_get_time();
    d->active = true;
}

bool combo_should_defer(uint8_t row, uint8_t col)
{
    return is_combo_key(row, col);
}

int combo_process(const uint8_t press_row[6], const uint8_t press_col[6])
{
    resolved_count = 0;
    expired_count = 0;
    int64_t now = esp_timer_get_time();

    /* Check each combo config */
    for (int i = 0; i < COMBO_MAX_SLOTS; i++) {
        if (configs[i].result == 0) continue;

        bool key1 = key_in_report(configs[i].row1, configs[i].col1, press_row, press_col);
        bool key2 = key_in_report(configs[i].row2, configs[i].col2, press_row, press_col);
        bool both = key1 && key2;

        if (both && !combo_active[i]) {
            combo_active[i] = true;
            /* Check that at least one key is deferred (within timeout) */
            deferred_key_t *d1 = find_deferred(configs[i].row1, configs[i].col1);
            deferred_key_t *d2 = find_deferred(configs[i].row2, configs[i].col2);
            if (d1 || d2) {
                /* Combo triggered — consume both deferred keys */
                if (d1) d1->active = false;
                if (d2) d2->active = false;
                if (resolved_count < COMBO_MAX_SLOTS) {
                    resolved_idx[resolved_count] = (uint8_t)i;
                    resolved_queue[resolved_count++] = configs[i].result;
                }
            }
        } else if (!both) {
            combo_active[i] = false;
        }
    }

    /* Check for expired deferred keys (timeout passed, no combo) */
    for (int i = 0; i < MAX_DEFERRED; i++) {
        if (!deferred[i].active) continue;
        /* Still pressed? Check if it's still in the report */
        bool still_pressed = key_in_report(deferred[i].row, deferred[i].col, press_row, press_col);
        int64_t age_us = now - deferred[i].press_time_us;

        if (age_us > (int64_t)COMBO_TIMEOUT_MS * 1000 || !still_pressed) {
            /* Timeout or released: emit the original key */
            if (expired_count < MAX_DEFERRED)
                expired_queue[expired_count++] = deferred[i].keycode;
            deferred[i].active = false;
        }
    }

    return resolved_count;
}

uint8_t combo_consume(uint8_t *out_r1, uint8_t *out_c1, uint8_t *out_r2, uint8_t *out_c2)
{
    if (resolved_count == 0) return 0;
    resolved_count--;
    uint8_t idx = resolved_idx[resolved_count];
    if (out_r1) *out_r1 = configs[idx].row1;
    if (out_c1) *out_c1 = configs[idx].col1;
    if (out_r2) *out_r2 = configs[idx].row2;
    if (out_c2) *out_c2 = configs[idx].col2;
    return resolved_queue[resolved_count];
}

uint8_t combo_consume_expired(void)
{
    if (expired_count == 0) return 0;
    return expired_queue[--expired_count];
}

void combo_save(void)
{
    uint8_t count = 0;
    for (int i = 0; i < COMBO_MAX_SLOTS; i++) {
        if (configs[i].result != 0) count = i + 1;
    }
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "combo_cfg", configs,
                                              count * sizeof(combo_config_t), "combo_cnt", count);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to save combos: %s", esp_err_to_name(err));
    else
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
