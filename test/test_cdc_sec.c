#define _GNU_SOURCE
#define TEST_HOST 1
#include "test_framework.h"
#include "sec_store.h"
#include "cdc_sec_cmds.h"
#include <string.h>

static void test_parse_set_slot_valid(void)
{
    uint8_t p[3 + SEC_LABEL_LEN + 20];
    memset(p, 0, sizeof(p));
    p[0] = 2; p[1] = SEC_SLOT_HMAC_SHA1;
    memcpy(&p[2], "github", 6);
    p[2 + SEC_LABEL_LEN] = 20;
    for (int i = 0; i < 20; i++) p[3 + SEC_LABEL_LEN + i] = (uint8_t)i;
    uint8_t idx, type, slen; char label[SEC_LABEL_LEN]; uint8_t secret[SEC_SECRET_MAX];
    bool ok = sec_cmd_parse_set_slot(p, sizeof(p), &idx, &type, label, secret, &slen);
    TEST_ASSERT(ok, "valid SET_SLOT parses");
    TEST_ASSERT_EQ(idx, 2, "idx");
    TEST_ASSERT_EQ(slen, 20, "secret_len");
    TEST_ASSERT(strcmp(label, "github") == 0, "label");
    TEST_ASSERT_EQ(secret[19], 19, "secret bytes");
}

static void test_parse_set_slot_rejects(void)
{
    uint8_t p[3 + SEC_LABEL_LEN + 64];
    memset(p, 0, sizeof(p));
    uint8_t idx, type, slen; char label[SEC_LABEL_LEN]; uint8_t secret[SEC_SECRET_MAX];
    p[0] = SEC_N_SLOTS; p[2 + SEC_LABEL_LEN] = 4;
    TEST_ASSERT(!sec_cmd_parse_set_slot(p, 3 + SEC_LABEL_LEN + 4, &idx, &type, label, secret, &slen), "idx oob rejected");
    p[0] = 0; p[2 + SEC_LABEL_LEN] = 65;
    TEST_ASSERT(!sec_cmd_parse_set_slot(p, sizeof(p), &idx, &type, label, secret, &slen), "secret_len > max rejected");
    TEST_ASSERT(!sec_cmd_parse_set_slot(p, 3, &idx, &type, label, secret, &slen), "truncated payload rejected");
}

static void test_list_has_no_secret(void)
{
    sec_store_init();
    uint8_t key[20]; memset(key, 0xAB, 20);
    sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "acct", key, 20);
    uint8_t buf[128]; uint16_t len = 0;
    sec_cmd_build_list(buf, &len);
    TEST_ASSERT(memmem(buf, len, "acct", 4) != NULL, "label present in LIST");
    uint8_t needle[4] = {0xAB,0xAB,0xAB,0xAB};
    TEST_ASSERT(memmem(buf, len, needle, 4) == NULL, "no secret bytes in LIST (I1)");
}

void test_cdc_sec(void)
{
    TEST_SUITE("cdc_sec provisioning");
    TEST_RUN(test_parse_set_slot_valid);
    TEST_RUN(test_parse_set_slot_rejects);
    TEST_RUN(test_list_has_no_secret);
}
