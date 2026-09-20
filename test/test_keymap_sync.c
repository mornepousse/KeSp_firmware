/* Reassembly of the keymap received in chunks (drip via ACK payload) — pure.
 *
 * The pull is driven by the left half: it NEVER requests anything but the next missing chunk,
 * and the dongle serves it. The order is therefore guaranteed by construction, and the
 * reassembler can be sequential: it only accepts the expected chunk and ignores duplicates /
 * out-of-sequence ones. Useful consequence: a replayed ACK payload, or a late frame, never
 * corrupts the keymap being received.
 *
 * Design: docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md
 */
#include "test_framework.h"
#include "../main/comm/rf/keymap_sync.h"
#include <string.h>

static void remplir(uint8_t *d, uint8_t seed)
{
    for (int i = 0; i < SYNC_CHUNK_BYTES; i++) d[i] = (uint8_t)(seed + i);
}

static void test_sequence_complete(void)
{
    keymap_rx_t s;
    keymap_rx_reset(&s);
    TEST_ASSERT_EQ(keymap_rx_next(&s), 0, "starts at chunk 0");
    TEST_ASSERT(!keymap_rx_complete(&s), "not complete at the start");
    for (int k = 0; k < SYNC_N_CHUNKS; k++) {
        uint8_t d[SYNC_CHUNK_BYTES]; remplir(d, (uint8_t)k);
        TEST_ASSERT(keymap_rx_chunk(&s, (uint8_t)k, d), "in-sequence chunk accepted");
        TEST_ASSERT_EQ(keymap_rx_next(&s), (uint8_t)(k + 1), "next advances");
    }
    TEST_ASSERT(keymap_rx_complete(&s), "complete after 40 chunks");
    /* The reassembled blob is exact, each chunk in its place. */
    for (int k = 0; k < SYNC_N_CHUNKS; k++) {
        uint8_t d[SYNC_CHUNK_BYTES]; remplir(d, (uint8_t)k);
        TEST_ASSERT(memcmp(&s.buf[k * SYNC_CHUNK_BYTES], d, SYNC_CHUNK_BYTES) == 0,
                    "chunk content in the right place");
    }
    /* Once complete, one more chunk is refused (no write outside the buffer). */
    uint8_t extra[SYNC_CHUNK_BYTES]; remplir(extra, 99);
    TEST_ASSERT(!keymap_rx_chunk(&s, SYNC_N_CHUNKS, extra), "beyond 40: refused");
    TEST_ASSERT_EQ(keymap_rx_next(&s), SYNC_N_CHUNKS, "next stays at 40");
}

static void test_doublon_et_hors_sequence_ignores(void)
{
    keymap_rx_t s;
    keymap_rx_reset(&s);
    uint8_t d0[SYNC_CHUNK_BYTES]; remplir(d0, 0);
    TEST_ASSERT(keymap_rx_chunk(&s, 0, d0), "chunk 0 accepted");
    /* Duplicate of 0 (replayed ACK): ignored, next unchanged. */
    TEST_ASSERT(!keymap_rx_chunk(&s, 0, d0), "duplicate ignored");
    TEST_ASSERT_EQ(keymap_rx_next(&s), 1, "next stays 1 after duplicate");
    /* Jump to 5 (out of sequence): ignored, and the content was not written. */
    uint8_t d5[SYNC_CHUNK_BYTES]; remplir(d5, 5);
    TEST_ASSERT(!keymap_rx_chunk(&s, 5, d5), "out of sequence ignored");
    TEST_ASSERT_EQ(keymap_rx_next(&s), 1, "next stays 1 after out-of-sequence");
    TEST_ASSERT(memcmp(&s.buf[5 * SYNC_CHUNK_BYTES], d5, SYNC_CHUNK_BYTES) != 0,
                "an out-of-sequence chunk writes nothing into the buffer");
}

/* A reset starts over from zero even after a partial pull — this is what the
 * left half does when a new BEACON announces a DIFFERENT fingerprint along
 * the way (the dongle's keymap has changed again): we start over cleanly. */
static void test_reset_repart_de_zero(void)
{
    keymap_rx_t s;
    keymap_rx_reset(&s);
    uint8_t d[SYNC_CHUNK_BYTES]; remplir(d, 1);
    keymap_rx_chunk(&s, 0, d);
    keymap_rx_chunk(&s, 1, d);
    TEST_ASSERT_EQ(keymap_rx_next(&s), 2, "two chunks received");
    keymap_rx_reset(&s);
    TEST_ASSERT_EQ(keymap_rx_next(&s), 0, "reset: back to chunk 0");
    TEST_ASSERT(!keymap_rx_complete(&s), "reset: no longer complete");
}

void test_keymap_sync(void)
{
    TEST_SUITE("Keymap reassembler (sequential pull)");
    test_sequence_complete();
    test_doublon_et_hors_sequence_ignores();
    test_reset_repart_de_zero();
}
