#pragma once

/*
 * espnow_msg.h — ESP-NOW info-channel message types and payload structs.
 *
 * Wire format:
 *   [type:u8][payload bytes...]
 *   Total ESP-NOW payload = 1 + sizeof(payload struct).
 *   espnow_send() in espnow_link.c prepends the type byte.
 *
 * Type ID ranges:
 *   0x00-0x0F  Info channel (this file — layer, battery, state).
 *   0x10-0x1F  OTA (Plan 5, dongle spec §8 — reserved, not implemented).
 *   0x20-0x2F  Config push (Plan 5 — reserved).
 *   0x30-0x3F  Verbose telemetry (Plan 5 — reserved).
 *
 * All structs are packed. Endianness: all fields ≤ 1 byte (no multi-byte
 * integers in the current info-channel payloads — no endianness issue).
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* ── Info-channel type IDs ────────────────────────────────────── */
#define EN_INFO_BATTERY   0x01   /* half → dongle: battery level (en_battery_t) */
#define EN_INFO_LAYER     0x02   /* dongle → half: current active layer (en_layer_t) */
#define EN_INFO_STATE     0x03   /* dongle → half: modifier + flag state (en_state_t) */

/* ── Reserved type ID ranges (Plan 5 — not implemented) ─────── */
/* EN_OTA_BEGIN = 0x10, EN_OTA_CHUNK = 0x11, EN_OTA_END = 0x12, EN_OTA_ACK = 0x13 */
/* EN_CFG_PUSH = 0x20, EN_CFG_ACK = 0x21                                           */
/* EN_TELEM_REQ = 0x30, EN_TELEM_RSP = 0x31                                        */

/* ── EN_INFO_BATTERY (half → dongle) — 3 bytes ──────────────── */
typedef struct __attribute__((packed)) {
    uint8_t batt_dV;    /* Battery voltage × 10. 0=unknown. Range: 0..83 (0..8.3V). */
    uint8_t soc_pct;    /* State of charge 0..100. 0=unknown (battery brick not done). */
    uint8_t charging;   /* 0=not charging, 1=charging. Source: BMS GPIO46 (stub). */
} en_battery_t;

/* ── EN_INFO_LAYER (dongle → half) — 17 bytes ───────────────── */
typedef struct __attribute__((packed)) {
    uint8_t layer_idx;  /* Active layer index 0..N-1. */
    char    name[16];   /* Layer name string, zero-padded. Not null-terminated if 16 chars. */
} en_layer_t;

/* ── EN_INFO_STATE (dongle → half) — 2 bytes ────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t modifiers;  /* HID modifier byte (same as USB HID Report byte 0). */
    uint8_t flags;      /* bit0=caps_word, bit1=bt_connected, bit2=usb_active, bits3-7=rsvd */
} en_state_t;

/* ── Encode helpers: write [type][payload] into buf, return total bytes written. ── */
/* buf must be at least 1 + sizeof(payload). Returns 0 on error. */

static inline uint8_t en_encode_battery(uint8_t *buf, const en_battery_t *b)
{
    buf[0] = EN_INFO_BATTERY;
    memcpy(buf + 1, b, sizeof(*b));
    return 1 + sizeof(*b);
}

static inline uint8_t en_encode_layer(uint8_t *buf, const en_layer_t *l)
{
    buf[0] = EN_INFO_LAYER;
    memcpy(buf + 1, l, sizeof(*l));
    return 1 + sizeof(*l);
}

static inline uint8_t en_encode_state(uint8_t *buf, const en_state_t *s)
{
    buf[0] = EN_INFO_STATE;
    memcpy(buf + 1, s, sizeof(*s));
    return 1 + sizeof(*s);
}

/* ── Decode helpers: parse from [type][payload] buffer.
 * Returns true if type matches and buf has enough bytes. ────── */

static inline bool en_decode_battery(const uint8_t *buf, uint8_t len, en_battery_t *out)
{
    if (len < 1 + sizeof(*out) || buf[0] != EN_INFO_BATTERY) return false;
    memcpy(out, buf + 1, sizeof(*out));
    return true;
}

static inline bool en_decode_layer(const uint8_t *buf, uint8_t len, en_layer_t *out)
{
    if (len < 1 + sizeof(*out) || buf[0] != EN_INFO_LAYER) return false;
    memcpy(out, buf + 1, sizeof(*out));
    return true;
}

static inline bool en_decode_state(const uint8_t *buf, uint8_t len, en_state_t *out)
{
    if (len < 1 + sizeof(*out) || buf[0] != EN_INFO_STATE) return false;
    memcpy(out, buf + 1, sizeof(*out));
    return true;
}
