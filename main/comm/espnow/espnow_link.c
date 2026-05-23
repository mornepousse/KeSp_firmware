/*
 * espnow_link.c — ESP-NOW link layer.
 *
 * Skeleton:  WiFi STA init, esp_now_init, recv dispatch, send with type prepend.
 * RF-3:      Real peer registration from NVS "rf" + MAC filter in recv callback.
 *
 * WiFi channel: derived from set_id % 3 → {1,6,11} for paired sets;
 * falls back to channel 6 (2437 MHz) for unpaired/factory state.
 * This gives ~29 MHz separation from the NRF24 left channel (2476 MHz).
 */

#ifndef TEST_HOST
#include "espnow_link.h"
#include "espnow_info.h"   /* espnow_info_dispatch */
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"       /* esp_read_mac, ESP_MAC_WIFI_STA */
#include "nvs_flash.h"
#include "nvs.h"
#endif /* TEST_HOST */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Peer MAC filter — pure logic, host-testable ─────────────────
 *
 * The static array is populated once in espnow_link_init() (before
 * esp_now_register_recv_cb) and then read-only from espnow_recv_cb.
 * No mutex needed: init completes before the callback is registered.
 *
 * Dongle: up to 2 peers (mac_left, mac_right).
 * Half:   up to 1 peer  (mac_dongle).
 * Unpaired (0 peers): all incoming frames are dropped. */
#define ESPNOW_MAX_PEERS 2

static uint8_t s_peer_macs[ESPNOW_MAX_PEERS][6];
static int     s_peer_count = 0;

/* Store up to ESPNOW_MAX_PEERS MAC addresses for recv filtering.
 * Call once in espnow_link_init(), before registering the recv callback.
 * macs: array of `count` MAC addresses (6 bytes each).
 * count: number of valid MACs in `macs` (0 = no peers = drop all). */
void espnow_set_peers(const uint8_t macs[][6], int count)
{
    s_peer_count = 0;
    for (int i = 0; i < count && i < ESPNOW_MAX_PEERS; i++) {
        memcpy(s_peer_macs[i], macs[i], 6);
        s_peer_count++;
    }
}

/* Returns true if `mac` matches any stored peer MAC (exact 6-byte compare).
 * Returns false if no peers configured or MAC is not in the list.
 * Called from espnow_recv_cb — must be fast (linear scan, ≤2 entries). */
bool is_known_peer(const uint8_t mac[6])
{
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(s_peer_macs[i], mac, 6) == 0) return true;
    }
    return false;
}

#ifndef TEST_HOST

static const char *TAG = "espnow_link";

/* ── WiFi channel from set_id (spec §2.5) ───────────────────────
 * CRC-16/CCITT: poly=0x1021, init=0xFFFF, no reflect, no final XOR. */
