/* The chest link, byte for byte — Niphar_chest main/link/link_proto.h and
 * test/test_link_proto.c at commit cfd7b35, protocol version 3 (64-byte
 * register block: account label, DMA channel signalling, TIME_VALID). The
 * register vectors below (V1, V4, V5, V6, V6b-g, V8-V16) are copied VERBATIM
 * from k_vec_v1..k_vec_v16 in that file (lines ~360-566), never edited: a
 * change on either side that the other did not follow breaks a fast check,
 * not the bench. Spec: docs/superpowers/specs/2026-09-29-chest-link-v3-design.md. */
#include "test_framework.h"
#include <string.h>
#include "../main/comm/chest/chest_proto.h"
#include "../main/security/cr_crc16.h"

/* Contract literals, pinned at compile time: a drift here is a wire-format
 * break, not a runtime bug, and the build should say so before a test does.
 * Values from Niphar_chest main/link/link_proto.h at cfd7b35. */
_Static_assert(CHEST_DMA_MAX == 512, "CHEST_DMA_MAX must equal the chest's LINK_DMA_MAX");
_Static_assert(CHEST_REG_REQ_SEQ == 0x3C, "CHEST_REG_REQ_SEQ must equal the chest's LINK_REG_REQ_SEQ (the doorbell)");
_Static_assert(CHEST_REG_USER_CONFIRM == 0x38, "CHEST_REG_USER_CONFIRM must equal the chest's LINK_REG_USER_CONFIRM");
_Static_assert(CHEST_REG_ECHO == 0x39, "CHEST_REG_ECHO must equal the chest's LINK_REG_CONFIRM_ECHO");
_Static_assert(CHEST_REG_MODE_REQ == 0x3A, "CHEST_REG_MODE_REQ must equal the chest's LINK_REG_USB_MODE_REQ");
_Static_assert(CHEST_LABEL_MAX == 34, "CHEST_LABEL_MAX must equal the chest's LINK_LABEL_MAX");

/* Niphar_chest test/test_link_proto.c, commit e5ee37d (V1-derived and V16 re-copied by script 2026-09-29: pending op 7/10 from sec_op_t) — copied verbatim, never edited. */

/* V1 — nominal v3: a pending OATH code for GITHUB. SD + USB mounted + ready +
 * time valid, 42 confirmations, instance 3, active mode oath. The label at
 * 0x14 is what makes it v3. */
