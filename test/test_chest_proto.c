/* The chest link, byte for byte — Niphar_chest docs/LINK_CONTRACT.md §10.
 * V1-V9 were produced by the chest's own link_proto.c and are pinned in its
 * test/test_link_proto.c too: a change on either side that the other did not
 * follow breaks a fast check, not the bench. Copied verbatim, never edited. */
#include "test_framework.h"
#include "../main/comm/chest/chest_proto.h"
#include "../main/security/cr_crc16.h"

static const uint8_t V1[20]  = { 0x4E,0x49,0x50,0x48,0x01,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0xAF,0xEA,0x00,0x00,0x00,0x00,0x00,0x00 };
static const uint8_t V2[20]  = { 0 };
static const uint8_t V3[20]  = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const uint8_t V4[20]  = { 0x4E,0x49,0x50,0x58,0x01,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0xAF,0xEA,0x00,0x00,0x00,0x00,0x00,0x00 };
static const uint8_t V5[20]  = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x7F,0x60,0x00,0x00,0x00,0x00,0x00,0x00 };
static const uint8_t V6[20]  = { 0x4E,0x49,0x50,0x48,0x01,0x07,0x01,0x00,0x2B,0x00,0x00,0x00,0xAF,0xEA,0x00,0x00,0x00,0x00,0x00,0x00 };
static const uint8_t V6b[20] = { 0x4E,0x49,0x50,0x48,0x01,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0xAE,0xEA,0x00,0x00,0x00,0x00,0x00,0x00 };
static const uint8_t V8[20]  = { 0x4E,0x49,0x50,0x48,0x01,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0xAF,0xEA,0x00,0x00,0x5A,0x00,0x00,0x00 };
static const uint8_t V9[20]  = { 0x4E,0x49,0x50,0x48,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x61,0x7A,0x00,0x00,0x00,0x00,0x00,0x00 };

/* Contract §1: compare check values, not names (the chest's header calls it X-25; it is MCRF4XX). */
static void test_chest_crc_check_value(void)
{
    TEST_ASSERT_EQ(cr_crc16((const uint8_t *)"123456789", 9), 0x6F91, "CRC-16/MCRF4XX check value (X-25 would be 0x906E)");
}

static void test_chest_contract_vectors(void)
{
    chest_status_t s;
    TEST_ASSERT_EQ(chest_proto_parse(V1, 20, &s), CHEST_BLOCK_OK, "V1 nominal");
    TEST_ASSERT_EQ(s.version, 1, "V1 version 1");
    TEST_ASSERT_EQ(s.state, 0x07, "V1 state SD|USB|READY");
    TEST_ASSERT_EQ(s.pending_op, 1, "V1 PSO:CDS pending");
    TEST_ASSERT_EQ(s.confirm_count, 42, "V1 count 42");
    TEST_ASSERT(chest_proto_is_absent(V2, 20), "V2 absent (0x00)");
    TEST_ASSERT_EQ(chest_proto_parse(V2, 20, &s), CHEST_BLOCK_ABSENT, "V2 parses as absent, not invalid");
    TEST_ASSERT(chest_proto_is_absent(V3, 20), "V3 absent (0xFF)");
    TEST_ASSERT_EQ(chest_proto_parse(V3, 20, &s), CHEST_BLOCK_ABSENT, "V3 parses as absent");
    TEST_ASSERT(!chest_proto_is_absent(V1, 20), "V1 is not absent");
    TEST_ASSERT_EQ(chest_proto_parse(V4, 20, &s), CHEST_BLOCK_CORRUPT, "V4 bad magic");
    TEST_ASSERT_EQ(chest_proto_parse(V5, 20, &s), CHEST_BLOCK_BAD_VERSION, "V5 version 2, correct CRC: refused");
    TEST_ASSERT_EQ(chest_proto_parse(V6, 20, &s), CHEST_BLOCK_CORRUPT, "V6 payload bit flipped");
    TEST_ASSERT_EQ(chest_proto_parse(V6b, 20, &s), CHEST_BLOCK_CORRUPT, "V6b CRC bit flipped");
    TEST_ASSERT_EQ(chest_proto_parse(V1, 19, &s), CHEST_BLOCK_CORRUPT, "V7 truncated to 19 bytes");
    TEST_ASSERT_EQ(chest_proto_parse(V8, 20, &s), CHEST_BLOCK_OK, "V8 confirmation in flight: CRC spans 12 bytes only");
    TEST_ASSERT_EQ(s.pending_op, 1, "V8 decodes like V1");
    TEST_ASSERT_EQ(chest_proto_parse(V9, 20, &s), CHEST_BLOCK_OK, "V9 present, booting");
    TEST_ASSERT_EQ(s.state, 0x00, "V9 not READY");
}

static void test_chest_noise_is_not_a_chest(void)
{
    /* Review focus: a booting/hung chest or a degrading bus returns noise. */
    uint8_t b[20]; chest_status_t s = { .pending_op = 0xBEEF };
    for (int i = 0; i < 20; i++) b[i] = (uint8_t)(0x31 * i + 7);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "noise is not a chest");
    TEST_ASSERT_EQ(chest_proto_parse(V5, 20, &s), CHEST_BLOCK_BAD_VERSION, "and V5 neither");
    TEST_ASSERT_EQ(s.pending_op, 0xBEEF, "out untouched on any failure");
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
    chest_op_label(42, l);
    TEST_ASSERT(strcmp(l, "OP 42") == 0, "an unknown op still prompts, with its code");
}

static void test_chest_confirm_rule(void)
{
    chest_confirm_t c = {0};
    TEST_ASSERT(!chest_confirm_request(&c, 0, 3, 1000), "nothing pending: press dropped");
    TEST_ASSERT(!chest_confirm_step(&c, 0, 3, 1000), "and nothing written");

    TEST_ASSERT(chest_confirm_request(&c, 1, 3, 1000), "pending SIGN: accepted");
    TEST_ASSERT(chest_confirm_step(&c, 1, 3, 1000), "first write at once");
    TEST_ASSERT(!chest_confirm_step(&c, 1, 3, 1100), "no retry before 200 ms");
    TEST_ASSERT(chest_confirm_step(&c, 1, 3, 1200), "one retry after 200 ms, count unchanged");
    TEST_ASSERT(!chest_confirm_step(&c, 1, 3, 1400), "never a third write");
    TEST_ASSERT(!c.armed, "given up after the retry");

    chest_confirm_request(&c, 1, 3, 2000);
    TEST_ASSERT(chest_confirm_step(&c, 1, 3, 2000), "write");
    TEST_ASSERT(!chest_confirm_step(&c, 1, 4, 2050), "count moved: delivered, done");
    TEST_ASSERT(!c.armed, "disarmed on delivery");

    /* Review focus: op A timed out, op B armed before the retry — B must not be authorized. */
    chest_confirm_request(&c, 1, 3, 3000);
    TEST_ASSERT(chest_confirm_step(&c, 1, 3, 3000), "write for SIGN");
    TEST_ASSERT(!chest_confirm_step(&c, 2, 3, 3250), "op changed: no retry for DECRYPT");
    TEST_ASSERT(!c.armed, "disarmed on op change");
}

void test_chest_proto(void)
{
    TEST_SUITE("chest link protocol (S3 master)");
    TEST_RUN(test_chest_crc_check_value);
    TEST_RUN(test_chest_contract_vectors);
    TEST_RUN(test_chest_noise_is_not_a_chest);
    TEST_RUN(test_chest_op_labels);
    TEST_RUN(test_chest_confirm_rule);
}
