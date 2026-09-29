/* The chest link, byte for byte — Niphar_chest LINK_CONTRACT.md §11, protocol
 * version 2. Vectors V1-V14 were produced by the chest's own link_proto.c and
 * are pinned in its test/test_link_proto.c too: a change on either side that
 * the other did not follow breaks a fast check, not the bench. Copied
 * verbatim, never edited. */
#include "test_framework.h"
#include <string.h>
#include "../main/comm/chest/chest_proto.h"
#include "../main/security/cr_crc16.h"

/* Niphar_chest docs/LINK_CONTRACT.md §11, v2, commit 14f9352 — copied verbatim, never edited. */
static const uint8_t V1[20]  = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V2[20]  = { 0 };
static const uint8_t V3[20]  = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const uint8_t V4[20]  = { 0x4E,0x49,0x50,0x58,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V5[20]  = { 0x4E,0x49,0x50,0x48,0x03,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xCC,0x07,0x00,0x00,0x00,0x00 };
static const uint8_t V6[20]  = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2B,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V6b[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEA,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V6c[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2A,0x00,0x00,0x00,0x00 };
static const uint8_t V6d[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x02,0x01,0xEB,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V6e[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x05,0xEB,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V8[20]  = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x5A,0x03,0x00,0x00 };
static const uint8_t V9[20]  = { 0x4E,0x49,0x50,0x48,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x39,0xD4,0x00,0x00,0x00,0x00 };
static const uint8_t V10[20] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };
static const uint8_t V11[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x5A,0x02,0x00,0x00 };
static const uint8_t V12[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x00,0x00,0x09,0x00 };
static const uint8_t V13[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x00,0x00,0x02,0x00 };
static const uint8_t V14[20] = { 0x4E,0x49,0x50,0x48,0x02,0x05,0x00,0x00,0x2A,0x00,0x00,0x00,0x03,0xFF,0x5F,0x2F,0x00,0x00,0x00,0x00 };

/* Contract §1: compare check values, not names (the chest's header calls it X-25; it is MCRF4XX). */
static void test_chest_crc_check_value(void)
{
    TEST_ASSERT_EQ(cr_crc16((const uint8_t *)"123456789", 9), 0x6F91, "CRC-16/MCRF4XX check value (X-25 would be 0x906E)");
}

static void test_chest_v2_vectors(void)
{
    chest_status_t s;
    TEST_ASSERT_EQ(chest_proto_parse(V1, 20, &s), CHEST_BLOCK_OK, "V1 nominal");
    TEST_ASSERT_EQ(s.version, 2, "V1 version 2");
    TEST_ASSERT_EQ(s.state, 0x07, "V1 state");
    TEST_ASSERT_EQ(s.pending_op, 1, "V1 PSO:CDS");
    TEST_ASSERT_EQ(s.confirm_count, 42, "V1 count 42");
    TEST_ASSERT_EQ(s.instance, 3, "V1 instance 3");
    TEST_ASSERT_EQ(s.active_mode, CHEST_MODE_STORAGE, "V1 active storage");
    TEST_ASSERT_EQ(chest_proto_parse(V2, 20, &s), CHEST_BLOCK_ABSENT, "V2 absent 0x00");
    TEST_ASSERT_EQ(chest_proto_parse(V3, 20, &s), CHEST_BLOCK_ABSENT, "V3 absent 0xFF");
    TEST_ASSERT_EQ(chest_proto_parse(V4, 20, &s), CHEST_BLOCK_CORRUPT, "V4 bad magic");
    TEST_ASSERT_EQ(chest_proto_parse(V5, 20, &s), CHEST_BLOCK_BAD_VERSION, "V5 version 3 refused");
    TEST_ASSERT_EQ(chest_proto_parse(V6, 20, &s), CHEST_BLOCK_CORRUPT, "V6 payload bit");
    TEST_ASSERT_EQ(chest_proto_parse(V6b, 20, &s), CHEST_BLOCK_CORRUPT, "V6b CRC low byte");
    TEST_ASSERT_EQ(chest_proto_parse(V6c, 20, &s), CHEST_BLOCK_CORRUPT, "V6c CRC high byte");
    TEST_ASSERT_EQ(chest_proto_parse(V6d, 20, &s), CHEST_BLOCK_CORRUPT, "V6d instance covered by the CRC");
    TEST_ASSERT_EQ(chest_proto_parse(V6e, 20, &s), CHEST_BLOCK_CORRUPT, "V6e active mode covered by the CRC");
    TEST_ASSERT_EQ(chest_proto_parse(V1, 19, &s), CHEST_BLOCK_CORRUPT, "V7 truncated");
    TEST_ASSERT_EQ(chest_proto_parse(V8, 20, &s), CHEST_BLOCK_OK, "V8 master word outside the CRC");
    TEST_ASSERT_EQ(chest_proto_parse(V9, 20, &s), CHEST_BLOCK_OK, "V9 present, booting");
    TEST_ASSERT_EQ(s.state, 0, "V9 not ready");
    TEST_ASSERT_EQ(s.instance, 0, "V9 nothing ever armed");
    TEST_ASSERT_EQ(s.active_mode, CHEST_MODE_NONE, "V9 no active mode");
    TEST_ASSERT(!chest_proto_is_absent(V10, 20), "V10 uniform but the last byte: present");
    TEST_ASSERT_EQ(chest_proto_parse(V10, 20, &s), CHEST_BLOCK_CORRUPT, "V10 not a chest");
    TEST_ASSERT_EQ(chest_proto_parse(V11, 20, &s), CHEST_BLOCK_OK, "V11 block valid");
    TEST_ASSERT_EQ(chest_proto_parse(V12, 20, &s), CHEST_BLOCK_OK, "V12 block valid");
    TEST_ASSERT_EQ(chest_proto_parse(V13, 20, &s), CHEST_BLOCK_OK, "V13 block valid");
    TEST_ASSERT_EQ(chest_proto_parse(V14, 20, &s), CHEST_BLOCK_OK, "V14 switch in flight is valid");
    TEST_ASSERT_EQ(s.active_mode, CHEST_MODE_IN_FLIGHT, "V14 active indeterminate");
    TEST_ASSERT_EQ(s.state & CHEST_STATE_USB, 0, "V14 USB_MOUNTED cleared");
}

/* A v1 chest (the 20-byte v1 map, version 1) is refused, not half-parsed. */
static void test_chest_v1_chest_refused(void)
{
    static const uint8_t v1_nominal[20] = { 0x4E,0x49,0x50,0x48,0x01,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0xAF,0xEA,0x00,0x00,0x00,0x00,0x00,0x00 };
    chest_status_t s;
    TEST_ASSERT_EQ(chest_proto_parse(v1_nominal, 20, &s), CHEST_BLOCK_BAD_VERSION, "a v1 chest is BAD_VERSION");
}

/* USB_MOUNTED set with no known mode is self-contradictory: corrupt (contract §1). */
static void test_chest_mounted_without_mode_is_corrupt(void)
{
    uint8_t b[20]; chest_status_t s;
    memcpy(b, V1, 20);
    b[CHEST_REG_MODE_ACTIVE] = CHEST_MODE_NONE;
    uint16_t crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF); b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "mounted + active none");
    b[CHEST_REG_MODE_ACTIVE] = CHEST_MODE_IN_FLIGHT;
    crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF); b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "mounted + active in flight");
    b[CHEST_REG_MODE_ACTIVE] = CHEST_MODE_OATH;
    crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF); b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_OK, "mounted + a known mode");
}

