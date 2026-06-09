#include "test_framework.h"
#include "sec_confirm.h"

static void test_arm_authorize_consume(void)
{
    sec_confirm_reset();
    sec_confirm_arm(2, 1000);
    TEST_ASSERT_EQ(sec_confirm_poll(1000, NULL), SEC_CONFIRM_PENDING, "armed -> PENDING");
    sec_confirm_authorize();
    uint8_t slot = 0xFF;
    TEST_ASSERT_EQ(sec_confirm_poll(1100, &slot), SEC_CONFIRM_AUTHORIZED, "authorized");
    TEST_ASSERT_EQ(slot, 2, "slot preserved");
    TEST_ASSERT_EQ(sec_confirm_poll(1200, NULL), SEC_CONFIRM_IDLE, "consumed -> IDLE");
}

static void test_timeout(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0, 1000);
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 14999, NULL), SEC_CONFIRM_PENDING, "before timeout");
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 15000, NULL), SEC_CONFIRM_TIMEDOUT, "at timeout");
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 16000, NULL), SEC_CONFIRM_IDLE, "after timeout -> IDLE");
}

static void test_authorize_without_arm(void)
{
    sec_confirm_reset();
    sec_confirm_authorize();
    TEST_ASSERT_EQ(sec_confirm_poll(0, NULL), SEC_CONFIRM_IDLE, "authorize w/o arm = no-op");
}

static void test_rearm_overwrites_slot(void)
{
    sec_confirm_reset();
    sec_confirm_arm(1, 1000);
    sec_confirm_arm(3, 1050);
    sec_confirm_authorize();
    uint8_t slot = 0xFF;
    sec_confirm_poll(1060, &slot);
    TEST_ASSERT_EQ(slot, 3, "re-arm overwrites slot");
}

void test_sec_confirm(void)
{
    TEST_SUITE("sec_confirm state machine");
    TEST_RUN(test_arm_authorize_consume);
    TEST_RUN(test_timeout);
    TEST_RUN(test_authorize_without_arm);
    TEST_RUN(test_rearm_overwrites_slot);
}
