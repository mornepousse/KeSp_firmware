/* When must the right half transmit? (pure logic)
 *
 * Two rules written separately on 2026-09-05 contradicted each other:
 *
 *   - the right half transmits ON MATRIX CHANGE, never periodically. That is
 *     premise §2.3 of the design and the only one that makes the R1 bet
 *     tenable: at rest, a keyboard produces no traffic.
 *   - the left half RELEASES remote keys after 250 ms of silence, so that a
 *     dead half does not leave "Shift" held on the host.
 *
 * Holding a key for more than 250 ms produces no change, hence no
 * frame, hence the left half concludes silence and releases a key that is
 * yet physically held. No repeat, and the right half's modifiers
 * let go mid-typing.
 *
 * The correct rule distinguishes REST from INACTIVITY: silence when nothing
 * is held, refresh while something is. Rest stays silent — R1 holds — but a
 * held key is reaffirmed before the left half can doubt it.
 */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"

#define PERIODE 100u

static void test_un_changement_emet_toujours(void)
{
    TEST_ASSERT(half_tx_doit_emettre(true, false, 1000, 1000, PERIODE),
                "a release transmits, even if nothing remains held");
    TEST_ASSERT(half_tx_doit_emettre(true, true, 1000, 999, PERIODE),
                "a press transmits without waiting for the period");
}

static void test_le_repos_est_muet(void)
{
    /* THE test of this file for R1: nothing held, nothing to say, even after
     * an eternity. This is what makes the link free at rest. */
    TEST_ASSERT(!half_tx_doit_emettre(false, false, 1000, 900, PERIODE),
                "nothing held: no transmission");
    TEST_ASSERT(!half_tx_doit_emettre(false, false, 999999, 0, PERIODE),
                "and still nothing, no matter how much time has passed");
}

static void test_un_maintien_est_rafraichi(void)
{
    /* THE test of this file for reliability. */
    TEST_ASSERT(!half_tx_doit_emettre(false, true, 1050, 1000, PERIODE),
                "50 ms later: too soon, the left half has no doubt yet");
    TEST_ASSERT(half_tx_doit_emettre(false, true, 1100, 1000, PERIODE),
                "100 ms later: the hold is reaffirmed");
    TEST_ASSERT(half_tx_doit_emettre(false, true, 5000, 1000, PERIODE),
                "and long after, all the more so");
}

static void test_la_periode_tient_sous_le_delai_de_la_gauche(void)
{
    /* Design constraint, not a matter of taste.
     *
     * The margin was TWO refreshes (100 ms against 250 ms), so a
     * single spare packet. Insufficient, and not just by bad luck: the
     * defect is self-sustaining. When the silence expires, the left half
     * releases the remote keys, which changes the HID report, which triggers
     * a transmit to the dongle and its bounded retransmissions — so many
     * PRX->PTX->PRX excursions during which it is DEAF. The right half's next
     * refresh falls into that hole, the silence expires again, and
     * the loop closes.
     *
     * Found on the bench on 2026-09-08: a long hold on Backspace ended
     * up releasing itself, even though the link was only losing one packet in
     * 960. It was not the link quality, it was the margin.
     *
     * FOUR refreshes: it now takes four CONSECUTIVE losses
     * to wrongly release, which is outside the range of an ordinary accident. */
    TEST_ASSERT(HALF_TX_REFRESH_MS * 4 <= HALF_LINK_TIMEOUT_MS,
                "four refreshes fit within the release delay");
}

static void test_le_compteur_de_ms_peut_deborder(void)
{
    /* esp_timer_get_time()/1000 truncated to 32 bits overflows after ~49 days.
     * The difference must be computed in unsigned modular arithmetic, otherwise
     * the keyboard stops transmitting its holds that day. */
    uint32_t avant = 0xFFFFFFF0u;      /* just before the overflow */
    uint32_t apres = 0x00000060u;      /* 112 ms later, after wraparound */
    TEST_ASSERT(half_tx_doit_emettre(false, true, apres, avant, PERIODE),
                "the counter overflow does not freeze transmission");
}

void test_half_tx_cadence(void)
{
    printf("\n-- right half transmit cadence --\n");
    test_un_changement_emet_toujours();
    test_le_repos_est_muet();
    test_un_maintien_est_rafraichi();
    test_la_periode_tient_sous_le_delai_de_la_gauche();
    test_le_compteur_de_ms_peut_deborder();
}
