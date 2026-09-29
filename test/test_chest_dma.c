/* The chest link's DMA channel, byte for byte — Niphar_chest
 * main/link/link_proto.h and test/test_link_proto.c at commit 440d79d
 * (vectors identical to cfd7b35), lines ~567-596. The vectors below
 * (k_vec_r1, k_vec_r2, k_vec_r3, k_vec_l1, k_vec_c1) are copied VERBATIM
 * from that file, never edited: a change on either side that the other did
 * not follow breaks a fast check, not the bench. Spec:
 * docs/superpowers/specs/2026-09-29-chest-link-v3-design.md §3. */
#include "test_framework.h"
#include <string.h>
#include "../main/comm/chest/chest_dma.h"
#include "../main/security/cr_crc16.h"

/* Contract literals, pinned at compile time. */
_Static_assert(CHEST_REQ_SIZE == 8, "CHEST_REQ_SIZE must equal the chest's LINK_REQ_SIZE");
_Static_assert(CHEST_CODE_SIZE == 14, "CHEST_CODE_SIZE must equal the chest's LINK_CODE_SIZE");
_Static_assert(CHEST_REQ_LIST == 0x01, "CHEST_REQ_LIST must equal the chest's LINK_REQ_CMD_LIST");
_Static_assert(CHEST_REQ_CODE == 0x02, "CHEST_REQ_CODE must equal the chest's LINK_REQ_CMD_CODE");

/* Niphar_chest test/test_link_proto.c, commit 440d79d (vectors identical to
 * cfd7b35) — copied verbatim, never edited. */

/* R1 — LIST request from the start. */
static const uint8_t k_vec_r1[8] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5B, 0x0C,
};
/* R2 — CODE request for the account at index 5. */
static const uint8_t k_vec_r2[8] = {
    0x02, 0x05, 0x00, 0x00, 0x00, 0x00, 0x72, 0x26,
};
/* R3 — the same, argument corrupted by one bit, CRC left as R2's: refused. */
static const uint8_t k_vec_r3[8] = {
    0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x72, 0x26,
};
/* L1 — one LIST page: twelve accounts total, three in this page from index 0,
 * the "more" flag set. */
static const uint8_t k_vec_l1[34] = {
    0x0C, 0x03, 0x00, 0x01, 0x00, 0x06, 0x47, 0x49, 0x54, 0x48,
    0x55, 0x42, 0x01, 0x09, 0x4F, 0x56, 0x48, 0x3A, 0x50, 0x45,
    0x52, 0x53, 0x4F, 0x02, 0x07, 0x4F, 0x56, 0x48, 0x3A, 0x50,
    0x52, 0x4F, 0xE5, 0xD7,
};
/* C1 — one CODE answer: account 5, six digits, 418902, twelve seconds left. */
static const uint8_t k_vec_c1[14] = {
    0x05, 0x06, 0x30, 0x30, 0x34, 0x31, 0x38, 0x39, 0x30, 0x32,
    0x0C, 0x00, 0x8F, 0x9B,
};

static void test_chest_req_pack(void)
{
    uint8_t out[CHEST_REQ_SIZE];

    chest_req_pack(out, CHEST_REQ_LIST, 0);
    TEST_ASSERT_EQ(memcmp(out, k_vec_r1, CHEST_REQ_SIZE), 0, "LIST(0) matches R1");

    chest_req_pack(out, CHEST_REQ_CODE, 5);
    TEST_ASSERT_EQ(memcmp(out, k_vec_r2, CHEST_REQ_SIZE), 0, "CODE(5) matches R2");

    /* R3 carries CODE(4) with R2's stale CRC (for arg 5): a freshly packed
     * CODE(4) recomputes its own CRC and must NOT equal R3. */
    chest_req_pack(out, CHEST_REQ_CODE, 4);
    TEST_ASSERT(memcmp(out, k_vec_r3, CHEST_REQ_SIZE) != 0,
                "a freshly packed CODE(4) differs from R3's stale CRC");
}

static void test_chest_list_decode_l1(void)
{
    chest_list_t out;
    memset(&out, 0xAA, sizeof out);

    TEST_ASSERT(chest_list_decode(k_vec_l1, sizeof k_vec_l1, &out), "L1 decoded");
    TEST_ASSERT_EQ(out.total, 12, "L1 total");
    TEST_ASSERT_EQ(out.count, 3, "L1 count");
    TEST_ASSERT_EQ(out.first, 0, "L1 first");
    TEST_ASSERT(out.more, "L1 more flag");
    TEST_ASSERT_EQ(out.e[0].index, 0, "L1 entry 0 index");
    TEST_ASSERT(strcmp(out.e[0].name, "GITHUB") == 0, "L1 entry 0 name");
    TEST_ASSERT_EQ(out.e[1].index, 1, "L1 entry 1 index");
    TEST_ASSERT(strcmp(out.e[1].name, "OVH:PERSO") == 0, "L1 entry 1 name");
    TEST_ASSERT_EQ(out.e[2].index, 2, "L1 entry 2 index");
    TEST_ASSERT(strcmp(out.e[2].name, "OVH:PRO") == 0, "L1 entry 2 name");
}