static const uint8_t V1[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x21, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V4 — magic word wrong by one byte, everything else identical to V1. */
static const uint8_t V4[64] = {
    0x4E, 0x49, 0x50, 0x58, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x21, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V5 — version 4 announced, CRC RECOMPUTED and correct: refused on version
 * alone, not on a corruption. */
static const uint8_t V5[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x04, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x6B, 0xD4, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V6 — one payload bit flipped (42 -> 43), CRC left untouched. */
static const uint8_t V6[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2B, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x21, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V6b — payload intact, one bit flipped in the LOW byte of the CRC. */
static const uint8_t V6b[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF2, 0x21, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V6c — the same bit flipped in the HIGH byte of the CRC. An implementation
 * that only compares the low byte would pass V6b and fail here. */
static const uint8_t V6c[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x20, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V6d — instance changed (3 -> 2) with no recalculation: proof, in bytes,
 * that the instance is inside the CRC's coverage. */
static const uint8_t V6d[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x02, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x21, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V6e — active mode changed (oath -> storage) with no recalculation. */
static const uint8_t V6e[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x01, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x21, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V6f — one byte of the LABEL changed ("G" -> "g") with no CRC
 * recalculation: proof that the label is covered too. */
static const uint8_t V6f[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x67, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x21, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V6g — impossible label length (35 for a 34-byte field), CRC RECOMPUTED and
 * correct. The refusal is about the block's COHERENCE, not its
 * transmission: a master that truncated instead of refusing would read
 * bytes that are not the label — Review Focus. */
static const uint8_t V6g[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x23, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x70, 0xD8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V8 — V1 plus a confirmation posed, echo on the ARMED instance (3). Same
 * CRC as V1: that is the whole argument about the CRC's span. */
static const uint8_t V8[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x21, 0x5A, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V9 — chest present and NOT ready: no state bit, nothing pending, no
 * label, no time. Its non-zero CRC distinguishes it from an absent block. */
static const uint8_t V9[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x95, 0x15, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V10 — uniform block EXCEPT the last byte: present, not absent. */
static const uint8_t V10[64] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00,
};
/* V11 — the v1 defect, in bytes: well-formed confirmation, echo on the
 * PREVIOUS instance (2 for an armed instance of 3). */
static const uint8_t V11[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x21, 0x5A, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V12 — an unknown requested mode (0x09): the block stays valid, it is the
 * REQUEST that is refused. */
static const uint8_t V12[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x21, 0x00, 0x00, 0x09, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V13 — the same with an assigned value (0x02, pgp): applied. */
static const uint8_t V13[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF3, 0x21, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V14 — a switch IN PROGRESS: active mode indeterminate (0xFF), USB_MOUNTED
 * dropped. The only state where requested and active legitimately differ. */
static const uint8_t V14[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0D, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0xFF, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x30, 0x15, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V15 — ready and mounted, but NO time posed (bit 3 clear). The keyboard
 * must show "NO TIME" and never request a code. */
static const uint8_t V15[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x07, 0x07, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x49, 0x54, 0x48, 0x55, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x15, 0xF5, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
/* V16 — a RESET pending: TWELVE accounts leave on one press. The label says
 * so in words and 0x0F says so in one byte, so the keyboard can show
 * "12 CPT" without parsing text. */
static const uint8_t V16[64] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x0F, 0x0A, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0x0A, 0x0C, 0x00, 0x00, 0x00, 0x00,
    0x31, 0x32, 0x20, 0x43, 0x4F, 0x4D, 0x50, 0x54, 0x45, 0x53,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x7A, 0xAF, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

/* Contract §1: compare check values, not names (the chest's header calls it X-25; it is MCRF4XX). */
static void test_chest_crc_check_value(void)
{
    TEST_ASSERT_EQ(cr_crc16((const uint8_t *)"123456789", 9), 0x6F91, "CRC-16/MCRF4XX check value (X-25 would be 0x906E)");
}

static void test_chest_v3_vectors_accepted(void)
{
    chest_status_t s;

    /* 0xAA, not 0: a dropped or misplaced NUL terminator must not hide
     * behind a buffer that already reads as zero. */
    memset(&s, 0xAA, sizeof s);
    TEST_ASSERT_EQ(chest_proto_parse(V1, 64, &s), CHEST_BLOCK_OK, "V1 nominal");
    TEST_ASSERT_EQ(s.version, 3, "V1 version 3");
    TEST_ASSERT_EQ(s.state, 0x0F, "V1 state: SD + mounted + ready + time valid");
    TEST_ASSERT_EQ(s.pending_op, 7, "V1 pending op: SEC_OP_OATH_CODE");
    TEST_ASSERT_EQ(s.confirm_count, 42, "V1 count 42");
    TEST_ASSERT_EQ(s.instance, 3, "V1 instance 3");
    TEST_ASSERT_EQ(s.active_mode, CHEST_MODE_OATH, "V1 active oath");
    TEST_ASSERT_EQ(s.op_count, 1, "V1 one account targeted");
    TEST_ASSERT_EQ(s.label_len, 6, "V1 label length");
    TEST_ASSERT_EQ(memcmp(s.label, "GITHUB", 6), 0, "V1 the account is NAMED");
    TEST_ASSERT_EQ(s.label[6], '\0', "V1 label NUL-terminated at label_len");
    TEST_ASSERT(s.state & CHEST_STATE_TIME, "V1 time posed");
    TEST_ASSERT_EQ(s.dma_kind, CHEST_DMA_NONE, "V1 no DMA segment queued");
    TEST_ASSERT_EQ(s.dma_seq, 0, "V1 dma_seq 0");
    TEST_ASSERT_EQ(s.dma_len, 0, "V1 dma_len 0");
    TEST_ASSERT(!chest_proto_is_absent(V1, 64), "V1 not absent");

    /* V8 differs from V1 only in the master's range — same verdict and
     * fields. That is the CRC span made visible. */
    memset(&s, 0, sizeof s);
    TEST_ASSERT_EQ(chest_proto_parse(V8, 64, &s), CHEST_BLOCK_OK, "V8 master word outside the CRC");
    TEST_ASSERT_EQ(s.state, 0x0F, "V8 state identical to V1");
    TEST_ASSERT_EQ(s.pending_op, 7, "V8 op identical to V1");
    TEST_ASSERT_EQ(memcmp(s.label, "GITHUB", 6), 0, "V8 same label as V1");
    TEST_ASSERT_EQ(s.confirm_count, 42, "V8 count identical to V1");
    TEST_ASSERT_EQ(s.instance, 3, "V8 instance identical to V1");

    memset(&s, 0, sizeof s);
    TEST_ASSERT_EQ(chest_proto_parse(V9, 64, &s), CHEST_BLOCK_OK, "V9 present, booting");
    TEST_ASSERT_EQ(s.state, 0, "V9 not ready");
    TEST_ASSERT_EQ(s.pending_op, 0, "V9 nothing pending");
    TEST_ASSERT_EQ(s.confirm_count, 0, "V9 count 0");
    TEST_ASSERT_EQ(s.instance, 0, "V9 nothing ever armed");
    TEST_ASSERT_EQ(s.active_mode, CHEST_MODE_NONE, "V9 no active mode");
    TEST_ASSERT_EQ(s.label_len, 0, "V9 no label");
    TEST_ASSERT_EQ(s.op_count, 0, "V9 no account targeted");
    TEST_ASSERT_EQ(s.state & CHEST_STATE_TIME, 0, "V9 no time posed");
    TEST_ASSERT(!chest_proto_is_absent(V9, 64), "V9 present and not ready, not absent");

    TEST_ASSERT(!chest_proto_is_absent(V10, 64), "V10 one different byte is enough: present");
    TEST_ASSERT_EQ(chest_proto_parse(V10, 64, &s), CHEST_BLOCK_CORRUPT, "V10 not a chest");

    /* V11, V12, V13 carry a chest block identical to V1: it is the MASTER's
     * range that differs, and it is outside the CRC. */
    memset(&s, 0, sizeof s);
    TEST_ASSERT_EQ(chest_proto_parse(V11, 64, &s), CHEST_BLOCK_OK, "V11 block valid");
    TEST_ASSERT_EQ(s.instance, 3, "V11 armed instance");
    TEST_ASSERT_EQ(chest_proto_parse(V12, 64, &s), CHEST_BLOCK_OK, "V12 block valid");
    TEST_ASSERT_EQ(chest_proto_parse(V13, 64, &s), CHEST_BLOCK_OK, "V13 block valid");

    TEST_ASSERT_EQ(chest_proto_parse(V14, 64, &s), CHEST_BLOCK_OK, "V14 switch in flight is valid");
    TEST_ASSERT_EQ(s.active_mode, CHEST_MODE_IN_FLIGHT, "V14 active indeterminate");
    TEST_ASSERT_EQ(s.state & CHEST_STATE_USB, 0, "V14 USB_MOUNTED cleared");
}

static void test_chest_v3_vectors_rejected(void)
{
    chest_status_t s;

    TEST_ASSERT_EQ(chest_proto_parse(V4, 64, &s), CHEST_BLOCK_CORRUPT, "V4 bad magic");
    TEST_ASSERT_EQ(chest_proto_parse(V5, 64, &s), CHEST_BLOCK_BAD_VERSION, "V5 version 4 refused");
    TEST_ASSERT_EQ(chest_proto_parse(V6, 64, &s), CHEST_BLOCK_CORRUPT, "V6 payload bit");
    TEST_ASSERT_EQ(chest_proto_parse(V6b, 64, &s), CHEST_BLOCK_CORRUPT, "V6b CRC low byte");
    TEST_ASSERT_EQ(chest_proto_parse(V6c, 64, &s), CHEST_BLOCK_CORRUPT, "V6c CRC high byte");
    TEST_ASSERT_EQ(chest_proto_parse(V6d, 64, &s), CHEST_BLOCK_CORRUPT, "V6d instance covered by the CRC");
    TEST_ASSERT_EQ(chest_proto_parse(V6e, 64, &s), CHEST_BLOCK_CORRUPT, "V6e active mode covered by the CRC");
    TEST_ASSERT_EQ(chest_proto_parse(V6f, 64, &s), CHEST_BLOCK_CORRUPT, "V6f one byte of the LABEL covered by the CRC");
    /* Review Focus: V6g must be CORRUPT even with a valid CRC — a truncating
     * parser would show a name the chest never composed. */
    TEST_ASSERT_EQ(chest_proto_parse(V6g, 64, &s), CHEST_BLOCK_CORRUPT, "V6g impossible label length refused despite a correct CRC");
    TEST_ASSERT_EQ(chest_proto_parse(V1, 63, &s), CHEST_BLOCK_CORRUPT, "V7 truncated");

    TEST_ASSERT(!chest_proto_is_absent(V10, 64), "V10 a single different byte rules out absence");
    TEST_ASSERT_EQ(chest_proto_parse(V10, 64, &s), CHEST_BLOCK_CORRUPT, "V10 not interpreted");

    TEST_ASSERT(!chest_proto_is_absent(V4, 64), "V4 not an absent block");
    TEST_ASSERT(!chest_proto_is_absent(V5, 64), "V5 not an absent block");
    TEST_ASSERT(!chest_proto_is_absent(V6, 64), "V6 not an absent block");
    TEST_ASSERT(!chest_proto_is_absent(V6b, 64), "V6b not an absent block");
    TEST_ASSERT(!chest_proto_is_absent(V6c, 64), "V6c not an absent block");
    TEST_ASSERT(!chest_proto_is_absent(V6d, 64), "V6d not an absent block");
}

/* V15 and V16 — the two states the v3 layout makes visible. */
static void test_chest_v3_states(void)
{
    chest_status_t s;

    memset(&s, 0, sizeof s);
    TEST_ASSERT_EQ(chest_proto_parse(V15, 64, &s), CHEST_BLOCK_OK, "V15 accepted");
    TEST_ASSERT_EQ(s.state & CHEST_STATE_TIME, 0, "V15 no time posed");
    TEST_ASSERT(s.state & CHEST_STATE_USB, "V15 mounted anyway");
    TEST_ASSERT(s.state & CHEST_STATE_READY, "V15 ready anyway");

    memset(&s, 0, sizeof s);
    TEST_ASSERT_EQ(chest_proto_parse(V16, 64, &s), CHEST_BLOCK_OK, "V16 accepted");
    TEST_ASSERT_EQ(s.pending_op, 10, "V16 pending op: SEC_OP_OATH_RESET");
    TEST_ASSERT_EQ(s.op_count, 12, "V16 twelve accounts leave on ONE press");
    TEST_ASSERT_EQ(s.label_len, 10, "V16 label length");
    TEST_ASSERT_EQ(memcmp(s.label, "12 COMPTES", 10), 0, "V16 the label states the count in words");
    TEST_ASSERT(s.op_count > 1, "V16 the keyboard shows N CPT");
}

/* Non-printable bytes in the label are shown as '?' — never interpreted —
 * and the sanitized label stays NUL-terminated at label_len. Not one of the
 * chest's pinned vectors: built here from V1 with a byte inside "GITHUB"
 * replaced and the CRC recomputed. */
static void test_chest_label_sanitized_and_terminated(void)
{
    uint8_t b[64];
    chest_status_t s;
    memcpy(b, V1, 64);
    b[CHEST_REG_LABEL + 1] = 0x01;   /* the 'I' of GITHUB becomes a control byte */
    uint16_t crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF);
    b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);

    memset(&s, 0xAA, sizeof s);
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_OK, "a non-printable label byte does not corrupt the block");
    TEST_ASSERT_EQ(s.label[0], 'G', "byte before is untouched");
    TEST_ASSERT_EQ(s.label[1], '?', "the non-printable byte is shown as '?'");
    TEST_ASSERT_EQ(memcmp(&s.label[2], "THUB", 4), 0, "the rest is untouched");
    TEST_ASSERT_EQ(s.label[6], '\0', "still NUL-terminated at label_len");
}

/* Review Focus boundary: label_len == CHEST_LABEL_MAX (34) must be OK, not
 * CORRUPT — the one length that legitimately fills the whole field, edge to
 * edge with no padding left. Catches `> CHEST_LABEL_MAX` weakened to
 * `> CHEST_LABEL_MAX - 1`, which would refuse it. Not one of the chest's
 * pinned vectors: built from V1 with label_len set to 34, the field filled
 * with printable bytes, and the CRC recomputed. */
static void test_chest_label_len_34_is_ok(void)
{
    uint8_t b[64];
    chest_status_t s;
    memcpy(b, V1, 64);
    b[CHEST_REG_LABEL_LEN] = CHEST_LABEL_MAX;
    for (uint8_t i = 0; i < CHEST_LABEL_MAX; i++)
        b[CHEST_REG_LABEL + i] = (uint8_t)('A' + (i % 26));
    uint16_t crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF);
    b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);

    memset(&s, 0xAA, sizeof s);
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_OK, "label_len == CHEST_LABEL_MAX (34) is accepted, not corrupt");
    TEST_ASSERT_EQ(s.label_len, CHEST_LABEL_MAX, "label_len decoded as 34");
    TEST_ASSERT_EQ(s.label[33], (char)('A' + (33 % 26)), "last byte of a full-width label copied");
    TEST_ASSERT_EQ(s.label[34], '\0', "NUL right after the last byte, still in bounds (array is CHEST_LABEL_MAX + 1)");
}

/* DMA fields decoded straight off their own offsets, not swapped and not
 * truncated: kind=LIST, seq=7, len=0x0180 (384). Not one of the chest's
 * vectors: built from V1 with the DMA fields patched and the CRC
 * recomputed. */
static void test_chest_dma_fields_decoded_from_regs(void)
{
    uint8_t b[64];
    chest_status_t s;
    memcpy(b, V1, 64);
    b[CHEST_REG_DMA_KIND]     = CHEST_DMA_LIST;   /* 0x10 */
    b[CHEST_REG_DMA_SEQ]      = 0x07;             /* 0x11 */
    b[CHEST_REG_DMA_LEN]      = 0x80;             /* 0x12, LE low byte */
    b[CHEST_REG_DMA_LEN + 1]  = 0x01;             /* 0x13, LE high byte: 0x0180 = 384 */
    uint16_t crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF);
    b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);

    memset(&s, 0xAA, sizeof s);
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_OK, "DMA-carrying block accepted");
    TEST_ASSERT_EQ(s.dma_kind, CHEST_DMA_LIST, "dma_kind decoded — not swapped with dma_seq");
    TEST_ASSERT_EQ(s.dma_seq, 7, "dma_seq decoded — not swapped with dma_kind");
    TEST_ASSERT_EQ(s.dma_len, 384, "dma_len decoded little-endian — high byte not dropped");
}

/* A uniform block, either polarity, is ABSENT — the ordinary case (chest on
 * battery, no chest wired at all), not a corruption. */
static void test_chest_absent_blocks(void)
{
    uint8_t zero[64], ones[64];
    chest_status_t s;
    memset(zero, 0x00, sizeof zero);
    memset(ones, 0xFF, sizeof ones);
    TEST_ASSERT_EQ(chest_proto_parse(zero, 64, &s), CHEST_BLOCK_ABSENT, "64 zero bytes: absent");
    TEST_ASSERT_EQ(chest_proto_parse(ones, 64, &s), CHEST_BLOCK_ABSENT, "64 0xFF bytes: absent");
}

/* An older version (2, and 1) must be refused specifically as BAD_VERSION,
 * not silently accepted. Catches the version check's `!=` weakened to `>`,
 * which would let anything below CHEST_PROTO_VERSION slip through as OK.
 * Not one of the chest's vectors: built from V1 with the version byte
 * lowered and the CRC recomputed. */
static void test_chest_older_version_is_bad_version(void)
{
    uint8_t b[64];
    chest_status_t s;

    memcpy(b, V1, 64);
    b[0x04] = 2;
    uint16_t crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF);
    b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_BAD_VERSION, "version 2 refused as BAD_VERSION");

    memcpy(b, V1, 64);
    b[0x04] = 1;
    crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF);
    b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_BAD_VERSION, "version 1 refused as BAD_VERSION");
}

/* Master-word offsets checked against the chest's OWN vectors
 * (test/test_link_proto.c, cfd7b35, test_shared_vectors_master_side): V8
 * carries a well-formed confirmation echoing the armed instance (m.confirm
 * == LINK_USER_CONFIRM_MAGIC, m.echo == 3); V13 carries a requested mode of
 * pgp (m.usb_mode == LINK_USB_MODE_PGP == 0x02). Read here at the raw
 * offsets chest_link.c uses to write/read that range. */
static void test_chest_master_offsets_against_chest_vectors(void)
{
    TEST_ASSERT_EQ(V8[CHEST_REG_USER_CONFIRM], CHEST_CONFIRM_MAGIC, "V8 confirm byte");
    TEST_ASSERT_EQ(V8[CHEST_REG_ECHO], 3, "V8 echoes the armed instance");
    TEST_ASSERT_EQ(V13[CHEST_REG_MODE_REQ], CHEST_MODE_PGP, "V13 requests pgp");
}

/* Sanitize bounds: 0x7E ('~') is the top of the kept range, 0x7F (DEL) and
 * 0x80 (high bit) sit just outside it on either side and both become '?'.
 * Not one of the chest's vectors: built from V1 with three label bytes
 * patched and the CRC recomputed. */
static void test_chest_label_sanitize_bounds(void)
{
    uint8_t b[64];
    chest_status_t s;
    memcpy(b, V1, 64);
    b[CHEST_REG_LABEL_LEN] = 3;
    b[CHEST_REG_LABEL + 0] = 0x7E;   /* '~', top of the kept range */
    b[CHEST_REG_LABEL + 1] = 0x7F;   /* DEL, just above: refused */
    b[CHEST_REG_LABEL + 2] = 0x80;   /* high bit, refused */
    uint16_t crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF);
    b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);

    memset(&s, 0xAA, sizeof s);
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_OK, "block accepted");
    TEST_ASSERT_EQ(s.label[0], '~', "0x7E kept: top of the printable range");
    TEST_ASSERT_EQ(s.label[1], '?', "0x7F (DEL) replaced");
    TEST_ASSERT_EQ(s.label[2], '?', "0x80 replaced");
    TEST_ASSERT_EQ(s.label[3], '\0', "NUL-terminated at label_len 3");
}

