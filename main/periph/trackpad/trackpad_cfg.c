/*
 * trackpad_cfg.c — Dongle-side runtime config for trackpad accel curve.
 *
 * Loads persisted cfg from NVS at boot; exposes trackpad_cfg_active() used
 * by rf_rx_task.c (PKT_TYPE_TRACKPAD). CDC handlers (0xB8/0xB9) call
 * trackpad_cfg_apply_and_save() to change the live cfg and persist it.
 *
 * Dongle only: gated by CONFIG_KASE_HAS_RF_RX in CMakeLists.txt.
 * No malloc. No FreeRTOS. No I/O in trackpad_map.c (pure).
 */

#include "trackpad.h"
#include "nvs_utils.h"
#include "esp_log.h"
#include <string.h>

#define TP_NS        "storage"
#define TP_KEY       "tp_cfg"
#define TP_TOT_KEY   "tp_cfg_tot"   /* unused counter, required by nvs_utils API */

static const char *TAG = "tp_cfg";

static trackpad_cfg_t s_cfg = {
    .fmt      = TRACKPAD_CFG_FMT,
    .base     = 90,
    .accel    = 40,
    .gain_max = 300,
};

static bool cfg_valid(const trackpad_cfg_t *c)
{
    return c->gain_max <= 1000
        && c->accel    <= 1000
        && c->base     >= 1
        && c->base     <= c->gain_max;
}

void trackpad_cfg_load(void)
{
    trackpad_cfg_t tmp;
    uint32_t       total = 0;
    esp_err_t err = nvs_load_blob_with_total(TP_NS, TP_KEY, &tmp, sizeof(tmp),
                                              TP_TOT_KEY, &total);
    if (err == ESP_OK && tmp.fmt == TRACKPAD_CFG_FMT && cfg_valid(&tmp)) {
        s_cfg = tmp;
        ESP_LOGI(TAG, "loaded cfg base=%u accel=%u gain_max=%u",
                 (unsigned)s_cfg.base, (unsigned)s_cfg.accel, (unsigned)s_cfg.gain_max);
    } else {
        ESP_LOGI(TAG, "using defaults base=%u accel=%u gain_max=%u",
                 (unsigned)s_cfg.base, (unsigned)s_cfg.accel, (unsigned)s_cfg.gain_max);
    }
}

const trackpad_cfg_t *trackpad_cfg_active(void)
{
    return &s_cfg;
}

bool trackpad_cfg_apply_and_save(const trackpad_cfg_t *c)
{
    if (!cfg_valid(c)) {
        ESP_LOGW(TAG, "reject invalid cfg base=%u accel=%u gain_max=%u",
                 (unsigned)c->base, (unsigned)c->accel, (unsigned)c->gain_max);
        return false;
    }
    s_cfg      = *c;
    s_cfg.fmt  = TRACKPAD_CFG_FMT;
    esp_err_t err = nvs_save_blob_with_total(TP_NS, TP_KEY, &s_cfg, sizeof(s_cfg),
                                              TP_TOT_KEY, 0u);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS save failed: %d", (int)err);
    }
    ESP_LOGI(TAG, "applied cfg base=%u accel=%u gain_max=%u",
             (unsigned)s_cfg.base, (unsigned)s_cfg.accel, (unsigned)s_cfg.gain_max);
    return true;
}