static void test_chest_list_decode_l1_crc_bit_flipped(void)
{
    uint8_t b[sizeof k_vec_l1];
    chest_list_t out;
    memcpy(b, k_vec_l1, sizeof b);
    b[sizeof b - 1] ^= 0x01;   /* one bit in the CRC's high byte */

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_list_decode(b, sizeof b, &out), "one CRC bit flipped: refused");
    TEST_ASSERT_EQ(out.total, 0xAA, "out left untouched on a bad CRC");
}

static void test_chest_list_decode_l1_truncated(void)
{
    chest_list_t out;
    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_list_decode(k_vec_l1, sizeof(k_vec_l1) - 1, &out), "L1 truncated by one byte: refused");
    TEST_ASSERT_EQ(out.total, 0xAA, "out left untouched on a short buffer");
}

/* L1 with the page count patched from 3 to 4 (a fourth entry the buffer does
 * not actually carry), CRC recomputed over the mutated header: the third
 * (fabricated) entry's index+length bytes run past the CRC. */
static void test_chest_list_decode_l1_count_overruns(void)
{
    uint8_t b[sizeof k_vec_l1];
    chest_list_t out;
    memcpy(b, k_vec_l1, sizeof b);
    b[CHEST_LIST_OFF_COUNT] = 4;
    uint16_t crc = cr_crc16(b, sizeof b - 2);
    b[sizeof b - 2] = (uint8_t)(crc & 0xFF);
    b[sizeof b - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_list_decode(b, sizeof b, &out), "count 4 with only 3 entries present: entries run past the length");
    TEST_ASSERT_EQ(out.total, 0xAA, "out left untouched");
}

/* L1 with the first entry's name length patched from 6 to 35 (one over
 * CHEST_LABEL_MAX), CRC recomputed. */
static void test_chest_list_decode_l1_name_too_long(void)
{
    uint8_t b[sizeof k_vec_l1];
    chest_list_t out;
    memcpy(b, k_vec_l1, sizeof b);
    b[CHEST_LIST_HDR_SIZE + 1] = 35;   /* entry 0's length byte */
    uint16_t crc = cr_crc16(b, sizeof b - 2);
    b[sizeof b - 2] = (uint8_t)(crc & 0xFF);
    b[sizeof b - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_list_decode(b, sizeof b, &out), "a name length of 35 is refused");
    TEST_ASSERT_EQ(out.total, 0xAA, "out left untouched");
}

/* L1 with one stray byte inserted between the last entry and the CRC, CRC
 * recomputed over the padded buffer: the three entries decode cleanly, but a
 * byte is left over before the CRC. Not one of the chest's vectors: built
 * here to exercise the leftover-bytes rule on its own, since none of the
 * chest's pinned vectors carries padding. */
static void test_chest_list_decode_l1_leftover_byte(void)
{
    uint8_t b[sizeof(k_vec_l1) + 1];
    chest_list_t out;
    memcpy(b, k_vec_l1, sizeof k_vec_l1 - 2);   /* header + 3 entries, no CRC */
    b[sizeof k_vec_l1 - 2] = 0x00;              /* one stray byte */
    uint16_t crc = cr_crc16(b, sizeof k_vec_l1 - 1);
    b[sizeof k_vec_l1 - 1] = (uint8_t)(crc & 0xFF);
    b[sizeof k_vec_l1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_list_decode(b, sizeof b, &out),
                "a byte left over between the last entry and the CRC is refused");
    TEST_ASSERT_EQ(out.total, 0xAA, "out left untouched");
}

/* count above CHEST_LIST_MAX_ENTRIES: refused before any entry is walked. */
static void test_chest_list_decode_count_over_max_entries(void)
{
    uint8_t b[CHEST_LIST_HDR_SIZE + 2];
    chest_list_t out;
    memset(b, 0x00, sizeof b);
    b[CHEST_LIST_OFF_TOTAL] = CHEST_LIST_MAX_ENTRIES + 1;
    b[CHEST_LIST_OFF_COUNT] = CHEST_LIST_MAX_ENTRIES + 1;
    uint16_t crc = cr_crc16(b, sizeof b - 2);
    b[sizeof b - 2] = (uint8_t)(crc & 0xFF);
    b[sizeof b - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_list_decode(b, sizeof b, &out), "count over CHEST_LIST_MAX_ENTRIES refused");
    TEST_ASSERT_EQ(out.total, 0xAA, "out left untouched");
}

static void test_chest_list_decode_unsafe_inputs(void)
{
    chest_list_t out;
    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_list_decode(NULL, sizeof k_vec_l1, &out), "NULL buf refused");
    TEST_ASSERT(!chest_list_decode(k_vec_l1, sizeof k_vec_l1, NULL), "NULL out refused");
    TEST_ASSERT(!chest_list_decode(k_vec_l1, 0, &out), "len 0 refused");
    TEST_ASSERT(!chest_list_decode(k_vec_l1, 1, &out), "len 1 (below the header+CRC floor) refused");
    TEST_ASSERT(!chest_list_decode(k_vec_l1, CHEST_DMA_MAX + 1, &out), "len over CHEST_DMA_MAX refused");
    TEST_ASSERT_EQ(out.total, 0xAA, "out left untouched throughout");
}