static void test_chest_dma_segment_ok(void)
{
    chest_status_t s = {0};

    s.dma_kind = CHEST_DMA_LIST; s.dma_len = 1;
    TEST_ASSERT(chest_dma_segment_ok(&s), "LIST, length 1: ok");
    s.dma_len = CHEST_DMA_MAX;
    TEST_ASSERT(chest_dma_segment_ok(&s), "LIST, length at the max: ok");
    s.dma_len = CHEST_DMA_MAX + 1;
    TEST_ASSERT(!chest_dma_segment_ok(&s), "length over the max: refused");
    s.dma_kind = CHEST_DMA_CODE; s.dma_len = 14;
    TEST_ASSERT(chest_dma_segment_ok(&s), "CODE, length 14: ok");
    s.dma_kind = CHEST_DMA_NONE; s.dma_len = 1;
    TEST_ASSERT(!chest_dma_segment_ok(&s), "kind none refused even with a plausible length");
    s.dma_kind = CHEST_DMA_LIST; s.dma_len = 0;
    TEST_ASSERT(!chest_dma_segment_ok(&s), "length 0 refused");
    s.dma_kind = 3; s.dma_len = 10;
    TEST_ASSERT(!chest_dma_segment_ok(&s), "unknown kind refused");
    TEST_ASSERT(!chest_dma_segment_ok(NULL), "NULL refused");
}

