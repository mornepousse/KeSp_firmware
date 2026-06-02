#include "test_framework.h"
#include "../main/comm/rf/half_power.h"

void test_half_power(void)
{
    printf("\n--- half_power ---\n");

    /* half_power_next: bornes de T_THROTTLE (3000) et T_SLEEP (15000) */
    TEST_ASSERT_EQ(half_power_next(0, 0),     HALF_POWER_ACTIVE,   "idle 0 -> ACTIVE");
    TEST_ASSERT_EQ(half_power_next(0, 2999),  HALF_POWER_ACTIVE,   "idle 2999 -> ACTIVE");
    TEST_ASSERT_EQ(half_power_next(0, 3000),  HALF_POWER_THROTTLE, "idle 3000 -> THROTTLE");
    TEST_ASSERT_EQ(half_power_next(0, 14999), HALF_POWER_THROTTLE, "idle 14999 -> THROTTLE");
    TEST_ASSERT_EQ(half_power_next(0, 15000), HALF_POWER_SLEEP,    "idle 15000 -> SLEEP");
    TEST_ASSERT_EQ(half_power_next(0, 99999), HALF_POWER_SLEEP,    "idle large -> SLEEP");

    /* activité récente avec base non nulle (now > last) */
    TEST_ASSERT_EQ(half_power_next(100000, 100500), HALF_POWER_ACTIVE,
                   "activite il y a 500ms -> ACTIVE");
    TEST_ASSERT_EQ(half_power_next(100000, 103000), HALF_POWER_THROTTLE,
                   "activite il y a 3s -> THROTTLE (base non-nulle)");
    TEST_ASSERT_EQ(half_power_next(100000, 135000), HALF_POWER_SLEEP,
                   "activite il y a 35s -> SLEEP");

    /* wrap uint32 : last=0xFFFFF448, now=2000 -> idle=(2000-0xFFFFF448) mod 2^32
     * = 5000 ms -> THROTTLE. Prouve que la soustraction non-signee gere le wrap. */
    TEST_ASSERT_EQ(half_power_next(0xFFFFF448u, 2000u), HALF_POWER_THROTTLE,
                   "wrap uint32: idle=5000ms -> THROTTLE");

    /* half_power_hb_divisor */
    TEST_ASSERT_EQ(half_power_hb_divisor(HALF_POWER_ACTIVE),   1,  "divisor ACTIVE = 1");
    TEST_ASSERT_EQ(half_power_hb_divisor(HALF_POWER_THROTTLE), 5,  "divisor THROTTLE = 5");
    TEST_ASSERT_EQ(half_power_hb_divisor(HALF_POWER_SLEEP),    10, "divisor SLEEP = 10");
    TEST_ASSERT_EQ(half_power_hb_divisor((half_power_state_t)99), 1,
                   "divisor etat hors-borne -> 1 (default)");
}