static void test_chest_code_decode_c1(void)
{
    chest_code_t out;
    memset(&out, 0xAA, sizeof out);

    TEST_ASSERT(chest_code_decode(k_vec_c1, sizeof k_vec_c1, &out), "C1 decoded");
    TEST_ASSERT_EQ(out.index, 5, "C1 index");
    TEST_ASSERT_EQ(out.digits, 6, "C1 digits");
    TEST_ASSERT(strcmp(out.code, "418902") == 0, "C1 code: the LAST 6 characters of the zero-padded field");
    TEST_ASSERT_EQ(out.seconds, 12, "C1 seconds left");
}

/* C1 with digits patched from 6 to 7, CRC recomputed: refused (only 6 or 8
 * are valid digit counts). */
static void test_chest_code_decode_c1_digits_7(void)
{
    uint8_t b[sizeof k_vec_c1];
    chest_code_t out;
    memcpy(b, k_vec_c1, sizeof b);
    b[1] = 7;
    uint16_t crc = cr_crc16(b, CHEST_CODE_SIZE - 2);
    b[CHEST_CODE_SIZE - 2] = (uint8_t)(crc & 0xFF);
    b[CHEST_CODE_SIZE - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_code_decode(b, sizeof b, &out), "digits 7 refused");
    TEST_ASSERT_EQ(out.index, 0xAA, "out left untouched");
}

/* C1 with one code byte replaced by a letter, CRC recomputed. */
static void test_chest_code_decode_c1_letter_in_code(void)
{
    uint8_t b[sizeof k_vec_c1];
    chest_code_t out;
    memcpy(b, k_vec_c1, sizeof b);
    b[2] = 'A';
    uint16_t crc = cr_crc16(b, CHEST_CODE_SIZE - 2);
    b[CHEST_CODE_SIZE - 2] = (uint8_t)(crc & 0xFF);
    b[CHEST_CODE_SIZE - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_code_decode(b, sizeof b, &out), "a letter in the code bytes is refused");
    TEST_ASSERT_EQ(out.index, 0xAA, "out left untouched");
}

/* C1 with seconds patched to 31 (one over the chest's own maximum: TOTP_STEP
 * == 30, sec_time_window_remaining() returns 1..30), CRC recomputed. */
static void test_chest_code_decode_c1_seconds_over_30(void)
{
    uint8_t b[sizeof k_vec_c1];
    chest_code_t out;
    memcpy(b, k_vec_c1, sizeof b);
    b[10] = 31;
    uint16_t crc = cr_crc16(b, CHEST_CODE_SIZE - 2);
    b[CHEST_CODE_SIZE - 2] = (uint8_t)(crc & 0xFF);
    b[CHEST_CODE_SIZE - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_code_decode(b, sizeof b, &out), "seconds 31 refused (impossible for a real chest)");
    TEST_ASSERT_EQ(out.index, 0xAA, "out left untouched");
}

static void test_chest_code_decode_wrong_length(void)
{
    chest_code_t out;
    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_code_decode(k_vec_c1, 13, &out), "a 13-byte buffer is refused");
    TEST_ASSERT_EQ(out.index, 0xAA, "out left untouched");
}

static void test_chest_code_decode_unsafe_inputs(void)
{
    chest_code_t out;
    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_code_decode(NULL, sizeof k_vec_c1, &out), "NULL buf refused");
    TEST_ASSERT(!chest_code_decode(k_vec_c1, sizeof k_vec_c1, NULL), "NULL out refused");
    TEST_ASSERT(!chest_code_decode(k_vec_c1, 0, &out), "len 0 refused");
    TEST_ASSERT(!chest_code_decode(k_vec_c1, CHEST_DMA_MAX + 1, &out), "len way over CHEST_CODE_SIZE refused");
    TEST_ASSERT_EQ(out.index, 0xAA, "out left untouched throughout");
}

void test_chest_dma(void)
{
    TEST_SUITE("chest link DMA channel (request, LIST, CODE)");
    TEST_RUN(test_chest_req_pack);
    TEST_RUN(test_chest_list_decode_l1);
    TEST_RUN(test_chest_list_decode_l1_crc_bit_flipped);
    TEST_RUN(test_chest_list_decode_l1_truncated);
    TEST_RUN(test_chest_list_decode_l1_count_overruns);
    TEST_RUN(test_chest_list_decode_l1_name_too_long);
    TEST_RUN(test_chest_list_decode_l1_leftover_byte);
    TEST_RUN(test_chest_list_decode_count_over_max_entries);
    TEST_RUN(test_chest_list_decode_unsafe_inputs);
    TEST_RUN(test_chest_code_decode_c1);
    TEST_RUN(test_chest_code_decode_c1_digits_7);
    TEST_RUN(test_chest_code_decode_c1_letter_in_code);
    TEST_RUN(test_chest_code_decode_c1_seconds_over_30);
    TEST_RUN(test_chest_code_decode_wrong_length);
    TEST_RUN(test_chest_code_decode_unsafe_inputs);
}
