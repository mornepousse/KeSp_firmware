/* Half cadences: at rest, no periodic wait under 3 ticks
 * (CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP) — otherwise automatic light sleep
 * never happens, with no error. The _Static_assert in cadence.h is the real
 * guard; this test says WHY and checks the active values. */
#include "test_framework.h"
#include "../main/power/cadence.h"

static void test_le_repos_laisse_trois_ticks(void)
{
    TEST_ASSERT_EQ(CADENCE_REPOS_MIN_MS, 3 * CADENCE_TICK_MS, "3 ticks at 100 Hz");
    TEST_ASSERT(KBD_CADENCE_REPOS_MS >= CADENCE_REPOS_MIN_MS, "keyboard task");
    TEST_ASSERT(KBD_RELAY_REPOS_MS   >= CADENCE_REPOS_MIN_MS, "left relay");
    TEST_ASSERT(HALF_TX_REPOS_MS     >= CADENCE_REPOS_MIN_MS, "right refresh");
    TEST_ASSERT(LINK_REPOS_MS        >= CADENCE_REPOS_MIN_MS, "TRRS link");
    TEST_ASSERT(MEMLCD_DROITE_PERIODE_MS >= CADENCE_REPOS_MIN_MS, "right display");
    TEST_ASSERT(LVGL_TICK_MS         >= CADENCE_REPOS_MIN_MS, "LVGL tick");
    TEST_ASSERT(LVGL_REFR_MS         >= CADENCE_REPOS_MIN_MS, "LVGL refresh");
}

static void test_l_actif_reste_reactif(void)
{
    /* Held key / repair: under 20 ms, otherwise the 100 ms reaffirmations
     * and the bounded repair (5 x 10 ms) lose their meaning. */
    TEST_ASSERT(KBD_CADENCE_ACTIF_MS <= 20, "active keyboard task");
    TEST_ASSERT(KBD_RELAY_REFRESH_MS <= 20, "active relay");
    TEST_ASSERT(HALF_TX_TENU_MS      <= 20, "right held key");
    TEST_ASSERT(LINK_TICK_MS         <= 20, "link handshake");
}

void test_cadence(void)
{
    TEST_SUITE("half cadences (tickless)");
    TEST_RUN(test_le_repos_laisse_trois_ticks);
    TEST_RUN(test_l_actif_reste_reactif);
}
