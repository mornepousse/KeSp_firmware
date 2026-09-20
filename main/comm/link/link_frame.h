/* Frame of the Niphargus inter-half link (TRRS, UART1).
 *
 * Pure logic, entirely inline: no UART, no FreeRTOS. The transport layer
 * calls the encoders to send, and link_decode() on what it
 * has received.
 *
 * -- Format -------------------------------------------------------------------
 *   [0]        SOF 0x4E ('N')
 *   [1]        payload length (type + seq [+ bitmap])
 *   [2]        type
 *   [3]        seq
 *   [4..]      5-byte bitmap, for MATRIX only
 *   [last]     CRC-8 over bytes [1] to the second-to-last
 *
 * -- Why a decoder that returns what it consumed -----------------------------
 *
 * The link is exposed at the connector: it gets hot-unplugged, it takes ESD,
 * and the first byte received readily falls in the middle of a frame. A
 * decoder that only answers "valid / not valid" does not tell the caller how
 * much to advance, and a noisy stream then never resynchronizes — which is
 * exactly what a length-prefixed format is supposed to offer.
 *
 * Hence the three-outcome contract:
 *   NEED_MORE  nothing consumed, call again with more bytes;
 *   FRAME      a valid frame is in *out, `consumed` bytes consumed;
 *   SKIP       `consumed` bytes to discard (noise, wrong CRC, inconsistent frame).
 *
 * A SKIP always consumes at least one byte: without this guarantee, a
 * resynchronization loop would spin forever on the same byte.
 *
 * -- Backward compatibility ---------------------------------------------------
 *
 * A well-framed frame whose type is unknown is consumed WHOLE rather than
 * resynchronized byte by byte. A newer half can therefore talk to an
 * older one without garbling its stream.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "rf_packet.h"             /* RF_HALF_BITMAP_BYTES — same bitmap as over RF */
#include "cdc_binary_protocol.h"   /* ks_crc8 */

#define LINK_SOF          0x4Eu

#define LINK_TYPE_MATRIX  0x01u    /* half-matrix state */
#define LINK_TYPE_PROBE   0x02u    /* "are you really my other half?" (5 V handshake) */
#define LINK_TYPE_ACK     0x03u    /* reply to a probe */

/* Payload = type + seq, plus the bitmap for MATRIX. */
#define LINK_PAYLOAD_CTRL    2
#define LINK_PAYLOAD_MATRIX  (2 + RF_HALF_BITMAP_BYTES)
#define LINK_PAYLOAD_MAX     LINK_PAYLOAD_MATRIX

/* SOF + length + payload + CRC */
#define LINK_FRAME_MAX  (2 + LINK_PAYLOAD_MAX + 1)
#define LINK_FRAME_MIN  (2 + LINK_PAYLOAD_CTRL + 1)

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t bitmap[RF_HALF_BITMAP_BYTES];   /* filled for MATRIX, zero otherwise */
} link_frame_t;

typedef enum {
    LINK_DECODE_NEED_MORE = 0,   /* nothing consumed, call again with more bytes */
    LINK_DECODE_FRAME,           /* valid frame in *out */
    LINK_DECODE_SKIP,            /* bytes to discard */
} link_decode_status_t;

/* The link reuses ks_crc8() from the binary CDC protocol rather than
 * reimplementing a CRC-8: a single implementation to review in the repo.
 * link_crc8() stays a separate function to preserve the module's interface and
 * document that it really is the repo's CRC being used here. */
static inline uint8_t link_crc8(const uint8_t *data, uint16_t len)
{
    return ks_crc8(data, len);
}

/* -- Encoders: write into buf, return the number of bytes (0 on error) ------ */

static inline uint16_t link_encode_matrix(uint8_t *buf,
                                          const uint8_t bitmap[RF_HALF_BITMAP_BYTES],
                                          uint8_t seq)
{
    if (buf == NULL || bitmap == NULL) return 0;
    buf[0] = LINK_SOF;
    buf[1] = LINK_PAYLOAD_MATRIX;
    buf[2] = LINK_TYPE_MATRIX;
    buf[3] = seq;
    memcpy(&buf[4], bitmap, RF_HALF_BITMAP_BYTES);
    buf[LINK_FRAME_MAX - 1] = link_crc8(&buf[1], 1 + LINK_PAYLOAD_MATRIX);
    return LINK_FRAME_MAX;
}

/* Control frames without payload: PROBE and ACK. */
static inline uint16_t link_encode_ctrl(uint8_t *buf, uint8_t type, uint8_t seq)
{
    if (buf == NULL) return 0;
    buf[0] = LINK_SOF;
    buf[1] = LINK_PAYLOAD_CTRL;
    buf[2] = type;
    buf[3] = seq;
    buf[LINK_FRAME_MIN - 1] = link_crc8(&buf[1], 1 + LINK_PAYLOAD_CTRL);
    return LINK_FRAME_MIN;
}

/* -- Resynchronizing decoder -------------------------------------------------- */

static inline link_decode_status_t link_decode(const uint8_t *buf, uint16_t len,
                                               link_frame_t *out, uint16_t *consumed)
{
    if (consumed != NULL) *consumed = 0;
    if (buf == NULL || out == NULL || consumed == NULL) return LINK_DECODE_NEED_MORE;

    if (len == 0) return LINK_DECODE_NEED_MORE;

    /* Not a frame start: discard THIS byte only. Discarding more would
     * risk swallowing the real SOF that may follow immediately. */
    if (buf[0] != LINK_SOF) { *consumed = 1; return LINK_DECODE_SKIP; }

    if (len < 2) return LINK_DECODE_NEED_MORE;      /* the length is still missing */

    uint8_t payload_len = buf[1];
    if (payload_len < LINK_PAYLOAD_CTRL || payload_len > LINK_PAYLOAD_MAX) {
        /* Impossible length: this 0x4E was noise, not a SOF. */
        *consumed = 1;
        return LINK_DECODE_SKIP;
    }

    uint16_t total = (uint16_t)(2 + payload_len + 1);
    if (len < total) return LINK_DECODE_NEED_MORE;  /* incomplete frame */

    if (link_crc8(&buf[1], (uint16_t)(1 + payload_len)) != buf[total - 1]) {
        /* Wrong CRC: the announced framing cannot be trusted — a real SOF may
         * be hiding inside. Restart at the next byte. */
        *consumed = 1;
        return LINK_DECODE_SKIP;
    }

    /* From here the framing is authenticated: whatever is decided about the
     * content, the whole frame is consumed. */
    *consumed = total;

    uint8_t type = buf[2];
    if (type == LINK_TYPE_MATRIX && payload_len != LINK_PAYLOAD_MATRIX)
        return LINK_DECODE_SKIP;    /* MATRIX without its bitmap: inconsistent */

    out->type = type;
    out->seq  = buf[3];
    if (type == LINK_TYPE_MATRIX)
        memcpy(out->bitmap, &buf[4], RF_HALF_BITMAP_BYTES);
    else
        memset(out->bitmap, 0, RF_HALF_BITMAP_BYTES);

    return LINK_DECODE_FRAME;
}
