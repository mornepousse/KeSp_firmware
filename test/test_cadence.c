/* Half cadences: at rest, no periodic wait under 3 ticks
 * (CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP) — otherwise automatic light sleep
 * never happens, with no error. The _Static_assert in cadence.h is the real
 * guard; this test says WHY and checks the active values. */
#include "test_framework.h"
#include "../main/power/cadence.h"
#include "../main/comm/rf/rf_slot.h"   /* RF_STATUS_PERIOD_MS, RF_LINK_LOST_MS */
#include "../main/comm/rf/half_link.h" /* HALF_LINK_TIMEOUT_MS */

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
    TEST_ASSERT(STATUS_DISP_PERIODE_MS >= CADENCE_REPOS_MIN_MS, "status display");
    TEST_ASSERT(CDC_ATTENTE_MAX_MS   >= CADENCE_REPOS_MIN_MS, "CDC safety net");
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

/* The left's STATUS goes out from the relay tick: at rest it leaves at the
 * first tick after RF_STATUS_PERIOD_MS, i.e. up to one rest period late. The
 * dongle declares the keyboard slot lost after RF_LINK_LOST_MS of silence —
 * so a slower rest tick must still fit (2026-09-25: 500 + 1000 < 2500). */
static void test_le_repos_du_relais_tient_la_patience_du_dongle(void)
{
    TEST_ASSERT(KBD_RELAY_REPOS_MS + RF_STATUS_PERIOD_MS < RF_LINK_LOST_MS,
                "relay rest + STATUS period < dongle link-lost timeout");
}

/* A held left key is reaffirmed at the first held tick after
 * KBD_RELAY_REAFFIRM_MS, i.e. up to one held tick late. The dongle releases a
 * mute half after HALF_LINK_TIMEOUT_MS: one reaffirmation must be losable
 * (15 ESB retries exhausted) without the key being released. */
static void test_le_maintien_du_relais_tient_la_patience_du_dongle(void)
{
    TEST_ASSERT(KBD_RELAY_TENU_MS >= CADENCE_REPOS_MIN_MS, "held tick leaves the chip 3 ticks");
    TEST_ASSERT(2u * (KBD_RELAY_REAFFIRM_MS + KBD_RELAY_TENU_MS) <= HALF_LINK_TIMEOUT_MS,
                "one reaffirmation losable before the dongle releases the half");
}

void test_cadence(void)
{
    TEST_SUITE("half cadences (tickless)");
    TEST_RUN(test_le_repos_laisse_trois_ticks);
    TEST_RUN(test_l_actif_reste_reactif);
    TEST_RUN(test_le_repos_du_relais_tient_la_patience_du_dongle);
    TEST_RUN(test_le_maintien_du_relais_tient_la_patience_du_dongle);
}
