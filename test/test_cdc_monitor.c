#include "test_framework.h"
#include <limits.h>
#include "../main/comm/cdc/ks_monitor.h"

void test_cdc_monitor(void)
{
    printf("\n--- cdc_monitor ---\n");

    ks_monitor_t m = {
        .fmt = KS_MONITOR_FMT,
        .flags = KS_MON_F_HAS_RF | KS_MON_F_LINK_L | KS_MON_F_USB,
        .uptime_s = 0x01020304u,
        .heap_free_kb = 0xABCD,
        .temp_c = 42,
        .layer_idx = 3,
        .wpm = 77,
        .keys_total = 0x11223344u,
        .sig_left = 250, .sig_right = 200,
        .hb_age_l_ms = 0x0102, .hb_age_r_ms = 0x0304,
        .batt_l_dv = 41, .batt_l_soc = 92, .batt_l_chg = 1,
        .batt_r_dv = 38, .batt_r_soc = 60, .batt_r_chg = 0,
        .bt_slot = 2,
    };
    uint8_t b[KS_MONITOR_SIZE];
    uint16_t n = ks_monitor_encode(b, &m);

    TEST_ASSERT_EQ(n, KS_MONITOR_SIZE, "encode returns 28");
    TEST_ASSERT_EQ(b[0], KS_MONITOR_FMT, "off0 fmt");
    TEST_ASSERT_EQ(b[1], (KS_MON_F_HAS_RF|KS_MON_F_LINK_L|KS_MON_F_USB), "off1 flags");
    TEST_ASSERT_EQ(b[2], 0x04, "uptime LE b0"); TEST_ASSERT_EQ(b[3], 0x03, "uptime LE b1");
    TEST_ASSERT_EQ(b[4], 0x02, "uptime LE b2"); TEST_ASSERT_EQ(b[5], 0x01, "uptime LE b3");
    TEST_ASSERT_EQ(b[6], 0xCD, "heap LE lo"); TEST_ASSERT_EQ(b[7], 0xAB, "heap LE hi");
    TEST_ASSERT_EQ((int8_t)b[8], 42, "off8 temp");
    TEST_ASSERT_EQ(b[9], 3,  "off9 layer");
    TEST_ASSERT_EQ(b[10], 77, "off10 wpm");
    TEST_ASSERT_EQ(b[11], 0x44, "keys LE b0"); TEST_ASSERT_EQ(b[12], 0x33, "keys LE b1");
    TEST_ASSERT_EQ(b[13], 0x22, "keys LE b2"); TEST_ASSERT_EQ(b[14], 0x11, "keys LE b3");
    TEST_ASSERT_EQ(b[15], 250, "off15 sig_l"); TEST_ASSERT_EQ(b[16], 200, "off16 sig_r");
    TEST_ASSERT_EQ(b[17], 0x02, "hbL lo"); TEST_ASSERT_EQ(b[18], 0x01, "hbL hi");
    TEST_ASSERT_EQ(b[19], 0x04, "hbR lo"); TEST_ASSERT_EQ(b[20], 0x03, "hbR hi");
    TEST_ASSERT_EQ(b[21], 41, "batt_l_dv");  TEST_ASSERT_EQ(b[22], 92, "batt_l_soc"); TEST_ASSERT_EQ(b[23], 1, "batt_l_chg");
    TEST_ASSERT_EQ(b[24], 38, "batt_r_dv");  TEST_ASSERT_EQ(b[25], 60, "batt_r_soc"); TEST_ASSERT_EQ(b[26], 0, "batt_r_chg");
    TEST_ASSERT_EQ(b[27], 2, "off27 bt_slot");

    m.temp_c = INT8_MIN;
    ks_monitor_encode(b, &m);
    TEST_ASSERT_EQ((int8_t)b[8], INT8_MIN, "temp sentinel -128");
}
