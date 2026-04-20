/* Leader Key engine */
#include "leader.h"
#include "keyboard_config.h"
#include "nvs_utils.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "LEADER";

static leader_entry_t entries[LEADER_MAX_ENTRIES];

static bool active = false;
static uint8_t buffer[LEADER_MAX_SEQ_LEN];
static uint8_t buf_len = 0;
static uint32_t last_key_ms = 0;

static uint8_t resolved_key = 0;
static uint8_t resolved_mod = 0;
static bool resolved_flag = false;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void try_match(void)
{
    for (int i = 0; i < LEADER_MAX_ENTRIES; i++) {
        if (entries[i].result == 0) continue;

        /* Compare sequence */
        bool match = true;
        for (int j = 0; j < LEADER_MAX_SEQ_LEN; j++) {
            if (entries[i].sequence[j] == 0 && j == buf_len) break; /* exact match */
            if (j >= buf_len || entries[i].sequence[j] != buffer[j]) {
                match = false;
                break;
            }
            if (entries[i].sequence[j + 1] == 0 && j + 1 == buf_len) break; /* end of entry seq */
        }

        /* Verify lengths match */
        uint8_t entry_len = 0;
        for (int j = 0; j < LEADER_MAX_SEQ_LEN && entries[i].sequence[j] != 0; j++)
            entry_len++;
        if (!match || entry_len != buf_len) continue;

        /* Match found */
        resolved_key = entries[i].result;
        resolved_mod = entries[i].result_mod;
        resolved_flag = true;
        active = false;
        return;
    }
}

static void cancel(void)
{
    active = false;
    buf_len = 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

void leader_init(void)
{
    memset(entries, 0, sizeof(entries));
    active = false;
    buf_len = 0;
    resolved_flag = false;
}

void leader_set(uint8_t index, const leader_entry_t *entry)
{
    if (index < LEADER_MAX_ENTRIES)
        entries[index] = *entry;
}

const leader_entry_t *leader_get(uint8_t index)
{
    if (index >= LEADER_MAX_ENTRIES) return NULL;
    return &entries[index];
}

void leader_start(void)
{
    active = true;
    buf_len = 0;
    last_key_ms = now_ms();
    resolved_flag = false;
}

bool leader_feed(uint8_t keycode)
{
    if (!active || keycode == 0) return false;

    if (buf_len < LEADER_MAX_SEQ_LEN) {
        buffer[buf_len++] = keycode;
        last_key_ms = now_ms();
        try_match();
        return true;
    }

    /* Buffer full, no match → cancel */
    cancel();
    return false;
}

bool leader_tick(void)
{
    /* Result already pending — return true until consumed */
    if (resolved_flag) return true;
    if (!active) return false;

    if ((now_ms() - last_key_ms) >= LEADER_TIMEOUT_MS) {
        /* Timeout — try match with current buffer, then cancel */
        if (buf_len > 0) try_match();
        cancel();
        return resolved_flag;
    }
    return false;
}

uint8_t leader_consume(uint8_t *out_mod)
{
    uint8_t kc = resolved_key;
    if (out_mod) *out_mod = resolved_mod;
    resolved_key = 0;
    resolved_mod = 0;
    resolved_flag = false;
    return kc;
}

bool leader_is_active(void) { return active; }

void leader_save(void)
{
    uint8_t count = 0;
    for (int i = 0; i < LEADER_MAX_ENTRIES; i++)
        if (entries[i].result != 0) count = i + 1;
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "leader_cfg", entries,
                              sizeof(entries), "leader_cnt", count);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to save leader: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "Leader sequences saved (%d)", count);
}

void leader_load(void)
{
    uint32_t count = 0;
    nvs_load_blob_with_total(STORAGE_NAMESPACE, "leader_cfg", entries,
                              sizeof(entries), "leader_cnt", &count);
    if (count > 0)
        ESP_LOGI(TAG, "Leader sequences loaded (%lu)", (unsigned long)count);
}
