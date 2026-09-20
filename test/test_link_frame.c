/* Frame of the inter-half link (TRRS / UART1).
 *
 * The link is exposed at the connector: it gets hot-unplugged, it takes
 * ESD hits, and the next byte can arrive in the middle of a frame. The decoder
 * must therefore reject cleanly, never read out of bounds, and above all
 * **say how much to advance by** — without which a noisy stream never resyncs.
 *
 * It carries three types: the matrix, and the two PROBE / ACK control frames
 * that the 5 V handshake needs (link_handshake.h).
 */
#include "test_framework.h"
#include "../main/comm/link/link_frame.h"

static const uint8_t BM[RF_HALF_BITMAP_BYTES] = {0xA5, 0x00, 0x3C, 0xFF, 0x01};

/* ── Round trip ──────────────────────────────────────────────────── */

static void test_roundtrip_matrix(void)
{
    uint8_t buf[LINK_FRAME_MAX];
    uint16_t n = link_encode_matrix(buf, BM, 42);
    TEST_ASSERT_EQ(n, LINK_FRAME_MAX, "the matrix frame is the max size");

    link_frame_t f; uint16_t used = 0;
    TEST_ASSERT_EQ(link_decode(buf, n, &f, &used), LINK_DECODE_FRAME, "decoded");
    TEST_ASSERT_EQ(used, n, "all bytes consumed");
    TEST_ASSERT_EQ(f.type, LINK_TYPE_MATRIX, "type preserved");
    TEST_ASSERT_EQ(f.seq, 42, "seq preserved");
    for (int i = 0; i < RF_HALF_BITMAP_BYTES; i++)
        TEST_ASSERT_EQ(f.bitmap[i], BM[i], "bitmap preserved byte by byte");
}

static void test_roundtrip_control_frames(void)
{
    uint8_t buf[LINK_FRAME_MAX];
    link_frame_t f; uint16_t used = 0;

    uint16_t n = link_encode_ctrl(buf, LINK_TYPE_PROBE, 7);
    TEST_ASSERT_EQ(n, LINK_FRAME_MIN, "a control frame is short");
    TEST_ASSERT_EQ(link_decode(buf, n, &f, &used), LINK_DECODE_FRAME, "PROBE decoded");
    TEST_ASSERT_EQ(used, n, "exact consumption of a short frame");
    TEST_ASSERT_EQ(f.type, LINK_TYPE_PROBE, "PROBE type");
    TEST_ASSERT_EQ(f.seq, 7, "PROBE seq");

    n = link_encode_ctrl(buf, LINK_TYPE_ACK, 8);
    TEST_ASSERT_EQ(link_decode(buf, n, &f, &used), LINK_DECODE_FRAME, "ACK decoded");
    TEST_ASSERT_EQ(f.type, LINK_TYPE_ACK, "ACK type");
}

/* ── Resynchronization: the heart of the contract ─────────────────── */

static void test_incomplete_input_asks_for_more(void)
{
    uint8_t buf[LINK_FRAME_MAX];
    uint16_t n = link_encode_matrix(buf, BM, 3);
    link_frame_t f; uint16_t used = 99;

    /* Any truncation asks for more bytes, without ever consuming anything:
     * consuming a partial frame would lose it for good. */
    for (uint16_t cut = 0; cut < n; cut++) {
        used = 99;
        TEST_ASSERT_EQ(link_decode(buf, cut, &f, &used), LINK_DECODE_NEED_MORE,
                       "truncated frame -> needs more");
        TEST_ASSERT_EQ(used, 0, "nothing is consumed on an incomplete frame");
    }
    TEST_ASSERT_EQ(link_decode(buf, n, &f, &used), LINK_DECODE_FRAME, "complete -> decoded");
}

static void test_garbage_byte_is_skipped_one_at_a_time(void)
{
    /* A byte that is not a frame start is discarded alone — no more, otherwise
     * we might swallow the real SOF that immediately follows it. */
    uint8_t buf[4] = {0x00, 0x11, 0x22, 0x33};
    link_frame_t f; uint16_t used = 0;
    TEST_ASSERT_EQ(link_decode(buf, sizeof(buf), &f, &used), LINK_DECODE_SKIP,
                   "stray byte -> to discard");
    TEST_ASSERT_EQ(used, 1, "only one byte discarded at a time");
}

static void test_resyncs_onto_a_frame_buried_in_noise(void)
{
    /* The real case: the jack is replugged mid-typing and the buffer starts
     * with noise. By advancing by what the decoder says it consumed, we
     * must eventually land on the frame. */
    uint8_t stream[3 + LINK_FRAME_MAX];
    stream[0] = 0x00; stream[1] = 0xFF; stream[2] = 0x4E;  /* including a fake SOF */
    link_encode_matrix(&stream[3], BM, 55);

    uint16_t off = 0;
    link_frame_t f; uint16_t used = 0;
    link_decode_status_t st = LINK_DECODE_SKIP;
    int guard = 0;
    while (off < sizeof(stream) && guard++ < 32) {
        st = link_decode(&stream[off], (uint16_t)(sizeof(stream) - off), &f, &used);
        if (st == LINK_DECODE_FRAME) break;
        TEST_ASSERT(used > 0 || st == LINK_DECODE_NEED_MORE,
                    "a SKIP always consumes at least one byte — otherwise infinite loop");
        off += used;
    }
    TEST_ASSERT_EQ(st, LINK_DECODE_FRAME, "the buried frame is eventually found");
    TEST_ASSERT_EQ(f.seq, 55, "and it's the right one");
}

/* ── Rejections ──────────────────────────────────────────────────── */

