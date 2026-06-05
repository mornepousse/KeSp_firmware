#include "test_framework.h"
#include "../main/periph/trackpad/trackpad.h"

void test_trackpad_cfg(void)
{
    printf("\n--- trackpad_cfg ---\n");
    trackpad_cfg_t c = { .fmt = TRACKPAD_CFG_FMT, .base = 90, .accel = 40, .gain_max = 300 };
    uint8_t b[TRACKPAD_CFG_SIZE];
    uint16_t n = trackpad_cfg_encode(b, &c);
    TEST_ASSERT_EQ(n, TRACKPAD_CFG_SIZE, "cfg encode size 7");
    TEST_ASSERT_EQ(b[0], TRACKPAD_CFG_FMT, "fmt");
    TEST_ASSERT_EQ(b[1], 90, "base lo");  TEST_ASSERT_EQ(b[2], 0, "base hi");
    TEST_ASSERT_EQ(b[3], 40, "accel lo"); TEST_ASSERT_EQ(b[4], 0, "accel hi");
    TEST_ASSERT_EQ(b[5], 0x2C, "gmax lo (300)"); TEST_ASSERT_EQ(b[6], 1, "gmax hi");
    trackpad_cfg_t d;
    TEST_ASSERT(trackpad_cfg_decode(b, n, &d), "decode 7B ok");
    TEST_ASSERT_EQ(d.base, 90, "rt base"); TEST_ASSERT_EQ(d.accel, 40, "rt accel"); TEST_ASSERT_EQ(d.gain_max, 300, "rt gmax");
    uint8_t setp[6] = { 100,0, 50,0, 0x2C,1 };   /* SET payload, no fmt */
    TEST_ASSERT(trackpad_cfg_decode(setp, 6, &d), "decode 6B ok");
    TEST_ASSERT_EQ(d.base, 100, "6B base"); TEST_ASSERT_EQ(d.gain_max, 300, "6B gmax");
}
