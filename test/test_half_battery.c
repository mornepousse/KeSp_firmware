#include "test_framework.h"
#include "../main/comm/rf/half_battery.h"

void test_half_battery(void)
{
    printf("\n--- half_battery ---\n");

    /* batt_dV is voltage x10 (decivolts). 1S Li-ion: 3.0V..4.2V → 0..100%. */

    /* Unknown / no reading */
    TEST_ASSERT_EQ(half_batt_soc_pct(0), 0, "dV=0 (unknown) -> 0%");

    /* Clamp below empty / above full */
    TEST_ASSERT_EQ(half_batt_soc_pct(29), 0,   "dV=2.9V (below empty) -> 0%");
    TEST_ASSERT_EQ(half_batt_soc_pct(30), 0,   "dV=3.0V (empty) -> 0%");
    TEST_ASSERT_EQ(half_batt_soc_pct(42), 100, "dV=4.2V (full) -> 100%");
    TEST_ASSERT_EQ(half_batt_soc_pct(43), 100, "dV=4.3V (over) -> clamp 100%");
    TEST_ASSERT_EQ(half_batt_soc_pct(83), 100, "dV=8.3V (max field) -> clamp 100%");

    /* Exact LUT knee points (1S curve) */
    TEST_ASSERT_EQ(half_batt_soc_pct(34), 8,   "dV=3.4V -> 8%");
    TEST_ASSERT_EQ(half_batt_soc_pct(36), 30,  "dV=3.6V -> 30%");
    TEST_ASSERT_EQ(half_batt_soc_pct(38), 60,  "dV=3.8V -> 60%");
    TEST_ASSERT_EQ(half_batt_soc_pct(40), 85,  "dV=4.0V -> 85%");

    /* Linear interpolation between knees: 4.1V is halfway 4.0(85)→4.2(100) */
    TEST_ASSERT_EQ(half_batt_soc_pct(41), 92,  "dV=4.1V -> 92% (interp 85..100)");
    /* 3.7V halfway 3.6(30)→3.8(60) = 45 */
    TEST_ASSERT_EQ(half_batt_soc_pct(37), 45,  "dV=3.7V -> 45% (interp 30..60)");

    /* Monotonic non-decreasing across the range */
    int prev = -1, mono = 1;
    for (uint8_t dv = 30; dv <= 42; dv++) {
        int s = half_batt_soc_pct(dv);
        if (s < prev) mono = 0;
        prev = s;
    }
    TEST_ASSERT(mono, "SoC monotonic non-decreasing 3.0V..4.2V");
}
