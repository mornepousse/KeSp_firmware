/*
 * rf_pairing.c — Per-set NRF24 derivation + pairing NVS/eFuse helpers.
 * See rf_pairing.h and spec §2, §4, §5.
 *
 * Layout: pure derivations are OUTSIDE #ifndef TEST_HOST (host-testable).
 * eFuse/NVS/log helpers are INSIDE the guard (firmware only).
 */

#include "rf_pairing.h"

/* ── Pure: CRC-16/CCITT-FALSE ──────────────────────────────────── */
uint16_t crc16_ccitt(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
            else              crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ── Pure: NRF address + channel derivation ────────────────────── */
bool rf_derive_addr(uint16_t set_id, uint8_t slot,
                    uint8_t addr_out[4], uint8_t *suffix_out,
                    uint8_t *channel_out)
{
    if (set_id == 0x0000 || set_id == 0xFFFF) {
        return false;   /* unpaired sentinel — caller keeps factory cfg */
    }
    addr_out[0] = 'K';   /* 0x4B */
    addr_out[1] = 'S';   /* 0x53 */
    addr_out[2] = (uint8_t)(set_id >> 8);
    addr_out[3] = (uint8_t)(set_id & 0xFF);
    *suffix_out = slot;
    uint8_t base_ch = (uint8_t)(80 + 2 * (set_id % 20));   /* 80..118 even */
    *channel_out = (slot == 0x01) ? base_ch : (uint8_t)(base_ch + 1);
    return true;
}

/* ── Pure: WiFi channel derivation (Plan 3 consumer) ───────────── */
uint8_t rf_derive_wifi_ch(uint16_t set_id)
{
    if (set_id == 0x0000 || set_id == 0xFFFF) {
        return 6;   /* factory default ESP-NOW channel (espnow_link.c) */
    }
    static const uint8_t wifi_chans[3] = { 1, 6, 11 };
    return wifi_chans[set_id % 3];
}

#ifndef TEST_HOST

#include "esp_mac.h"     /* esp_read_mac, ESP_MAC_WIFI_STA */
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "rf_pair";

uint16_t rf_compute_set_id(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   /* eFuse read; no WiFi init required */
    uint16_t id = crc16_ccitt(mac, 6);
    if (id == 0x0000 || id == 0xFFFF) {
        id = 0x0001;   /* spec §2.1 guard: never use the reserved sentinels */
    }
    return id;
}

void rf_apply_set_id(rf_radio_cfg_t *cfg, uint16_t set_id, uint8_t slot)
{
    uint8_t addr[4];
    uint8_t suffix;
    uint8_t channel;
    if (!rf_derive_addr(set_id, slot, addr, &suffix, &channel)) {
        return;   /* sentinel → leave cfg at factory defaults */
    }
    cfg->rx_addr[0] = addr[0];
    cfg->rx_addr[1] = addr[1];
    cfg->rx_addr[2] = addr[2];
    cfg->rx_addr[3] = addr[3];
    cfg->addr_suffix = suffix;
    cfg->channel = channel;
}

uint16_t rf_pairing_load_set_id_dongle(void)
{
    nvs_handle_t h;
    if (nvs_open(RF_STORAGE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no rf NVS namespace — factory defaults");
        return 0;
    }
    uint8_t paired_count = 0;
    esp_err_t e = nvs_get_u8(h, "paired_count", &paired_count);
    nvs_close(h);
    if (e != ESP_OK || paired_count == 0) {
        ESP_LOGI(TAG, "paired_count=%u — factory defaults", (unsigned)paired_count);
        return 0;
    }
    uint16_t id = rf_compute_set_id();
    ESP_LOGI(TAG, "paired_count=%u set_id=0x%04X", (unsigned)paired_count, id);
    return id;
}

uint16_t rf_pairing_load_set_id_half(uint8_t fallback_slot, uint8_t *slot_out)
{
    *slot_out = fallback_slot;   /* default to the board factory suffix */

    nvs_handle_t h;
    if (nvs_open(RF_STORAGE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no rf NVS namespace — factory defaults");
        return 0;
    }
    uint16_t set_id = 0;
    esp_err_t e = nvs_get_u16(h, "set_id", &set_id);
    if (e != ESP_OK || set_id == 0) {
        nvs_close(h);
        ESP_LOGI(TAG, "no set_id in NVS — factory defaults");
        return 0;
    }
    if (set_id == 0xFFFF) set_id = 0x0001;   /* guard, same as dongle */

    uint8_t slot = 0;
    if (nvs_get_u8(h, "slot", &slot) == ESP_OK && (slot == 0x01 || slot == 0x02)) {
        *slot_out = slot;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "set_id=0x%04X slot=0x%02X", set_id, *slot_out);
    return set_id;
}

#endif /* TEST_HOST */
