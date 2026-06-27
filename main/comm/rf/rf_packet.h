#ifndef RF_PACKET_H
#define RF_PACKET_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Packet type = high nibble of byte 0; flags = low nibble. */
#define PKT_TYPE_KEY        0x1
#define PKT_TYPE_HEARTBEAT  0x2
#define PKT_TYPE_TRACKPAD   0x3
#define PKT_TYPE_HIDREPORT  0x5   /* keyboard-agnostic relay: final HID report */
#define PKT_TYPE_PAIR_ACK   0xE   /* dongle→half pairing ACK (RF-2) */
#define PKT_TYPE_PAIR_REQ   0xF   /* half→dongle pairing request (RF-2) */

/* Sub-types for PKT_TYPE_HIDREPORT (byte 1) */
#define RF_HID_SUB_KBD   0
#define RF_HID_SUB_MOUSE 1

/* Flags (low nibble of byte 0) */
#define PKT_FLAG_PRESSED    0x01   /* PKT_KEY: key is pressed (vs released) */
#define PKT_FLAG_IS_RETRY   0x02   /* application-level retransmit */

/* Matrix geometry per half (must match board.h half dimensions) */
#define RF_HALF_ROWS         5
#define RF_HALF_COLS         7
#define RF_HALF_BITMAP_BYTES 5     /* ceil(5*7 / 8) = 5 */

typedef struct {
    uint8_t row;       /* 0..4 */
    uint8_t col;       /* 0..6 (local to the half) */
    bool    pressed;
    bool    is_retry;
    uint8_t seq;
} rf_key_event_t;

typedef struct {
    uint8_t bitmap[RF_HALF_BITMAP_BYTES];  /* MSB-first, row*7+col */
    uint8_t batt_dV;   /* 0..83 = 0..8.3V, 0 = unknown */
    uint8_t link_q;    /* cumulative retries since last heartbeat */
    uint8_t seq;
} rf_heartbeat_t;

typedef struct {
    uint8_t ge0, ge1;
    uint8_t n_fingers;
    int16_t rel_x, rel_y;
    uint8_t seq;
} rf_trackpad_t;

typedef struct {
    uint16_t set_id;              /* host order; encoded big-endian on wire */
    uint8_t  dongle_wifi_mac[6];
    uint8_t  slot;               /* 0x01=left, 0x02=right */
} rf_pair_ack_t;

/* ── Encoders: write into buf, return byte count (0 on error) ── */

static inline uint16_t rf_encode_key(uint8_t *buf, const rf_key_event_t *e)
{
    if (e->row > 15 || e->col > 15) return 0;
    uint8_t flags = (e->pressed ? PKT_FLAG_PRESSED : 0) |
                    (e->is_retry ? PKT_FLAG_IS_RETRY : 0);
    buf[0] = (PKT_TYPE_KEY << 4) | (flags & 0x0F);
    buf[1] = (uint8_t)((e->row << 4) | (e->col & 0x0F));
    buf[2] = e->seq;
    return 3;
}

static inline uint16_t rf_encode_heartbeat(uint8_t *buf, const rf_heartbeat_t *h)
{
    buf[0] = (PKT_TYPE_HEARTBEAT << 4);
    memcpy(&buf[1], h->bitmap, RF_HALF_BITMAP_BYTES);
    buf[1 + RF_HALF_BITMAP_BYTES] = h->batt_dV;
    buf[2 + RF_HALF_BITMAP_BYTES] = h->link_q;
    buf[3 + RF_HALF_BITMAP_BYTES] = h->seq;
    return 4 + RF_HALF_BITMAP_BYTES;   /* 9 */
}

static inline uint16_t rf_encode_trackpad(uint8_t *buf, const rf_trackpad_t *t)
{
    buf[0] = (PKT_TYPE_TRACKPAD << 4);
    buf[1] = t->ge0; buf[2] = t->ge1; buf[3] = t->n_fingers;
    buf[4] = (uint8_t)((uint16_t)t->rel_x >> 8);
    buf[5] = (uint8_t)((uint16_t)t->rel_x & 0xFF);
    buf[6] = (uint8_t)((uint16_t)t->rel_y >> 8);
    buf[7] = (uint8_t)((uint16_t)t->rel_y & 0xFF);
    buf[8] = t->seq;
    return 9;
}

/* PKT_PAIR_REQ: 8 bytes — type 0xF, the half's 6-byte WiFi STA MAC, then the
 * half's declared slot (0x01=left / 0x02=right, board identity). slot=0 = unknown
 * (legacy 7-byte halves). */
static inline uint16_t rf_encode_pair_req(uint8_t *buf, const uint8_t mac[6], uint8_t slot)
{
    buf[0] = (PKT_TYPE_PAIR_REQ << 4);
    memcpy(buf + 1, mac, 6);
    buf[7] = slot;
    return 8;
}

/* PKT_PAIR_ACK: 10 bytes — type 0xE, set_id big-endian, dongle MAC, slot. */
static inline uint16_t rf_encode_pair_ack(uint8_t *buf, const rf_pair_ack_t *a)
{
    buf[0] = (PKT_TYPE_PAIR_ACK << 4);
    buf[1] = (uint8_t)(a->set_id >> 8);
    buf[2] = (uint8_t)(a->set_id & 0xFF);
    memcpy(buf + 3, a->dongle_wifi_mac, 6);
    buf[9] = a->slot;
    return 10;
}

