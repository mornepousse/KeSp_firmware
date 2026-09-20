/* Cadence of the keyboard -> dongle supervision frame (pure logic).
 *
 * The dongle declares a slot LOST after RF_LINK_LOST_MS of total silence and
 * applies its fallback — it releases the keys, so that a vanished keyboard
 * doesn't leave a stuck key on the host. It therefore expects an idle status
 * frame; its own comment says so ("well above the cadence of the idle
 * status frame").
 *
 * But PKT_TYPE_STATUS was NEVER sent: it only existed on the decode side.
 * Nothing sent it from the keyboard.
 *
 * As long as you're typing, HID reports keep the link alive by accident. But
 * a HELD key produces no change, hence no more reports: the silence exceeds
 * RF_LINK_LOST_MS and the dongle releases the key. Observed on the bench on
 * 2026-09-08 — a held Backspace came back up on its own after about two
 * seconds, and the dongle's constant is 2000/2500 ms.
 *
 * This is the SAME flaw as the right half's, one link further down the
 * chain: "emit on change" and "release on silence" do not compose,
 * whichever link is involved. */
#include "test_framework.h"
#include "../main/comm/rf/rf_slot.h"

static void test_rien_avant_la_periode(void)
{
    TEST_ASSERT(!rf_status_doit_emettre(1500, 1000, RF_STATUS_PERIOD_MS),
                "500 ms later: too early");
    TEST_ASSERT(!rf_status_doit_emettre(1999, 1000, RF_STATUS_PERIOD_MS),
                "999 ms later: still too early");
}

static void test_emission_a_la_periode(void)
{
    TEST_ASSERT(rf_status_doit_emettre(2000, 1000, RF_STATUS_PERIOD_MS),
                "one full period: we emit");
    TEST_ASSERT(rf_status_doit_emettre(9999, 1000, RF_STATUS_PERIOD_MS),
                "and even more so well after");
}

static void test_la_marge_couvre_une_perte(void)
{
    /* THE test of this file. A single lost frame must not be enough to
     * declare the link dead: at least two consecutive ones are needed. */
    TEST_ASSERT(RF_STATUS_PERIOD_MS * 2 < RF_LINK_LOST_MS,
                "two status frames fit within the dongle's silence budget");
}

static void test_le_compteur_de_ms_peut_deborder(void)
{
    /* esp_timer_get_time()/1000 truncated to 32 bits overflows after 49 days: a
     * signed subtraction would freeze supervision that day, and the dongle
     * would release keys in a loop. */
    /* last = 0xFFFFFF00, now = 0x000003B0: 1200 ms have elapsed ACROSS
     * the wraparound. The unsigned subtraction must yield 1200, not a huge
     * or negative value. */
    TEST_ASSERT(rf_status_doit_emettre(0x000003B0u, 0xFFFFFF00u, RF_STATUS_PERIOD_MS),
                "1200 ms across the wraparound: we emit");
    /* And the wraparound must also not make it emit TOO EARLY: 512 ms. */
    TEST_ASSERT(!rf_status_doit_emettre(0x00000100u, 0xFFFFFF00u, RF_STATUS_PERIOD_MS),
                "512 ms across the wraparound: still too early");
}

void test_rf_status_cadence(void)
{
    printf("\n-- cadence de la trame de supervision --\n");
    test_rien_avant_la_periode();
    test_emission_a_la_periode();
    test_la_marge_couvre_une_perte();
    test_le_compteur_de_ms_peut_deborder();
}
