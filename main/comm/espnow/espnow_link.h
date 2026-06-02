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
 * MAC sources (RF-3):
 *   - Dongle: loads mac_left / mac_right from NVS namespace "rf".
 *   - Half:   loads mac_dongle from NVS namespace "rf".
 *   - All-zero MACs (absent keys) are skipped gracefully (no peers added).
 *
 * WiFi channel: derived from set_id % 3 → {1,6,11} for paired sets;
 * defaults to 6 (2437 MHz) for unpaired/factory state.
 * "rf.wifi_ch" NVS key is no longer read (deprecated — spec §2.5/§6.3).
 */

#include <stdint.h>
#include <stdbool.h>

/* ── Peer filter API (also used by host tests) ─────────────────── */

/* Populate the static peer-MAC filter table.
 * Must be called before esp_now_register_recv_cb().
 * macs: array of count MAC addresses. count: 0..ESPNOW_MAX_PEERS.
 * All-zero MACs are silently skipped by the caller (espnow_link_init). */
void espnow_set_peers(const uint8_t macs[][6], int count);

/* Returns true if mac matches any registered peer (exact 6-byte compare).
 * Returns false if no peers configured or MAC not in list.
 * Called from espnow_recv_cb — safe for concurrent read after init. */
bool is_known_peer(const uint8_t mac[6]);

/* ── Link API ───────────────────────────────────────────────────── */

/* Initialize ESP-NOW link:
 *   - esp_netif_init() + esp_event_loop_create_default() (idempotent)
 *   - WiFi STA mode (no connection, radio only)
 *   - Channel derived from set_id (paired) or 6 (unpaired)
 *   - esp_now_init()
 *   - Load peer MACs from NVS "rf", register peers, populate filter table
 *   - Register recv callback (after filter table is populated)
 * Returns true on success. */
bool espnow_link_init(void);

/* (Re)derive the WiFi channel and (re)register all peer MACs from NVS "rf".
 * Idempotent; requires esp_now_init() to have run. Call after a pairing completes
 * so a freshly paired half is reachable WITHOUT a dongle reboot (also re-applies
 * the channel if set_id changed on the first pairing). */
void espnow_reload_peers(void);

/* Re-initialize ESP-NOW after a WiFi stop/start cycle (e.g. light-sleep wake).
 * Calls esp_now_init(), espnow_reload_peers(), and esp_now_register_recv_cb()
 * in one step. Requires esp_wifi_start() to have completed before this call.
 * Returns true on success. */
bool espnow_link_restart_espnow(void);

/* Send an ESP-NOW message.
 *   mac:     6-byte target peer MAC
 *   type:    message type ID (from espnow_msg.h)
 *   payload: message body (may be NULL if len == 0)
 *   len:     payload length in bytes (uint16_t per spec; ESP-NOW max is 250 bytes)
 * The function prepends [type] before payload in the ESP-NOW frame.
 * Returns true if esp_now_send() accepted the frame (fire-and-forget; no ACK). */
bool espnow_send(const uint8_t mac[6], uint8_t type, const void *payload, uint16_t len);
