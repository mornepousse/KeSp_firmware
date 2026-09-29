/* The chest link's DMA channel, byte for byte — Niphar_chest
 * main/link/link_proto.h and test/test_link_proto.c at commit 440d79d
 * (vectors identical to cfd7b35), lines ~567-596. The vectors below
 * (k_vec_r1, k_vec_r2, k_vec_r3, k_vec_l1, k_vec_c1) are copied VERBATIM
 * from that file, never edited: a change on either side that the other did
 * not follow breaks a fast check, not the bench. Spec:
 * docs/superpowers/specs/2026-09-29-chest-link-v3-design.md §3.
 *
 * This file also builds ITS OWN LIST/CODE buffers (functions below, not
 * copied from the chest) to exercise bounds the chest's own pinned vectors
 * do not happen to touch: 32/33-entry pages, an oversized name WITH its
 * bytes actually present, an oversized page at exactly CHEST_DMA_MAX + 1
 * built as a real well-formed list (not a truncated buffer), sanitize
 * edges, the "more" flag on its own bit, and CODE's padding/seconds/digit
 * rules. Every one of these is compiled into BOTH test_runner and the
 * sanitized `test_chest_sanitized` binary (test/CMakeLists.txt): the
 * ordinary build catches most mutations by a changed return value, and the
 * ASan+UBSan binary catches the ones an ordinary build cannot tell apart
 * from an accidental "still refused" (see test_chest_list_decode_32_ok /
 * _33_refused below, and the CMakeLists.txt comment on why). */
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
 * CHEST_LABEL_MAX), CRC recomputed. The 35 bytes are NOT actually present
 * (buffer size unchanged): this exercises the same path as the count-overrun
 * test above (caught by the "name runs past the CRC" bound too). See
 * test_chest_list_decode_namelen_35_present below for a buffer that carries
 * the 35 bytes for real. */
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

/* Review item 2 (M3): a page whose 35-byte name is REALLY present in the
 * buffer, so `name_off + namelen <= end` holds and the ONLY thing that can
 * refuse it is the `namelen > CHEST_LABEL_MAX` check itself. Without the
 * check, the sanitize loop writes name[0..34] (fills the 35-byte array
 * exactly) and the terminator write `name[35] = '\0'` lands one byte past
 * chest_list_entry_t.name — out of bounds, and this is the buffer that
 * proves it: total 1, count 1, first 0, flags 0, one entry (index 0, name
 * length 35, 35 'A' bytes), CRC. Not one of the chest's vectors: the chest
 * never emits a name this long (LINK_LABEL_MAX == 34), built here to pin
 * the boundary on its own. */
static void test_chest_list_decode_namelen_35_present(void)
{
    uint8_t b[4 + 2 + 35 + 2];
    chest_list_t out;
    b[CHEST_LIST_OFF_TOTAL] = 1;
    b[CHEST_LIST_OFF_COUNT] = 1;
    b[CHEST_LIST_OFF_FIRST] = 0;
    b[CHEST_LIST_OFF_FLAGS] = 0;
    b[4] = 0;    /* entry index */
    b[5] = 35;   /* entry name length: one over CHEST_LABEL_MAX */
    memset(&b[6], 'A', 35);
    uint16_t crc = cr_crc16(b, sizeof b - 2);
    b[sizeof b - 2] = (uint8_t)(crc & 0xFF);
    b[sizeof b - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_list_decode(b, sizeof b, &out), "a name length of 35, bytes actually present, is refused");
    TEST_ASSERT_EQ(out.total, 0xAA, "out left untouched");
}

/* Review item 2 (M4): the legitimate boundary, one byte under the refused
 * one above — a 34-character name must be ACCEPTED, not refused by a
 * weakened `namelen >= CHEST_LABEL_MAX`. */
