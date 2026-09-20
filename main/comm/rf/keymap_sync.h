#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "rf_packet.h"   /* SYNC_CHUNK_BYTES, SYNC_N_CHUNKS */

/* Reassembly of the keymap received in chunks — auto sync dongle->left over ACK
 * payload (fusion, phase 3). LEFT side.
 *
 * The pull is driven by the left: it only ever requests the NEXT missing
 * chunk (rf_sync_req_t.next), and the dongle serves it in the ACK. Order
 * is therefore guaranteed by construction, and the reassembler can stay
 * sequential: it only accepts the expected chunk and ignores duplicates /
 * out-of-sequence ones. A replayed ACK payload or a late frame never corrupts the keymap in progress.
 *
 * No bitmap of received chunks: a simple counter suffices, and it is also
 * the "next chunk wanted" that the left announces to the dongle. When it
 * reaches SYNC_N_CHUNKS, the buffer holds exactly KEYMAP_BLOB_BYTES bytes,
 * ready for save_keymaps; the fingerprint announced on the next STATUS is its acknowledgment.
 *
 * Pure, tested on host (test/test_keymap_sync.c). Design:
 * docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md */
typedef struct {
    uint8_t buf[SYNC_N_CHUNKS * SYNC_CHUNK_BYTES];   /* 1120 B = KEYMAP_BLOB_BYTES */
    uint8_t next;                                    /* next expected chunk, 0..40 */
} keymap_rx_t;

static inline void keymap_rx_reset(keymap_rx_t *s) { s->next = 0; }

/* A chunk arrives. Accepted (copied, next advances) ONLY if it is the one
 * expected and there is room left; otherwise ignored without writing
 * anything. Returns true if accepted. */
static inline bool keymap_rx_chunk(keymap_rx_t *s, uint8_t idx, const uint8_t *data)
{
    if (s->next >= SYNC_N_CHUNKS || idx != s->next) return false;
    memcpy(&s->buf[(size_t)idx * SYNC_CHUNK_BYTES], data, SYNC_CHUNK_BYTES);
    s->next++;
    return true;
}

/* Next chunk wanted — what the left puts into rf_sync_req_t.next. */
static inline uint8_t keymap_rx_next(const keymap_rx_t *s) { return s->next; }

/* All chunks are here: buf is a complete keymap ready to save. */
static inline bool keymap_rx_complete(const keymap_rx_t *s) { return s->next >= SYNC_N_CHUNKS; }