static void test_bad_crc_is_skipped_not_trusted(void)
{
    uint8_t buf[LINK_FRAME_MAX];
    uint16_t n = link_encode_matrix(buf, BM, 7);
    buf[n - 1] ^= 0xFF;                       /* corrupted CRC */
    link_frame_t f; uint16_t used = 0;
    TEST_ASSERT_EQ(link_decode(buf, n, &f, &used), LINK_DECODE_SKIP, "wrong CRC -> discarded");
    TEST_ASSERT_EQ(used, 1, "we resume at the next byte, the real SOF might be inside");

    n = link_encode_matrix(buf, BM, 7);
    buf[4] ^= 0x01;                           /* corrupted payload */
    TEST_ASSERT_EQ(link_decode(buf, n, &f, &used), LINK_DECODE_SKIP, "corrupted payload -> discarded");
}

static void test_absurd_length_is_skipped(void)
{
    uint8_t buf[LINK_FRAME_MAX];
    uint16_t n = link_encode_matrix(buf, BM, 1);
    link_frame_t f; uint16_t used = 0;

    buf[1] = 0xFF;                            /* impossible announced length */
    TEST_ASSERT_EQ(link_decode(buf, n, &f, &used), LINK_DECODE_SKIP, "absurd length -> discarded");
    TEST_ASSERT_EQ(used, 1, "this SOF was noise, we advance by one byte");

    n = link_encode_matrix(buf, BM, 1);
    buf[1] = 0;                               /* too short to carry a type */
    TEST_ASSERT_EQ(link_decode(buf, n, &f, &used), LINK_DECODE_SKIP, "zero length -> discarded");
}

/* ── Backward compatibility ────────────────────────────────────────── */

static void test_unknown_type_is_consumed_whole(void)
{
    /* A newer half will send types this one does not know. They must be
     * swallowed cleanly, not resynchronized byte by byte: that is the whole
     * point of a length-prefixed format. */
    uint8_t buf[LINK_FRAME_MAX];
    uint16_t n = link_encode_ctrl(buf, LINK_TYPE_ACK, 9);
    buf[2] = 0x7E;                            /* unknown type */
    buf[n - 1] = link_crc8(&buf[1], (uint16_t)(1 + buf[1]));   /* recomputed CRC */

    link_frame_t f; uint16_t used = 0;
    link_decode_status_t st = link_decode(buf, n, &f, &used);
    TEST_ASSERT(st == LINK_DECODE_FRAME || st == LINK_DECODE_SKIP,
                "an unknown but well-formed type does not block the stream");
    TEST_ASSERT_EQ(used, n, "it is consumed WHOLE, not byte by byte");
}

static void test_matrix_type_with_wrong_length_is_rejected(void)
{
    /* MATRIX type but control-frame length: well-framed, but
     * inconsistent. We consume the announced frame and discard it. */
    uint8_t buf[LINK_FRAME_MAX];
    uint16_t n = link_encode_ctrl(buf, LINK_TYPE_MATRIX, 4);
    buf[n - 1] = link_crc8(&buf[1], (uint16_t)(1 + buf[1]));

    link_frame_t f; uint16_t used = 0;
    TEST_ASSERT_EQ(link_decode(buf, n, &f, &used), LINK_DECODE_SKIP,
                   "MATRIX without its bitmap -> inconsistent, discarded");
    TEST_ASSERT_EQ(used, n, "but consumed whole, the framing was correct");
}

/* ── Arguments ───────────────────────────────────────────────────── */

static void test_null_arguments(void)
{
    uint8_t buf[LINK_FRAME_MAX];
    link_frame_t f; uint16_t used = 0;
    TEST_ASSERT_EQ(link_encode_matrix(NULL, BM, 0), 0, "NULL buf -> 0");
    TEST_ASSERT_EQ(link_encode_matrix(buf, NULL, 0), 0, "NULL bitmap -> 0");
    TEST_ASSERT_EQ(link_encode_ctrl(NULL, LINK_TYPE_PROBE, 0), 0, "NULL ctrl buf -> 0");
    TEST_ASSERT_EQ(link_decode(NULL, 8, &f, &used), LINK_DECODE_NEED_MORE, "NULL buf");
    TEST_ASSERT_EQ(link_decode(buf, 8, NULL, &used), LINK_DECODE_NEED_MORE, "NULL output");
    TEST_ASSERT_EQ(link_decode(buf, 8, &f, NULL), LINK_DECODE_NEED_MORE, "NULL counter");
}

/* The link's CRC is the CDC protocol's, not a reimplementation. */
static void test_crc_is_the_repo_one(void)
{
    const uint8_t v1[] = {0x01, 0x02, 0x03};
    const uint8_t v2[] = {0xFF, 0x00, 0xA5, 0x5A};
    TEST_ASSERT_EQ(link_crc8(v1, sizeof(v1)), ks_crc8(v1, sizeof(v1)), "link_crc8 == ks_crc8");
    TEST_ASSERT_EQ(link_crc8(v2, sizeof(v2)), ks_crc8(v2, sizeof(v2)), "same on another vector");
}

void test_link_frame(void)
{
    printf("\n-- inter-half link frame --\n");
    test_roundtrip_matrix();
    test_roundtrip_control_frames();
    test_incomplete_input_asks_for_more();
    test_garbage_byte_is_skipped_one_at_a_time();
    test_resyncs_onto_a_frame_buried_in_noise();
    test_bad_crc_is_skipped_not_trusted();
    test_absurd_length_is_skipped();
    test_unknown_type_is_consumed_whole();
    test_matrix_type_with_wrong_length_is_rejected();
    test_null_arguments();
    test_crc_is_the_repo_one();
}