/* USB_MOUNTED set with no known mode is self-contradictory: corrupt — KeSp's
 * own invariant, not part of the chest's own parse_status. */
static void test_chest_mounted_without_mode_is_corrupt(void)
{
    uint8_t b[64]; chest_status_t s;
    memcpy(b, V1, 64);
    b[CHEST_REG_MODE_ACTIVE] = CHEST_MODE_NONE;
    uint16_t crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF); b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_CORRUPT, "mounted + active none");
    b[CHEST_REG_MODE_ACTIVE] = CHEST_MODE_IN_FLIGHT;
    crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF); b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_CORRUPT, "mounted + active in flight");
    b[CHEST_REG_MODE_ACTIVE] = CHEST_MODE_OATH;
    crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF); b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_OK, "mounted + a known mode");
}

static void test_chest_noise_is_not_a_chest(void)
{
    /* Review focus: a booting/hung chest or a degrading bus returns noise. */
    uint8_t b[64]; chest_status_t s = { .pending_op = 0xBEEF };
    for (int i = 0; i < 64; i++) b[i] = (uint8_t)(0x31 * i + 7);
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_CORRUPT, "noise is not a chest");
    TEST_ASSERT_EQ(chest_proto_parse(V5, 64, &s), CHEST_BLOCK_BAD_VERSION, "and V5 neither (a version-4 block)");
    TEST_ASSERT_EQ(s.pending_op, 0xBEEF, "out untouched on any failure");
}

