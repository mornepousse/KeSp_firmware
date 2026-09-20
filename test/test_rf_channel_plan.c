/* Project 2.4 GHz channel plan (pure logic).
 *
 * Four radio links coexist, and until now nothing stopped them from landing
 * on the same channel: each was defined in its own corner — rf_pairing.h for
 * pairing, the board.h files for the links to the dongle. A collision breaks
 * no compilation; it shows up as a link that goes silent, or worse as packets
 * that get through intermittently depending on traffic.
 *
 * This test gathers all four and locks in their separation. The next person
 * who adds a link hits a red rather than a silence.
 *
 * Spacing rule, nRF24L01+ Product Specification §6.3 p.25: "The channel
 * occupies a bandwidth of less than 1MHz at 250kbps and 1Mbps". The driver
 * transmits at 1 Mbps (RF_SETUP = 0x06), so 1 MHz of separation is enough.
 * The 2 MHz constraint only applies at 2 Mbps.
 */
#include "test_framework.h"
#include "../main/comm/rf/rf_slot.h"
#include "../main/comm/rf/rf_pairing.h"

/* 2.4 GHz ISM band: 2400 to 2483.5 MHz. The chip could go up to 2525 (§6.3)
 * but transmitting beyond that leaves the band, which is not just a paperwork
 * matter: it jams other services there. */
#define ISM_CH_MAX  83   /* 2483 MHz */

static void test_les_canaux_sont_dans_la_bande(void)
{
    const int ch[] = { RF_PAIR_CHANNEL, RF_CH_KBD_DONGLE,
                       RF_CH_MOUSE_DONGLE, RF_CH_HALF_LINK };
    const int n = (int)(sizeof(ch) / sizeof(ch[0]));
    for (int i = 0; i < n; i++) {
        TEST_ASSERT(ch[i] >= 0, "channel >= 2400 MHz");
        TEST_ASSERT(ch[i] <= ISM_CH_MAX, "channel within ISM band (<= 2483 MHz)");
    }
}

static void test_aucune_collision_ni_recouvrement(void)
{
    /* THE test of this file. Two links on the same channel, or less than 1 MHz
     * apart, jam each other — and the symptom is a link going silent, not an error. */
    const int ch[] = { RF_PAIR_CHANNEL, RF_CH_KBD_DONGLE,
                       RF_CH_MOUSE_DONGLE, RF_CH_HALF_LINK };
    const int n = (int)(sizeof(ch) / sizeof(ch[0]));
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            int d = ch[i] > ch[j] ? ch[i] - ch[j] : ch[j] - ch[i];
            TEST_ASSERT(d >= 1, "two links spaced by at least 1 MHz (1 Mbps)");
        }
}

static void test_le_lien_inter_moities_est_hors_wifi(void)
{
    /* 2.4 GHz WiFi occupies at most up to ~2473 MHz (channel 11). Above that the
     * band is markedly quieter — which is why the dongle's links are already
     * there. The inter-half link joins them rather than going back down. */
    TEST_ASSERT(RF_CH_HALF_LINK > 73, "above WiFi channel 11 (~2473 MHz)");
    TEST_ASSERT(RF_CH_HALF_LINK < RF_CH_MOUSE_DONGLE, "below the mouse link");
    TEST_ASSERT(RF_CH_HALF_LINK > RF_CH_KBD_DONGLE, "above the keyboard link");
}

static void test_le_suffixe_suit_la_convention(void)
{
    /* 0x01 keyboard, 0x02 mouse (rf_slot.h) — 0x03 for the inter-half link. */
    TEST_ASSERT_EQ(RF_ADDR_HALF_LINK, 0x03, "inter-half link suffix");
}

void test_rf_channel_plan(void)
{
    printf("\n-- 2.4 GHz channel plan --\n");
    test_les_canaux_sont_dans_la_bande();
    test_aucune_collision_ni_recouvrement();
    test_le_lien_inter_moities_est_hors_wifi();
    test_le_suffixe_suit_la_convention();
}