static uint16_t crc16_ccitt_espnow(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Derive WiFi channel from dongle WiFi STA MAC (spec §2.5).
 * Reads eFuse MAC — no WiFi init required.
 * Returns 6 (unpaired default) if NVS paired_count is 0 or absent. */
static uint8_t espnow_derive_wifi_ch(void)
{
    /* Read NVS paired_count to decide whether to apply derivation. */
    uint8_t paired_count = 0;
    nvs_handle_t h;
    if (nvs_open("rf", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "paired_count", &paired_count);
        nvs_close(h);
    }
    if (paired_count == 0) {
        return 6;   /* factory default: 2437 MHz, same as prior hardcoded value */
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint16_t set_id = crc16_ccitt_espnow(mac, 6);
    /* Guard reserved sentinels (spec §2.1) */
    if (set_id == 0x0000 || set_id == 0xFFFF) set_id = 0x0001;

    /* {1,6,11}[set_id % 3] — non-overlapping 2.4 GHz WiFi channels (spec §2.5) */
    static const uint8_t ch_table[3] = {1, 6, 11};
    return ch_table[set_id % 3];
}

/* ── Recv callback — called from esp_now internal task (not ISR) ── */
/* ESP-IDF signature: data_len is int. Safe cast to uint8_t: ESP-NOW max payload
 * is 250 bytes, so data_len is always in [1..250] when non-negative. */
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int data_len)
{
    if (recv_info == NULL || data == NULL || data_len < 1 || data_len > 250) return;

    /* Drop frames from unknown senders (isolation: reject foreign KaSe sets).
     * If no peers configured (unpaired), all frames are dropped here. */
    if (!is_known_peer(recv_info->src_addr)) {
        ESP_LOGD(TAG, "ESP-NOW recv from unknown MAC %02X:%02X:%02X:%02X:%02X:%02X — dropped",
                 recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
                 recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);
        return;
    }

    espnow_info_dispatch(recv_info->src_addr, data, (uint8_t)data_len);
}

bool espnow_link_init(void)
{
    /* ── Network interface init (idempotent) ───────────────────── */
    esp_err_t e;
    e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %d", e);
        return false;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %d", e);
        return false;
    }

    /* ── WiFi STA mode (no connection — radio only for ESP-NOW) ── */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&wifi_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %d", e);
        return false;
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   /* no NVS cred persistence */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* ── WiFi channel: derived from set_id (paired) or factory default 6 (unpaired).
     * DEPRECATION NOTE: the NVS key "rf.wifi_ch" from the 2026-05-11 dongle spec §6
     * is no longer read. For paired sets, wifi_ch is derived from set_id % 3 → {1,6,11}.
     * For unpaired sets, wifi_ch defaults to 6 (2437 MHz, unchanged from prior behaviour).
     * See rf-pairing-addressing-design.md §2.5 + §6.3 for rationale.
     * IMPORTANT: esp_wifi_set_channel() MUST precede esp_now_add_peer()
     *            (peer.channel must match the current WiFi channel). */
    uint8_t wifi_ch = espnow_derive_wifi_ch();
    esp_wifi_set_channel(wifi_ch, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG, "WiFi channel: %u (2%03u MHz)", wifi_ch, 412 + (uint32_t)wifi_ch * 5);

    /* ── ESP-NOW init + recv callback ─────────────────────────── */
    e = esp_now_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %d", e);
        return false;
    }

    /* ── Load peer MACs from NVS "rf" and register with ESP-NOW ──
     * Dongle role: keys "mac_left" (6B), "mac_right" (6B).
     * Half role:   key "mac_dongle" (6B).
     * If a key is absent or all-zeros: skip that peer — no panic.
     * All-zeros MAC means "not yet paired" (key written by Plan RF-2 pairing flow). */
    {
        uint8_t tmp_macs[ESPNOW_MAX_PEERS][6];
        int     tmp_count = 0;

        nvs_handle_t nvs_peer;
        if (nvs_open("rf", NVS_READONLY, &nvs_peer) == ESP_OK) {

#if CONFIG_KASE_DEVICE_ROLE_DONGLE
            /* Dongle: load mac_left and mac_right */
            uint8_t mac_l[6] = {0}, mac_r[6] = {0};
            size_t sz = 6;
            nvs_get_blob(nvs_peer, "mac_left",  mac_l, &sz);
            sz = 6;
            nvs_get_blob(nvs_peer, "mac_right", mac_r, &sz);
            if (mac_l[0] || mac_l[1] || mac_l[2] || mac_l[3] || mac_l[4] || mac_l[5]) {
                memcpy(tmp_macs[tmp_count++], mac_l, 6);
            }
            if (mac_r[0] || mac_r[1] || mac_r[2] || mac_r[3] || mac_r[4] || mac_r[5]) {
                memcpy(tmp_macs[tmp_count++], mac_r, 6);
            }
#endif /* CONFIG_KASE_DEVICE_ROLE_DONGLE */

#if CONFIG_KASE_DEVICE_ROLE_HALF
            /* Half: load mac_dongle */
            uint8_t mac_d[6] = {0};
            size_t sz = 6;
            nvs_get_blob(nvs_peer, "mac_dongle", mac_d, &sz);
            if (mac_d[0] || mac_d[1] || mac_d[2] || mac_d[3] || mac_d[4] || mac_d[5]) {
                memcpy(tmp_macs[tmp_count++], mac_d, 6);
            }
#endif /* CONFIG_KASE_DEVICE_ROLE_HALF */

            nvs_close(nvs_peer);
        }

        /* Register with ESP-NOW — must happen after esp_now_init() and esp_wifi_set_channel() */
        esp_now_peer_info_t peer = {
            .channel = wifi_ch,   /* MUST match esp_wifi_set_channel() above */
            .ifidx   = WIFI_IF_STA,
            .encrypt = false,
        };
        for (int i = 0; i < tmp_count; i++) {
            memcpy(peer.peer_addr, tmp_macs[i], 6);
            esp_err_t pe = esp_now_add_peer(&peer);
            if (pe != ESP_OK) {
                ESP_LOGW(TAG, "esp_now_add_peer failed for peer %d: %d", i, pe);
            } else {
                ESP_LOGI(TAG, "ESP-NOW peer %d registered: %02X:%02X:%02X:%02X:%02X:%02X ch=%u",
                         i,
                         tmp_macs[i][0], tmp_macs[i][1], tmp_macs[i][2],
                         tmp_macs[i][3], tmp_macs[i][4], tmp_macs[i][5],
                         wifi_ch);
            }
        }

        /* Populate the recv-callback filter table (BEFORE registering callback) */
        espnow_set_peers((const uint8_t (*)[6])tmp_macs, tmp_count);

        if (tmp_count == 0) {
            ESP_LOGW(TAG, "no paired peers configured (NVS rf.mac_* absent) — ESP-NOW send disabled");
        }
    }

    /* Register recv callback AFTER filter table is populated */
    esp_now_register_recv_cb(espnow_recv_cb);

    ESP_LOGI(TAG, "ESP-NOW link init OK");
    return true;
}

bool espnow_send(const uint8_t mac[6], uint8_t type, const void *payload, uint16_t len)
{
    /* Build [type][payload] frame */
    uint8_t buf[1 + 250];   /* ESP-NOW max payload is 250 bytes */
    if (len > 249) {
        ESP_LOGE(TAG, "espnow_send: payload too large (%u > 249)", (unsigned)len);
        return false;
    }
    buf[0] = type;
    if (payload && len > 0) {
        memcpy(buf + 1, payload, len);
    }
    esp_err_t e = esp_now_send(mac, buf, 1 + len);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send failed: %d (peer not added?)", e);
        return false;
    }
    return true;
}

#endif /* TEST_HOST */
