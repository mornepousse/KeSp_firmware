/* Frames for the dongle→left auto keymap sync via ACK payload (fusion, phase 3).
 *
 * Three frames: BEACON (dongle→left in the ACK: "I have a keymap with
 * fingerprint fp_target in n_chunks"), CHUNK (dongle→left in the ACK: a 28-byte
 * piece), REQ (left→dongle uplink: "the next chunk I want"). All fit within an
 * nRF24 ACK payload (≤ 32 bytes).
 *
 * Design: docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md
 */
#include "test_framework.h"
#include "../main/comm/rf/rf_packet.h"
#include <string.h>

static void test_beacon_roundtrip(void)
{
    rf_sync_beacon_t in = { .fp_target = 0xDEADBEEFu, .n_chunks = SYNC_N_CHUNKS };
    uint8_t buf[8];
    uint16_t n = rf_encode_sync_beacon(buf, &in);
    TEST_ASSERT_EQ(n, 6, "beacon = 6 bytes");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_SYNC_BEACON, "type BEACON");
    rf_sync_beacon_t out = {0};
    TEST_ASSERT(rf_decode_sync_beacon(buf, n, &out), "decode beacon");
    TEST_ASSERT_EQ(out.fp_target, 0xDEADBEEFu, "fp_target round-trip");
    TEST_ASSERT_EQ(out.n_chunks, SYNC_N_CHUNKS, "n_chunks round-trip");
    /* wrong type and short frame rejected */
    uint8_t bad[6]; memcpy(bad, buf, 6); bad[0] = (PKT_TYPE_STATUS << 4);
    TEST_ASSERT(!rf_decode_sync_beacon(bad, 6, &out), "rejects other type");
    TEST_ASSERT(!rf_decode_sync_beacon(buf, 5, &out), "rejects too short");
}

static void test_chunk_roundtrip(void)
{
    rf_sync_chunk_t in = { .idx = 39 };
    for (int i = 0; i < SYNC_CHUNK_BYTES; i++) in.data[i] = (uint8_t)(i * 3 + 1);
    uint8_t buf[32];
    uint16_t n = rf_encode_sync_chunk(buf, &in);
    TEST_ASSERT_EQ(n, 30, "chunk = 30 bytes (fits in an ACK payload)");
    TEST_ASSERT(n <= 32, "≤ 32 bytes, the nRF24 limit");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_SYNC_CHUNK, "type CHUNK");
    rf_sync_chunk_t out = {0};
    TEST_ASSERT(rf_decode_sync_chunk(buf, n, &out), "decode chunk");
    TEST_ASSERT_EQ(out.idx, 39, "idx round-trip");
    TEST_ASSERT(memcmp(out.data, in.data, SYNC_CHUNK_BYTES) == 0, "data round-trip");
    TEST_ASSERT(!rf_decode_sync_chunk(buf, 29, &out), "rejects too short");
}

static void test_req_roundtrip(void)
{
    rf_sync_req_t in = { .next = 7 };
    uint8_t buf[4];
    uint16_t n = rf_encode_sync_req(buf, &in);
    TEST_ASSERT_EQ(n, 2, "req = 2 bytes");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_SYNC_REQ, "type REQ");
    rf_sync_req_t out = {0};
    TEST_ASSERT(rf_decode_sync_req(buf, n, &out), "decode req");
    TEST_ASSERT_EQ(out.next, 7, "next round-trip");
    TEST_ASSERT(!rf_decode_sync_req(buf, 1, &out), "rejects too short");
}

/* The drip geometry: 40 chunks of 28 bytes cover EXACTLY the keymap blob
 * (1120 bytes), no partial chunk — otherwise the reassembler would have to handle a tail. */
static void test_geometrie_des_chunks(void)
{
    TEST_ASSERT_EQ(SYNC_N_CHUNKS * SYNC_CHUNK_BYTES, 1120, "40 × 28 = 1120 = keymap");
}

void test_keymap_sync_frames(void)
{
    TEST_SUITE("Keymap sync frames (BEACON/CHUNK/REQ)");
    test_beacon_roundtrip();
    test_chunk_roundtrip();
    test_req_roundtrip();
    test_geometrie_des_chunks();
}