static void test_chest_noise_is_not_a_chest(void)
{
    /* Review focus: a booting/hung chest or a degrading bus returns noise. */
    uint8_t b[20]; chest_status_t s = { .pending_op = 0xBEEF };
    for (int i = 0; i < 20; i++) b[i] = (uint8_t)(0x31 * i + 7);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "noise is not a chest");
    TEST_ASSERT_EQ(chest_proto_parse(V5, 20, &s), CHEST_BLOCK_BAD_VERSION, "and V5 neither (a version-3 block)");
    TEST_ASSERT_EQ(s.pending_op, 0xBEEF, "out untouched on any failure");
}

static void test_chest_absence_full_scan(void)
{
    /* Gap 2: chest_proto_is_absent must scan all bytes, not just a prefix. */
    uint8_t b[20];
    chest_status_t s;

    /* 0x00 with last byte 0x01 is NOT absent. */
    memset(b, 0x00, 20);
    b[19] = 0x01;
    TEST_ASSERT(!chest_proto_is_absent(b, 20), "0x00 block with last byte 0x01 is not absent");
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "and parse rejects it");

    /* 0xFF with last byte 0xFE is NOT absent. */
    memset(b, 0xFF, 20);
    b[19] = 0xFE;
    TEST_ASSERT(!chest_proto_is_absent(b, 20), "0xFF block with last byte 0xFE is not absent");
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "and parse rejects it");
}

static void test_chest_op_labels(void)
{
    char l[CHEST_LABEL_BUF];
    static const char *exp[] = { "", "SIGN", "DECRYP", "AUTH", "OTP", "FIDO +", "FIDO",
                                 "TOTP", "DELETE", "REPLAC", "RESET!" };

    /* Known ops 1-10: use lookup table. */
    for (uint16_t op = 1; op <= 10; op++) {
        chest_op_label(op, l);
        TEST_ASSERT(strcmp(l, exp[op]) == 0, "label of each chest sec_op_t code");
        TEST_ASSERT(strlen(l) <= 6, "6 characters max");
    }

    /* Gap 3: Unknown ops formatting: 11-99 → "OP nn", >= 100 → "OP ?". */
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
    chest_proto_parse(V1, 20, &s);                                   /* op 1, instance 3, READY */
    TEST_ASSERT(chest_press_matches(CHEST_TAG(1, 3), CHEST_BLOCK_OK, &s), "same op, same instance");
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(1, 2), CHEST_BLOCK_OK, &s), "same op, older instance");
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(2, 3), CHEST_BLOCK_OK, &s), "other op");
    TEST_ASSERT(!chest_press_matches(0, CHEST_BLOCK_OK, &s), "no press");
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(1, 3), CHEST_BLOCK_CORRUPT, &s), "non-OK block");
    chest_proto_parse(V9, 20, &s);
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(0, 0), CHEST_BLOCK_OK, &s), "not READY, nothing pending");
}

static void test_chest_confirm_rule_v2(void)
{
    chest_status_t a, b;
    chest_proto_parse(V1, 20, &a);                                   /* op 1, inst 3, count 42 */
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
    TEST_SUITE("chest link protocol (S3 master)");
    TEST_RUN(test_chest_crc_check_value);
    TEST_RUN(test_chest_v2_vectors);
    TEST_RUN(test_chest_v1_chest_refused);
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
