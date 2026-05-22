/*
 * espnow_link.c — ESP-NOW link layer skeleton.
 *
 * Skeleton:  WiFi STA init, esp_now_init, recv dispatch, send with type prepend.
 * Stub:      Peer MAC loading from NVS (zeros → skip add + log warning).
 *
 * WiFi channel: 6 (2437 MHz) by default. This gives ~29 MHz separation from the
 * NRF24 left channel (2476 MHz). Strategy A from spec §8.1: tolerate occasional
 * NRF packet loss (heartbeat reconciliation recovers state). Bench measurement of
 * rf.link_q before/after ESP-NOW activation is required to validate the approach.
 */

#include "espnow_link.h"
#include "espnow_info.h"   /* espnow_info_dispatch */
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "espnow_link";

/* ── Recv callback — called from esp_now internal task (not ISR) ── */
/* ESP-IDF signature: data_len is int. Safe cast to uint8_t: ESP-NOW max payload
 * is 250 bytes, so data_len is always in [1..250] when non-negative. */
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int data_len)
{
    if (recv_info == NULL || data == NULL || data_len < 1 || data_len > 250) return;
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

    /* ── WiFi channel from NVS rf.wifi_ch (default 6) ─────────── */
    uint8_t wifi_ch = 6;
    nvs_handle_t nvs_h;
    if (nvs_open("rf", NVS_READONLY, &nvs_h) == ESP_OK) {
        uint8_t ch_nvs = 0;
        if (nvs_get_u8(nvs_h, "wifi_ch", &ch_nvs) == ESP_OK && ch_nvs >= 1 && ch_nvs <= 13) {
            wifi_ch = ch_nvs;
        }
        nvs_close(nvs_h);
    }
    esp_wifi_set_channel(wifi_ch, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG, "WiFi channel: %u (2%03u MHz)", wifi_ch, 412 + wifi_ch * 5);

    /* ── ESP-NOW init + recv callback ─────────────────────────── */
    e = esp_now_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %d", e);
        return false;
    }
    esp_now_register_recv_cb(espnow_recv_cb);

    /* ── Peer add from NVS (stub: MACs are zeros until pairing) ── */
    /* NVS namespace "rf" keys mac_left, mac_right (dongle) / mac_dongle (half).
     * Until Plan 4 (pairing), these keys are absent → MACs are zero → skip add. */
    /* TODO STUB: load peer MACs from NVS namespace "rf":
     *   Dongle: keys "mac_left" (6 bytes), "mac_right" (6 bytes)
     *   Half:   key "mac_dongle" (6 bytes)
     * Then:
     *   esp_now_peer_info_t peer = {
     *     .channel = wifi_ch,
     *     .ifidx   = WIFI_IF_STA,
     *     .encrypt = false,
     *   };
     *   memcpy(peer.peer_addr, mac, 6);
     *   esp_now_add_peer(&peer);
     * For now: log warning and continue (send/recv still works for broadcast tests). */
    ESP_LOGW(TAG, "no peer MACs configured (NVS rf.mac_* not set) — ESP-NOW TX disabled");

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
