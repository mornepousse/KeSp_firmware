#pragma once

/*
 * espnow_link.h — ESP-NOW link layer API.
 *
 * Handles WiFi STA init (radio only, no IP), esp_now_init, peer management,
 * send (with type byte prepend), and recv dispatch to espnow_info_dispatch().
 *
 * Call espnow_info_init() before espnow_link_init() to ensure g_half_state_mutex
 * exists before the recv callback can fire.
 *
 * MAC sources:
 *   - Dongle: loads mac_left / mac_right from NVS namespace "rf" (stub: zeros → skip add).
 *   - Half: loads mac_dongle from NVS namespace "rf" (stub: zeros → battery TX skipped).
 *   - All MACs zero by default until pairing (Plan 4) is implemented.
 */

#include <stdint.h>
#include <stdbool.h>

/* Initialize ESP-NOW link:
 *   - esp_netif_init() + esp_event_loop_create_default() (idempotent)
 *   - WiFi STA mode (no connection, radio only)
 *   - Channel from NVS rf.wifi_ch (default 6 = 2437 MHz)
 *   - esp_now_init()
 *   - Register recv callback → espnow_info_dispatch()
 *   - Add peers from NVS (zeros → log warning, skip add)
 * Returns true on success. */
bool espnow_link_init(void);

/* Send an ESP-NOW message.
 *   mac:     6-byte target peer MAC
 *   type:    message type ID (from espnow_msg.h)
 *   payload: message body (may be NULL if len == 0)
 *   len:     payload length in bytes (uint16_t per spec; ESP-NOW max is 250 bytes)
 * The function prepends [type] before payload in the ESP-NOW frame.
 * Returns true if esp_now_send() accepted the frame (fire-and-forget; no ACK). */
bool espnow_send(const uint8_t mac[6], uint8_t type, const void *payload, uint16_t len);
