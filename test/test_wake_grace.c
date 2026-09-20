/* Grace left to the matrix driver after wake — pure logic.
 *
 * Wake reconciliation concludes "key released before the first
 * scan" if the recreated driver has not signaled anything within the grace.
 * The driver only signals after its debounce (debounce_ticks x interval). A
 * grace shorter than the debounce wrongly releases a HELD key (Super+F lost
 * on the bench, 2026-09-13). The formula must therefore ALWAYS cover the
 * debounce, never go below the old value (10 ms), and stay bounded (a
 * silent driver must not delay the wake indefinitely).
 */
#include "test_framework.h"
#include "../main/input/wake_grace.h"

static void test_couvre_l_anti_rebond(void)
{
    /* Niphargus: 3 scans x 1 ms. Debounce + 2 scans = 5 ms, + margin. */
    uint32_t g = wake_grace_ms(3, 1000);
    TEST_ASSERT(g >= 5, "covers 3 scans + 2 startup ones at 1 ms");
    TEST_ASSERT_EQ(g, 15, "Niphargus: 5 ms of debounce + 10 ms of margin");
    /* KaSe V1: 5 scans x 1 ms. */
    TEST_ASSERT(wake_grace_ms(5, 1000) >= 7, "covers 5 scans + 2");
    /* A longer debounce can NEVER give a shorter grace. */
    for (uint32_t d = 0; d < 20; d++)
        TEST_ASSERT(wake_grace_ms(d + 1, 1000) >= wake_grace_ms(d, 1000),
                    "monotone in debounce_ticks");
}

static void test_plancher_10_ms(void)
{
    /* Never less than the old value, even for an instantaneous driver. */
    TEST_ASSERT_EQ(wake_grace_ms(0, 0), 10, "10 ms floor");
    TEST_ASSERT(wake_grace_ms(1, 100) >= 10, ">= 10 ms for 100 us x 3");
}

static void test_plafond_50_ms(void)
{
    /* A silent driver or an absurd config does not block the wake. */
    TEST_ASSERT_EQ(wake_grace_ms(40, 1000), 50, "50 ms ceiling");
    TEST_ASSERT_EQ(wake_grace_ms(3, 100000), 50, "ceiling even with a huge interval");
}

void test_wake_grace(void)
{
    TEST_SUITE("Driver grace on wake");
    test_couvre_l_anti_rebond();
    test_plancher_10_ms();
    test_plafond_50_ms();
}