static void test_chest_list_decode_namelen_34_ok(void)
{
    uint8_t b[4 + 2 + 34 + 2];
    chest_list_t out;
    b[CHEST_LIST_OFF_TOTAL] = 1;
    b[CHEST_LIST_OFF_COUNT] = 1;
    b[CHEST_LIST_OFF_FIRST] = 0;
    b[CHEST_LIST_OFF_FLAGS] = 0;
    b[4] = 0;
    b[5] = 34;
    memset(&b[6], 'B', 34);
    uint16_t crc = cr_crc16(b, sizeof b - 2);
    b[sizeof b - 2] = (uint8_t)(crc & 0xFF);
    b[sizeof b - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(chest_list_decode(b, sizeof b, &out), "a name length of exactly CHEST_LABEL_MAX (34) is accepted");
    TEST_ASSERT_EQ(strlen(out.e[0].name), 34, "the decoded name is 34 characters");
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

/* Review item 1: a REAL, well-formed 32-entry page (CHEST_LIST_MAX_ENTRIES,
 * the legitimate maximum) — must be accepted, not refused by an off-by-one
 * bound. Entries carry empty names (namelen 0) and index == their position,
 * so the last one pins e[31].index == 31. */
static void test_chest_list_decode_32_entries_ok(void)
{
    uint8_t b[4 + 32 * 2 + 2];
    chest_list_t out;
    b[CHEST_LIST_OFF_TOTAL] = 32;
    b[CHEST_LIST_OFF_COUNT] = 32;
    b[CHEST_LIST_OFF_FIRST] = 0;
    b[CHEST_LIST_OFF_FLAGS] = 0;
    uint16_t o = 4;
    for (uint16_t i = 0; i < 32; i++) {
        b[o++] = (uint8_t)i;
        b[o++] = 0;   /* empty name */
    }
    uint16_t crc = cr_crc16(b, o);
    b[o] = (uint8_t)(crc & 0xFF);
    b[o + 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(chest_list_decode(b, sizeof b, &out), "a real 32-entry page (the legitimate max) is accepted");
    TEST_ASSERT_EQ(out.count, 32, "count 32");
    TEST_ASSERT_EQ(out.e[31].index, 31, "the 32nd entry decoded, index 31");
}

/* Review item 1: a REAL, well-formed 33-entry page (one over
 * CHEST_LIST_MAX_ENTRIES) — must be refused, `out` untouched. Reviewer's
 * note: at -O0, removing the `count > CHEST_LIST_MAX_ENTRIES` guard makes
 * THIS test's own verdict unreliable — the resulting stack-buffer-overflow
 * write into chest_list_t.e[32] (array bound 32) clobbers unrelated locals
 * and can make the function return false "by accident" rather than by the
 * guard, so a plain build cannot be trusted to catch its removal. The
 * authoritative proof for this one guard is the sanitized binary
 * (test/CMakeLists.txt: test_chest_sanitized, -fsanitize=address,undefined):
 * with the guard removed it aborts on the overflow. This test still pins the
 * INTENDED behavior for the ordinary build (refused, out untouched) and
 * catches every OTHER way this could break. */
static void test_chest_list_decode_33_entries_refused(void)
{
    uint8_t b[4 + 33 * 2 + 2];
    chest_list_t out;
    b[CHEST_LIST_OFF_TOTAL] = 33;
    b[CHEST_LIST_OFF_COUNT] = 33;
    b[CHEST_LIST_OFF_FIRST] = 0;
    b[CHEST_LIST_OFF_FLAGS] = 0;
    uint16_t o = 4;
    for (uint16_t i = 0; i < 33; i++) {
        b[o++] = (uint8_t)i;
        b[o++] = 0;
    }
    uint16_t crc = cr_crc16(b, o);
    b[o] = (uint8_t)(crc & 0xFF);
    b[o + 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_list_decode(b, sizeof b, &out), "a real 33-entry page (one over the max) is refused");
    TEST_ASSERT_EQ(out.total, 0xAA, "out left untouched");
}

/* Review item 4: CHEST_DMA_MAX + 1 (513) as a REAL, well-formed list — 14
 * entries of a 34-character name plus one entry of a 1-character name (4
 * header + 14*36 + 1*3 + 2 CRC == 513) — so that if the `len > CHEST_DMA_MAX`
 * guard fell, decoding would proceed on genuine, in-bounds content instead of
 * reading past a short array: a deterministic false verdict either way, never
 * an out-of-bounds read the guard happens to mask. Old version of this test
 * passed CHEST_DMA_MAX + 1 with the 34-byte k_vec_l1 array, which is what the
 * length guard exists to prevent reading past in the first place — replaced
 * because it could never prove the guard's removal is unsafe, only that
 * len > CHEST_DMA_MAX is short-circuited before any buffer access (true, but
 * not the point). */
static void test_chest_list_decode_513_bytes_refused(void)
{
    uint8_t b[513];
    chest_list_t out;
    b[CHEST_LIST_OFF_TOTAL] = 15;
    b[CHEST_LIST_OFF_COUNT] = 15;
    b[CHEST_LIST_OFF_FIRST] = 0;
    b[CHEST_LIST_OFF_FLAGS] = 0;
    uint16_t o = 4;
    for (uint8_t i = 0; i < 14; i++) {
        b[o++] = i;
        b[o++] = 34;
        memset(&b[o], (char)('A' + i), 34);
        o = (uint16_t)(o + 34);
    }
    b[o++] = 14;
    b[o++] = 1;
    b[o++] = 'Z';
    TEST_ASSERT_EQ(o, 511, "511 bytes of header+entries before the CRC");
    uint16_t crc = cr_crc16(b, o);
    b[o] = (uint8_t)(crc & 0xFF);
    b[o + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ((uint16_t)(o + 2), 513, "513 bytes total: CHEST_DMA_MAX + 1");

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_list_decode(b, sizeof b, &out),
                "a real, well-formed 513-byte page is refused for its length alone");
    TEST_ASSERT_EQ(out.total, 0xAA, "out left untouched");
}

/* Review item 6: the sanitize boundary bytes, all in one name — 0x1F (just
 * below the kept range), 0x20 (space, the bottom of the kept range), 0x7E
 * ('~', the top), 0x7F (DEL, just above), 0x80 (high bit). */
static void test_chest_list_decode_sanitize_bounds(void)
{
    uint8_t b[4 + 2 + 5 + 2];
    chest_list_t out;
    const uint8_t raw[5] = { 0x1F, 0x20, 0x7E, 0x7F, 0x80 };
    b[CHEST_LIST_OFF_TOTAL] = 1;
    b[CHEST_LIST_OFF_COUNT] = 1;
    b[CHEST_LIST_OFF_FIRST] = 0;
    b[CHEST_LIST_OFF_FLAGS] = 0;
    b[4] = 0;
    b[5] = 5;
    memcpy(&b[6], raw, 5);
    uint16_t crc = cr_crc16(b, sizeof b - 2);
    b[sizeof b - 2] = (uint8_t)(crc & 0xFF);
    b[sizeof b - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(chest_list_decode(b, sizeof b, &out), "block decoded");
    TEST_ASSERT(strcmp(out.e[0].name, "? ~??") == 0,
                "0x1F->'?', 0x20->' ', 0x7E->'~', 0x7F->'?', 0x80->'?'");
}

/* Review item 9: a flags byte with some OTHER bit set (0x02) but bit 0
 * clear must decode `more` as false — pins that only CHEST_LIST_FLAG_MORE
 * (bit 0) is tested, not "the byte is nonzero". */
static void test_chest_list_decode_flags_other_bit_not_more(void)
{
    uint8_t b[6];
    chest_list_t out;
    b[CHEST_LIST_OFF_TOTAL] = 12;
    b[CHEST_LIST_OFF_COUNT] = 0;
    b[CHEST_LIST_OFF_FIRST] = 12;
    b[CHEST_LIST_OFF_FLAGS] = 0x02;   /* some other bit, not CHEST_LIST_FLAG_MORE */
    uint16_t crc = cr_crc16(b, 4);
    b[4] = (uint8_t)(crc & 0xFF);
    b[5] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(chest_list_decode(b, sizeof b, &out), "block decoded");
    TEST_ASSERT(!out.more, "flags 0x02 (not bit 0): more is false");
}

/* Review item 5: the acceptance edges an all-refusal test suite can miss.
 * An empty page {12,0,12,0} — total 12, nothing in THIS page, first
 * requested is 12 (one past the end), no more — must be ACCEPTED (count 0
 * is not itself a refusal reason), and `more` must read false. */
static void test_chest_list_decode_empty_page_ok(void)
{
    uint8_t b[6] = { 12, 0, 12, 0, 0, 0 };
    chest_list_t out;
    uint16_t crc = cr_crc16(b, 4);
    b[4] = (uint8_t)(crc & 0xFF);
    b[5] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(chest_list_decode(b, sizeof b, &out), "an empty page (count 0) is accepted");
    TEST_ASSERT_EQ(out.total, 12, "total 12");
    TEST_ASSERT_EQ(out.count, 0, "count 0");
    TEST_ASSERT_EQ(out.first, 12, "first 12 (one past the end)");
    TEST_ASSERT(!out.more, "no more");
}

/* count above CHEST_LIST_MAX_ENTRIES with an otherwise-tiny buffer: refused
 * before any entry is walked (a cheap, non-realistic buffer is fine here
 * since the count check itself is what is under test, not the entry loop —
 * see test_chest_list_decode_33_entries_refused for the realistic case). */
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

/* Review item 3: C1 with one bit flipped in the CRC's high byte. */
static void test_chest_code_decode_c1_crc_bit_flipped(void)
{
    uint8_t b[sizeof k_vec_c1];
    chest_code_t out;
    memcpy(b, k_vec_c1, sizeof b);
    b[13] ^= 0x01;

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_code_decode(b, sizeof b, &out), "one CRC bit flipped (b[13]): refused");
    TEST_ASSERT_EQ(out.index, 0xAA, "out left untouched");
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
    b[4] = 'A';   /* one of the six code digits (not the padding at b[2]/b[3]) */
    uint16_t crc = cr_crc16(b, CHEST_CODE_SIZE - 2);
    b[CHEST_CODE_SIZE - 2] = (uint8_t)(crc & 0xFF);
    b[CHEST_CODE_SIZE - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_code_decode(b, sizeof b, &out), "a letter in the code bytes is refused");
    TEST_ASSERT_EQ(out.index, 0xAA, "out left untouched");
}

/* Review item 10: link_proto_pack_code (Niphar_chest main/link/link_proto.c
 * at 440d79d) memsets the whole 8-byte field to '0' before writing the code
 * right-justified — for digits == 6 the two leftmost bytes (b[2], b[3]) are
 * ALWAYS that padding. C1 with b[2] patched from '0' to '1' (still a digit,
 * so the earlier "non-digit" check does not catch it), CRC recomputed. */
static void test_chest_code_decode_c1_digits6_bad_padding(void)
{
    uint8_t b[sizeof k_vec_c1];
    chest_code_t out;
    memcpy(b, k_vec_c1, sizeof b);
    b[2] = '1';
    uint16_t crc = cr_crc16(b, CHEST_CODE_SIZE - 2);
    b[CHEST_CODE_SIZE - 2] = (uint8_t)(crc & 0xFF);
    b[CHEST_CODE_SIZE - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_code_decode(b, sizeof b, &out),
                "digits 6 with a non-'0' padding byte (b[2]) is refused: not a code the chest's packer could produce");
    TEST_ASSERT_EQ(out.index, 0xAA, "out left untouched");

    /* Same for the other padding byte, b[3]. */
    memcpy(b, k_vec_c1, sizeof b);
    b[3] = '9';
    crc = cr_crc16(b, CHEST_CODE_SIZE - 2);
    b[CHEST_CODE_SIZE - 2] = (uint8_t)(crc & 0xFF);
    b[CHEST_CODE_SIZE - 1] = (uint8_t)(crc >> 8);
    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_code_decode(b, sizeof b, &out), "digits 6 with a non-'0' padding byte (b[3]) is refused");
}

/* Review item 5: seconds at its legitimate maximum (30, SEC_TIME_TOTP_STEP)
 * must be ACCEPTED. */
static void test_chest_code_decode_seconds_30_ok(void)
{
    uint8_t b[sizeof k_vec_c1];
    chest_code_t out;
    memcpy(b, k_vec_c1, sizeof b);
    b[10] = 30;
    uint16_t crc = cr_crc16(b, CHEST_CODE_SIZE - 2);
    b[CHEST_CODE_SIZE - 2] = (uint8_t)(crc & 0xFF);
    b[CHEST_CODE_SIZE - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(chest_code_decode(b, sizeof b, &out), "seconds 30 (the legitimate maximum) is accepted");
    TEST_ASSERT_EQ(out.seconds, 30, "seconds decoded as 30");
}

/* Review item 7: seconds 0 must be REFUSED — the chest's own
 * sec_time_window_remaining() never returns 0 (range 1..30); a 0 would read
 * as an already-expired code, which the keyboard must never show. */
static void test_chest_code_decode_seconds_0_refused(void)
{
    uint8_t b[sizeof k_vec_c1];
    chest_code_t out;
    memcpy(b, k_vec_c1, sizeof b);
    b[10] = 0;
    uint16_t crc = cr_crc16(b, CHEST_CODE_SIZE - 2);
    b[CHEST_CODE_SIZE - 2] = (uint8_t)(crc & 0xFF);
    b[CHEST_CODE_SIZE - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(!chest_code_decode(b, sizeof b, &out), "seconds 0 refused (an already-expired window)");
    TEST_ASSERT_EQ(out.index, 0xAA, "out left untouched");
}

/* C1 with seconds patched to 31 (one over the chest's own maximum), CRC
 * recomputed. */
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

/* Review item 5: an 8-digit code fills the whole field, no padding to check
 * — built fresh (not from C1, which is 6 digits): index 7, digits 8, code
 * "12345678", seconds 20. */
static void test_chest_code_decode_8_digits_ok(void)
{
    uint8_t b[CHEST_CODE_SIZE];
    chest_code_t out;
    b[0] = 7;
    b[1] = 8;
    memcpy(&b[2], "12345678", 8);
    b[10] = 20;
    b[11] = 0;
    uint16_t crc = cr_crc16(b, CHEST_CODE_SIZE - 2);
    b[CHEST_CODE_SIZE - 2] = (uint8_t)(crc & 0xFF);
    b[CHEST_CODE_SIZE - 1] = (uint8_t)(crc >> 8);

    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT(chest_code_decode(b, sizeof b, &out), "an 8-digit code, field entirely full, is accepted");
    TEST_ASSERT_EQ(out.index, 7, "index 7");
    TEST_ASSERT_EQ(out.digits, 8, "digits 8");
    TEST_ASSERT(strcmp(out.code, "12345678") == 0, "the whole 8-byte field is the code, no padding to strip");
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
    TEST_RUN(test_chest_list_decode_namelen_35_present);
    TEST_RUN(test_chest_list_decode_namelen_34_ok);
    TEST_RUN(test_chest_list_decode_l1_leftover_byte);
    TEST_RUN(test_chest_list_decode_32_entries_ok);
    TEST_RUN(test_chest_list_decode_33_entries_refused);
    TEST_RUN(test_chest_list_decode_513_bytes_refused);
    TEST_RUN(test_chest_list_decode_sanitize_bounds);
    TEST_RUN(test_chest_list_decode_flags_other_bit_not_more);
    TEST_RUN(test_chest_list_decode_empty_page_ok);
    TEST_RUN(test_chest_list_decode_count_over_max_entries);
    TEST_RUN(test_chest_list_decode_unsafe_inputs);
    TEST_RUN(test_chest_code_decode_c1);
    TEST_RUN(test_chest_code_decode_c1_crc_bit_flipped);
    TEST_RUN(test_chest_code_decode_c1_digits_7);
    TEST_RUN(test_chest_code_decode_c1_letter_in_code);
    TEST_RUN(test_chest_code_decode_c1_digits6_bad_padding);
    TEST_RUN(test_chest_code_decode_seconds_30_ok);
    TEST_RUN(test_chest_code_decode_seconds_0_refused);
    TEST_RUN(test_chest_code_decode_c1_seconds_over_30);
    TEST_RUN(test_chest_code_decode_8_digits_ok);
    TEST_RUN(test_chest_code_decode_wrong_length);
    TEST_RUN(test_chest_code_decode_unsafe_inputs);
}