static void test_chest_absence_full_scan(void)
{
    /* chest_proto_is_absent must scan all bytes, not just a prefix. */
    uint8_t b[64];
    chest_status_t s;

    memset(b, 0x00, 64);
    b[63] = 0x01;
    TEST_ASSERT(!chest_proto_is_absent(b, 64), "0x00 block with last byte 0x01 is not absent");
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_CORRUPT, "and parse rejects it");

    memset(b, 0xFF, 64);
    b[63] = 0xFE;
    TEST_ASSERT(!chest_proto_is_absent(b, 64), "0xFF block with last byte 0xFE is not absent");
    TEST_ASSERT_EQ(chest_proto_parse(b, 64, &s), CHEST_BLOCK_CORRUPT, "and parse rejects it");
}

static void test_chest_op_labels(void)
{
    char l[CHEST_LABEL_BUF];
    static const char *exp[] = { "", "SIGN", "DECRYP", "AUTH", "OTP", "FIDO +", "FIDO",
                                 "TOTP", "DELETE", "REPLAC", "RESET!" };

    for (uint16_t op = 1; op <= 10; op++) {
        chest_op_label(op, l);
        TEST_ASSERT(strcmp(l, exp[op]) == 0, "label of each chest sec_op_t code");
        TEST_ASSERT(strlen(l) <= 6, "6 characters max");
    }

    chest_op_label(11, l);
    TEST_ASSERT(strcmp(l, "OP 11") == 0, "op 11 format");
    TEST_ASSERT(strlen(l) <= 6, "op 11 length <= 6");

    chest_op_label(42, l);
    TEST_ASSERT(strcmp(l, "OP 42") == 0, "op 42 format");

    chest_op_label(99, l);
    TEST_ASSERT(strcmp(l, "OP 99") == 0, "op 99 format");
    TEST_ASSERT(strlen(l) <= 6, "op 99 length <= 6");

    chest_op_label(100, l);
    TEST_ASSERT(strcmp(l, "OP ?") == 0, "op 100 format: OP ?");
    TEST_ASSERT(strlen(l) <= 6, "op 100 length <= 6");

    chest_op_label(1042, l);
    TEST_ASSERT(strcmp(l, "OP ?") == 0, "op 1042 format: OP ?");
}

