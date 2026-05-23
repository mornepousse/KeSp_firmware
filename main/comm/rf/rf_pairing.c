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

/* ── Pure: positional slot assignment ──────────────────────────── */
bool rf_pairing_assign_slot(uint8_t paired_count, uint8_t *slot_out)
{
    if (paired_count == 0) { *slot_out = 0x01; return true; }
    if (paired_count == 1) { *slot_out = 0x02; return true; }
    return false;   /* window full */
}

/* ── Pure: dedup against already-stored peers ──────────────────── */
static bool mac_is_zero(const uint8_t m[6])
{
    for (int i = 0; i < 6; i++) if (m[i] != 0) return false;
    return true;
}

bool rf_pairing_match_slot(const uint8_t mac[6],
                           const uint8_t mac_left[6], const uint8_t mac_right[6],
                           uint8_t *slot_out)
{
    int i;
    if (!mac_is_zero(mac_left)) {
        for (i = 0; i < 6 && mac[i] == mac_left[i]; i++) {}
        if (i == 6) { *slot_out = 0x01; return true; }
    }
    if (!mac_is_zero(mac_right)) {
        for (i = 0; i < 6 && mac[i] == mac_right[i]; i++) {}
        if (i == 6) { *slot_out = 0x02; return true; }
    }
    return false;
}

#ifndef TEST_HOST

#include <string.h>      /* memset */
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

esp_err_t rf_pairing_save_peer_dongle(uint8_t slot, const uint8_t mac[6],
                                      uint8_t new_paired_count)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(RF_STORAGE_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    const char *key = (slot == 0x01) ? "mac_left" : "mac_right";
    e = nvs_set_blob(h, key, mac, 6);
    if (e == ESP_OK) e = nvs_set_u8(h, "paired_count", new_paired_count);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved %s, paired_count=%u (err=%d)", key, new_paired_count, e);
    return e;
}

esp_err_t rf_pairing_reset_dongle(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(RF_STORAGE_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_key(h, "mac_left");    /* ESP_ERR_NVS_NOT_FOUND is harmless */
    nvs_erase_key(h, "mac_right");
    nvs_set_u8(h, "paired_count", 0);
    e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "pairing reset (mac_left/right cleared, paired_count=0)");
    return e;
}

void rf_pairing_load_peers_dongle(uint8_t mac_left[6], uint8_t mac_right[6],
                                  uint8_t *paired_count)
{
    memset(mac_left, 0, 6);
    memset(mac_right, 0, 6);
    *paired_count = 0;
    nvs_handle_t h;
    if (nvs_open(RF_STORAGE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = 6; nvs_get_blob(h, "mac_left",  mac_left,  &sz);
    sz = 6;        nvs_get_blob(h, "mac_right", mac_right, &sz);
    nvs_get_u8(h, "paired_count", paired_count);
    nvs_close(h);
}

esp_err_t rf_pairing_save_half(uint16_t set_id, uint8_t slot, const uint8_t mac_dongle[6])
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(RF_STORAGE_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u16(h, "set_id", set_id);
    if (e == ESP_OK) e = nvs_set_u8(h, "slot", slot);
    if (e == ESP_OK) e = nvs_set_blob(h, "mac_dongle", mac_dongle, 6);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "half saved set_id=0x%04X slot=0x%02X (err=%d)", set_id, slot, e);
    return e;
}

#endif /* TEST_HOST */
