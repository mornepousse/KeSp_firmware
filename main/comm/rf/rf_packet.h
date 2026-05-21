#ifndef RF_PACKET_H
#define RF_PACKET_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Packet type = high nibble of byte 0; flags = low nibble. */
#define PKT_TYPE_KEY        0x1
#define PKT_TYPE_HEARTBEAT  0x2
#define PKT_TYPE_TRACKPAD   0x3

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
    int8_t  dx, dy;
    uint8_t buttons;   /* 3 bits */
    int8_t  scroll_v, scroll_h;
    uint8_t seq;
} rf_trackpad_t;

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
    buf[1] = (uint8_t)t->dx;
    buf[2] = (uint8_t)t->dy;
    buf[3] = (uint8_t)((t->buttons & 0x07) << 5);  /* buttons in top 3 bits */
    buf[4] = (uint8_t)t->scroll_v;
    buf[5] = (uint8_t)t->scroll_h;
    buf[6] = t->seq;
    return 7;
}

/* ── Decoder: returns type (0 on error/unknown), fills the matching struct ── */

static inline uint8_t rf_packet_type(const uint8_t *buf, uint16_t len)
{
    if (len < 1) return 0;
    return (buf[0] >> 4) & 0x0F;
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
    if (len < 7 || rf_packet_type(buf, len) != PKT_TYPE_TRACKPAD) return false;
    t->dx = (int8_t)buf[1];
    t->dy = (int8_t)buf[2];
    t->buttons  = (buf[3] >> 5) & 0x07;
    t->scroll_v = (int8_t)buf[4];
    t->scroll_h = (int8_t)buf[5];
    t->seq = buf[6];
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