static void test_chest_press_matches_tag(void)
{
    chest_status_t s;
    chest_proto_parse(V1, 64, &s);                                   /* op 7, instance 3, READY */
    TEST_ASSERT(chest_press_matches(CHEST_TAG(7, 3), CHEST_BLOCK_OK, &s), "same op, same instance");
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(7, 2), CHEST_BLOCK_OK, &s), "same op, older instance");
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(2, 3), CHEST_BLOCK_OK, &s), "other op");
    TEST_ASSERT(!chest_press_matches(0, CHEST_BLOCK_OK, &s), "no press");
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(7, 3), CHEST_BLOCK_CORRUPT, &s), "non-OK block");
    chest_proto_parse(V9, 64, &s);
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(0, 0), CHEST_BLOCK_OK, &s), "not READY, nothing pending");
}

static void test_chest_confirm_rule_v2(void)
{
    chest_status_t a, b;
    chest_proto_parse(V1, 64, &a);                                   /* op 7, inst 3, count 42 */
    chest_confirm_t c = {0};
    TEST_ASSERT(chest_confirm_request(&c, &a, 1000), "pending: accepted");
    TEST_ASSERT_EQ(c.instance, 3, "instance recorded");
    TEST_ASSERT(chest_confirm_step(&c, &a, 1000), "first write");
    TEST_ASSERT(!chest_confirm_step(&c, &a, 1199), "no retry at 199 ms");
    TEST_ASSERT(chest_confirm_step(&c, &a, 1200), "one retry at 200 ms");
    TEST_ASSERT(!chest_confirm_step(&c, &a, 1400), "never a third");
    /* Instance changed before the retry: same op code, another arming — no retry. */
    chest_confirm_request(&c, &a, 2000);
    TEST_ASSERT(chest_confirm_step(&c, &a, 2000), "write");
    b = a; b.instance = 4;
    TEST_ASSERT(!chest_confirm_step(&c, &b, 2250), "new instance: disarmed, no retry");
    TEST_ASSERT(!c.armed, "disarmed");
    /* Counter moved: delivered. */
    chest_confirm_request(&c, &a, 3000);
    chest_confirm_step(&c, &a, 3000);
    b = a; b.confirm_count = 43;
    TEST_ASSERT(!chest_confirm_step(&c, &b, 3050), "delivered");
    /* Counter moves AT the 200 ms retry boundary: still delivered, no retry
     * fires even though the timer alone would allow one. */
    chest_confirm_request(&c, &a, 5000);
    TEST_ASSERT(chest_confirm_step(&c, &a, 5000), "write at t");
    b = a; b.confirm_count = a.confirm_count + 1;
    TEST_ASSERT(!chest_confirm_step(&c, &b, 5200), "counter moved at 200 ms: delivered, no retry");
    TEST_ASSERT(!c.armed, "disarmed on delivery, not left armed for a phantom retry");
    b = a; b.pending_op = 0;
    TEST_ASSERT(!chest_confirm_request(&c, &b, 4000), "nothing pending: dropped");
}

