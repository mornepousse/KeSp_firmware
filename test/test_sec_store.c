#define TEST_HOST 1
#include "test_framework.h"
#include "sec_store.h"

static void test_set_get(void)
{
    sec_store_init();
    uint8_t key[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
    TEST_ASSERT(sec_store_set_slot(1, SEC_SLOT_HMAC_SHA1, "github", key, 20), "set ok");
    TEST_ASSERT_EQ(sec_store_type(1), SEC_SLOT_HMAC_SHA1, "type stored");
    TEST_ASSERT(strcmp(sec_store_label(1), "github") == 0, "label stored");
    uint8_t out[64]; uint8_t olen = 0;
    TEST_ASSERT(sec_store_get_secret(1, out, &olen), "get secret ok");
    TEST_ASSERT_EQ(olen, 20, "secret_len");
    TEST_ASSERT(memcmp(out, key, 20) == 0, "secret bytes match");
    TEST_ASSERT_EQ(sec_store_count(), 1, "count = 1");
}

static void test_clear(void)
{
    sec_store_init();
    uint8_t key[4] = {9,9,9,9};
    sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "x", key, 4);
    TEST_ASSERT(sec_store_clear_slot(0), "clear ok");
    TEST_ASSERT_EQ(sec_store_type(0), SEC_SLOT_EMPTY, "slot empty after clear");
    TEST_ASSERT(sec_store_label(0) == NULL, "label NULL after clear");
    TEST_ASSERT_EQ(sec_store_count(), 0, "count = 0");
}

static void test_bounds(void)
{
    sec_store_init();
    uint8_t key[4] = {1,2,3,4};
    TEST_ASSERT(!sec_store_set_slot(SEC_N_SLOTS, SEC_SLOT_HMAC_SHA1, "x", key, 4), "idx oob rejected");
    uint8_t big[65] = {0};
    TEST_ASSERT(!sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "x", big, 65), "secret_len > max rejected");
    TEST_ASSERT(sec_store_get_secret(2, key, NULL) == false, "get empty slot fails");
}

void test_sec_store(void)
{
    TEST_SUITE("sec_store");
    TEST_RUN(test_set_get);
    TEST_RUN(test_clear);
    TEST_RUN(test_bounds);
}