/* ── Decoder: returns type (0 on error/unknown), fills the matching struct ── */

static inline uint8_t rf_packet_type(const uint8_t *buf, uint16_t len)
{
    if (len < 1) return 0;
    return (buf[0] >> 4) & 0x0F;
}

/* PKT_HIDREPORT: relay final HID reports over NRF24.
 *   kbd:   [type<<4][SUB_KBD][modifier][keycodes×6]   → 9 bytes
 *   mouse: [type<<4][SUB_MOUSE][buttons][x][y][wheel] → 6 bytes
 */
static inline uint16_t rf_encode_hidreport_kbd(uint8_t *buf, uint8_t modifier, const uint8_t kb[6])
{
    buf[0] = (PKT_TYPE_HIDREPORT << 4); buf[1] = RF_HID_SUB_KBD; buf[2] = modifier;
    memcpy(buf + 3, kb, 6); return 9;
}

static inline uint16_t rf_encode_hidreport_mouse(uint8_t *buf, uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    buf[0] = (PKT_TYPE_HIDREPORT << 4); buf[1] = RF_HID_SUB_MOUSE;
    buf[2] = buttons; buf[3] = (uint8_t)x; buf[4] = (uint8_t)y; buf[5] = (uint8_t)wheel; return 6;
}

static inline bool rf_decode_hidreport(const uint8_t *buf, uint16_t len, uint8_t *sub,
        uint8_t *mod, uint8_t kb[6], uint8_t *btn, int8_t *x, int8_t *y, int8_t *wheel)
{
    if (len < 2 || rf_packet_type(buf, len) != PKT_TYPE_HIDREPORT) return false;
    *sub = buf[1];
    if (buf[1] == RF_HID_SUB_KBD)   { if (len < 9) return false; *mod = buf[2]; memcpy(kb, buf+3, 6); return true; }
    if (buf[1] == RF_HID_SUB_MOUSE) { if (len < 6) return false; *btn = buf[2];
        *x=(int8_t)buf[3]; *y=(int8_t)buf[4]; *wheel=(int8_t)buf[5]; return true; }
    return false;
}

static inline bool rf_decode_key(const uint8_t *buf, uint16_t len, rf_key_event_t *e)
{
    if (len < 3 || rf_packet_type(buf, len) != PKT_TYPE_KEY) return false;
    uint8_t flags = buf[0] & 0x0F;
    e->pressed  = (flags & PKT_FLAG_PRESSED) != 0;
    e->is_retry = (flags & PKT_FLAG_IS_RETRY) != 0;
    e->row = (buf[1] >> 4) & 0x0F;
    e->col = buf[1] & 0x0F;
    e->seq = buf[2];
    return true;
}

static inline bool rf_decode_heartbeat(const uint8_t *buf, uint16_t len, rf_heartbeat_t *h)
{
    if (len < 9 || rf_packet_type(buf, len) != PKT_TYPE_HEARTBEAT) return false;
    memcpy(h->bitmap, &buf[1], RF_HALF_BITMAP_BYTES);
    h->batt_dV = buf[1 + RF_HALF_BITMAP_BYTES];
    h->link_q  = buf[2 + RF_HALF_BITMAP_BYTES];
    h->seq     = buf[3 + RF_HALF_BITMAP_BYTES];
    return true;
}

static inline bool rf_decode_trackpad(const uint8_t *buf, uint16_t len, rf_trackpad_t *t)
{
    if (len < 9 || rf_packet_type(buf, len) != PKT_TYPE_TRACKPAD) return false;
    t->ge0 = buf[1]; t->ge1 = buf[2]; t->n_fingers = buf[3];
    t->rel_x = (int16_t)((uint16_t)(buf[4] << 8) | buf[5]);
    t->rel_y = (int16_t)((uint16_t)(buf[6] << 8) | buf[7]);
    t->seq = buf[8];
    return true;
}

static inline bool rf_decode_pair_req(const uint8_t *buf, uint16_t len,
                                      uint8_t mac_out[6], uint8_t *slot_out)
{
    if (len < 7 || rf_packet_type(buf, len) != PKT_TYPE_PAIR_REQ) return false;
    memcpy(mac_out, buf + 1, 6);
    *slot_out = (len >= 8) ? buf[7] : 0;   /* legacy 7-byte req → slot unknown */
    return true;
}

static inline bool rf_decode_pair_ack(const uint8_t *buf, uint16_t len, rf_pair_ack_t *a)
{
    if (len < 10 || rf_packet_type(buf, len) != PKT_TYPE_PAIR_ACK) return false;
    a->set_id = ((uint16_t)buf[1] << 8) | buf[2];
    memcpy(a->dongle_wifi_mac, buf + 3, 6);
    a->slot = buf[9];
    return true;
}

/* Bitmap helpers (row*7+col bit index, MSB-first in byte) */
static inline bool rf_bitmap_get(const uint8_t *bm, uint8_t row, uint8_t col)
{
    uint8_t idx = row * RF_HALF_COLS + col;
    return (bm[idx >> 3] >> (7 - (idx & 7))) & 1;
}

static inline void rf_bitmap_set(uint8_t *bm, uint8_t row, uint8_t col, bool val)
{
    uint8_t idx = row * RF_HALF_COLS + col;
    uint8_t mask = 1 << (7 - (idx & 7));
    if (val) bm[idx >> 3] |= mask;
    else     bm[idx >> 3] &= ~mask;
}

#endif /* RF_PACKET_H */