static void test_chest_mode_logic(void)
{
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_NONE), CHEST_MODE_STORAGE, "none -> storage");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_STORAGE), CHEST_MODE_PGP, "storage -> pgp");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_PGP), CHEST_MODE_OTP, "pgp -> otp");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_OTP), CHEST_MODE_FIDO, "otp -> fido");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_FIDO), CHEST_MODE_OATH, "fido -> oath");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_OATH), CHEST_MODE_NONE, "oath -> none");
    TEST_ASSERT_EQ(chest_mode_next(0x09), CHEST_MODE_NONE, "unknown -> none");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_IN_FLIGHT), CHEST_MODE_NONE, "0xFF -> none");
    TEST_ASSERT(!chest_mode_needs_write(V13, CHEST_MODE_PGP), "V13 already asks pgp");
    TEST_ASSERT(chest_mode_needs_write(V1, CHEST_MODE_PGP), "V1 asks none: rewrite pgp (chest reboot self-heal)");
    TEST_ASSERT(!chest_mode_needs_write(V1, CHEST_MODE_NONE), "none wanted, none written");
    TEST_ASSERT(chest_mode_needs_write(V12, CHEST_MODE_NONE), "V12 carries 0x09: rewrite");
}

static void test_chest_mode_track(void)
{
    chest_mode_track_t t = {0};
    TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_PGP, CHEST_MODE_PGP), CHEST_MODE_ARRIVED, "arrived");
    /* V14: in flight for as long as the chest says so — never a fault. */
    for (int i = 0; i < 100; i++)
        TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_IN_FLIGHT, CHEST_MODE_OATH), CHEST_MODE_PENDING, "0xFF is pending");
    /* Two known values differing: pending for 7 reads, fault at the 8th. */
    for (int i = 0; i < CHEST_MODE_FAULT_READS - 1; i++)
        TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_STORAGE, CHEST_MODE_OATH), CHEST_MODE_PENDING, "differs, not yet a fault");
    TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_STORAGE, CHEST_MODE_OATH), CHEST_MODE_FAULT, "persisting: fault");
    TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_IN_FLIGHT, CHEST_MODE_OATH), CHEST_MODE_PENDING, "in flight resets the count");
    TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_OATH, CHEST_MODE_OATH), CHEST_MODE_ARRIVED, "caught up");
    /* ARRIVED itself must reset the fault counter, not just the IN_FLIGHT
     * branch that happened to precede it above: drive differ_reads back up
     * to FAULT_READS - 1 with no IN_FLIGHT read in between, land on ARRIVED,
     * then one more difference. If ARRIVED did not reset, this read would
     * be the FAULT_READS-th difference and report FAULT, not PENDING. */
    for (int i = 0; i < CHEST_MODE_FAULT_READS - 1; i++)
        TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_STORAGE, CHEST_MODE_OATH), CHEST_MODE_PENDING, "differs again, not yet a fault");
    TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_OATH, CHEST_MODE_OATH), CHEST_MODE_ARRIVED, "arrived again, counter reset (not via IN_FLIGHT)");
    TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_STORAGE, CHEST_MODE_OATH), CHEST_MODE_PENDING, "arrived reset the fault counter: fresh difference is pending, not fault");
}

