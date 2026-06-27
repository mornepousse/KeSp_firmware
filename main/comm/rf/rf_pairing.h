#ifndef RF_PAIRING_H
#define RF_PAIRING_H

/*
 * rf_pairing.h — Per-set NRF24 address/channel derivation (Plan RF-1) and
 * pairing NVS/eFuse helpers (firmware-only).
 *
 * Pure derivations (host-testable, no ESP-IDF deps):
 *   crc16_ccitt, rf_derive_addr, rf_derive_wifi_ch.
 * Firmware-only helpers (NVS + eFuse, guarded by !TEST_HOST):
 *   rf_compute_set_id, rf_apply_set_id, rf_pairing_load_set_id_dongle/half.
 *
 * set_id derivation: CRC-16/CCITT-FALSE of the dongle WiFi STA MAC (eFuse).
 * Unpaired sentinel: set_id 0 or 0xFFFF → derivations are no-ops; the caller
 * keeps the board factory cfg unchanged (reproduces KaSe.01/.02, ch 76/82).
 *
 * See docs/superpowers/specs/2026-05-22-rf-pairing-addressing-design.md §2.
 */

#include <stdint.h>
#include <stdbool.h>

/* ── Pure derivations — host-safe, no ESP-IDF includes ─────────── */

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflect, no final XOR. */
uint16_t crc16_ccitt(const uint8_t *data, uint32_t len);

/* Derive the 5-byte NRF address (4-byte base + suffix) and channel for a
 * given set_id + slot (0x01=left, 0x02=right).
 *   addr_out[4]   ← {'K','S', set_id>>8, set_id&0xFF}
 *   *suffix_out   ← slot
 *   *channel_out  ← (slot == 0x01) ? base_ch : base_ch + 1
 *                   where base_ch = 80 + 2*(set_id % 20)
 * Returns false (and writes nothing) for the unpaired sentinel
 * (set_id == 0 || set_id == 0xFFFF). Returns true otherwise. */
bool rf_derive_addr(uint16_t set_id, uint8_t slot,
                    uint8_t addr_out[4], uint8_t *suffix_out,
                    uint8_t *channel_out);

/* Derive the ESP-NOW WiFi channel from set_id: {1,6,11}[set_id % 3].
 * For the unpaired sentinel (set_id 0 / 0xFFFF) returns 6 (factory default,
 * matching espnow_link.c). Plan 3 consumes this. */
uint8_t rf_derive_wifi_ch(uint16_t set_id);

/* ── Pairing rendezvous (fixed, immutable — spec §3.2) ─────────── */
#define RF_PAIR_CHANNEL  0x28                          /* 40 dec, 2440 MHz */
#define RF_PAIR_ADDR     { 'K', 'S', 'P', 'R', 0xFF }  /* 5 bytes */

/* Device type advertised in the v2 pairing request (rf_encode_pair_req2).
 * Legacy 8-byte halves (rf_encode_pair_req) are treated as DUMB_HALF. */
#define RF_DEV_DUMB_HALF  0   /* sends raw matrix; HID engine runs on dongle */
#define RF_DEV_SMART_KBD  1   /* sends final HID reports over NRF */

/* Positional slot assignment (spec §3.4). paired_count 0→0x01, 1→0x02.
 * Returns false (window full) when paired_count >= 2. Pure. */
bool rf_pairing_assign_slot(uint8_t paired_count, uint8_t *slot_out);

/* Resolve the slot for a pairing half: if the half declared a valid slot
 * (0x01/0x02), honor it (order-independent — no L/R swap). Otherwise fall back
 * to positional assignment by paired_count. Returns false if positional and
 * the window is full. Pure. */
bool rf_pairing_resolve_slot(uint8_t declared_slot, uint8_t paired_count, uint8_t *slot_out);

/* Dedup: if `mac` equals an already-stored peer, return its slot. All-zero
 * stored MACs are treated as empty (no match). Returns false if no match. Pure. */
bool rf_pairing_match_slot(const uint8_t mac[6],
                           const uint8_t mac_left[6], const uint8_t mac_right[6],
                           uint8_t *slot_out);

#ifndef TEST_HOST

#include "rf_driver.h"   /* rf_radio_cfg_t — firmware only */

/* NVS namespace for pairing data (spec §4.1). Distinct from STORAGE_NAMESPACE. */
#define RF_STORAGE_NAMESPACE "rf"

/* Compute set_id from the dongle's own WiFi STA MAC (eFuse, no WiFi init).
 * Applies the 0/0xFFFF → 0x0001 guard (spec §2.1). */
uint16_t rf_compute_set_id(void);

/* Apply per-set address+channel to a cfg in place. No-op for the sentinel
 * (set_id 0 / 0xFFFF) → cfg keeps its factory defaults. */
void rf_apply_set_id(rf_radio_cfg_t *cfg, uint16_t set_id, uint8_t slot);

/* Dongle: read NVS rf.paired_count. 0/absent → return 0 (factory).
 * Else → rf_compute_set_id(). */
uint16_t rf_pairing_load_set_id_dongle(void);

/* Half: read NVS rf.set_id (u16) + rf.slot (u8). Absent/0 → return 0 and set
 * *slot_out = fallback_slot (the board factory suffix). 0xFFFF → 0x0001. */
uint16_t rf_pairing_load_set_id_half(uint8_t fallback_slot, uint8_t *slot_out);

/* Dongle: persist a paired half's MAC into the slot's NVS key and bump
 * paired_count. slot 0x01 → "mac_left", 0x02 → "mac_right". Returns ESP_OK. */
esp_err_t rf_pairing_save_peer_dongle(uint8_t slot, const uint8_t mac[6],
                                      uint8_t new_paired_count);

/* Dongle: clear mac_left/mac_right/paired_count (re-pair reset). */
esp_err_t rf_pairing_reset_dongle(void);

/* Dongle: load mac_left + mac_right (6B each; zeroed if absent) and paired_count. */
void rf_pairing_load_peers_dongle(uint8_t mac_left[6], uint8_t mac_right[6],
                                  uint8_t *paired_count);

/* Half: persist set_id + slot + dongle MAC after receiving PKT_PAIR_ACK. */
esp_err_t rf_pairing_save_half(uint16_t set_id, uint8_t slot, const uint8_t mac_dongle[6]);

#endif /* TEST_HOST */

#endif /* RF_PAIRING_H */