static void test_chest_mode_label(void)
{
    char l[CHEST_MODE_LABEL_BUF];
    chest_mode_label(CHEST_MODE_ARRIVED, CHEST_MODE_FIDO, CHEST_MODE_FIDO, l);
    TEST_ASSERT(strcmp(l, "FIDO") == 0, "arrived: upper");
    chest_mode_label(CHEST_MODE_ARRIVED, CHEST_MODE_STORAGE, CHEST_MODE_STORAGE, l);
    TEST_ASSERT(strcmp(l, "MSC") == 0, "storage is MSC");
    chest_mode_label(CHEST_MODE_ARRIVED, CHEST_MODE_NONE, CHEST_MODE_NONE, l);
    TEST_ASSERT(l[0] == '\0', "none: nothing");
    chest_mode_label(CHEST_MODE_PENDING, CHEST_MODE_IN_FLIGHT, CHEST_MODE_OATH, l);
    TEST_ASSERT(strcmp(l, "oath") == 0, "pending: wanted, lower");
    chest_mode_label(CHEST_MODE_FAULT, CHEST_MODE_STORAGE, CHEST_MODE_PGP, l);
    TEST_ASSERT(strcmp(l, "ERR") == 0, "fault");
    for (uint8_t m = 0; m < CHEST_MODE_COUNT; m++) {
        chest_mode_label(CHEST_MODE_ARRIVED, m, m, l);
        TEST_ASSERT(strlen(l) <= 4, "4 characters max");
    }
}

void test_chest_proto(void)
{
    TEST_SUITE("chest link protocol (S3 master, v3)");
    TEST_RUN(test_chest_crc_check_value);
    TEST_RUN(test_chest_v3_vectors_accepted);
    TEST_RUN(test_chest_v3_vectors_rejected);
    TEST_RUN(test_chest_v3_states);
    TEST_RUN(test_chest_label_sanitized_and_terminated);
    TEST_RUN(test_chest_label_len_34_is_ok);
    TEST_RUN(test_chest_dma_fields_decoded_from_regs);
    TEST_RUN(test_chest_absent_blocks);
    TEST_RUN(test_chest_older_version_is_bad_version);
    TEST_RUN(test_chest_master_offsets_against_chest_vectors);
    TEST_RUN(test_chest_label_sanitize_bounds);
    TEST_RUN(test_chest_dma_segment_ok);
    TEST_RUN(test_chest_mounted_without_mode_is_corrupt);
    TEST_RUN(test_chest_noise_is_not_a_chest);
    TEST_RUN(test_chest_absence_full_scan);
    TEST_RUN(test_chest_op_labels);
    TEST_RUN(test_chest_press_matches_tag);
    TEST_RUN(test_chest_confirm_rule_v2);
    TEST_RUN(test_chest_mode_logic);
    TEST_RUN(test_chest_mode_track);
    TEST_RUN(test_chest_mode_label);
}
