/* Bounded repair after a change, on the RIGHT half side — pure logic.
 *
 * A change frame rejected by the ESB (~1% at the bench, after 15 hardware
 * retransmissions) only had one chance: the hold rule (half_tx_doit_emettre)
 * only replays the state for a HELD key, never for a brief press. A brief
 * press whose frame was rejected was lost forever (bench 2026-09-13, same
 * cause as the left).
 *
 * Remedy: a change ARMS a bounded number of repeats, consumed one per tick
 * even if nothing is held; then, back to the hold rule. A state never armed
 * at rest stays mute — that's the §2.3 premise (bet R1). */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"

#define P 100u   /* period for reaffirming holds */

static void test_changement_arme_des_repetitions(void)
{
    half_tx_repeat_t r = {0};
    TEST_ASSERT(half_tx_doit_emettre_repare(&r, true, false, 1000, 1000, P),
                "the change emits, even if nothing is left held");
    for (unsigned i = 0; i < HALF_TX_REPEATS; i++)
        TEST_ASSERT(half_tx_doit_emettre_repare(&r, false, false, 1020 + 20 * i, 1000 + 20 * i, P),
                    "repeat without a hold: the lost frame gets a second chance");
    TEST_ASSERT(!half_tx_doit_emettre_repare(&r, false, false, 1300, 1280, P),
                "repeats exhausted, nothing held: silence");
}

static void test_repos_jamais_arme_reste_muet(void)
{
    /* THE test on the R1 side: a fresh state never produces anything. */
    half_tx_repeat_t r = {0};
    for (int i = 0; i < 200; i++)
        TEST_ASSERT(!half_tx_doit_emettre_repare(&r, false, false, 1000 + 20 * i, 1000, P),
                    "never armed: mute");
}

static void test_maintien_toujours_rafraichi_apres_les_repetitions(void)
{
    half_tx_repeat_t r = {0};
    half_tx_doit_emettre_repare(&r, true, true, 1000, 1000, P);                 /* arms */
    for (unsigned i = 0; i < HALF_TX_REPEATS; i++)
        half_tx_doit_emettre_repare(&r, false, true, 1000, 1000, P);            /* consumes */
    TEST_ASSERT(!half_tx_doit_emettre_repare(&r, false, true, 1050, 1000, P),
                "held, 50 ms: too early for the reaffirmation");
    TEST_ASSERT(half_tx_doit_emettre_repare(&r, false, true, 1100, 1000, P),
                "held, 100 ms: reaffirmed — the hold rule is intact");
}

static void test_un_nouveau_changement_rearme(void)
{
    half_tx_repeat_t r = {0};
    half_tx_doit_emettre_repare(&r, true, true, 1000, 1000, P);
    half_tx_doit_emettre_repare(&r, false, true, 1020, 1000, P);                /* 1 consumed */
    TEST_ASSERT(half_tx_doit_emettre_repare(&r, true, false, 1030, 1020, P),
                "a release (change) emits and re-arms");
    for (unsigned i = 0; i < HALF_TX_REPEATS; i++)
        TEST_ASSERT(half_tx_doit_emettre_repare(&r, false, false, 1050 + 20 * i, 1030, P),
                    "FULL set of repeats after re-arming");
    TEST_ASSERT(!half_tx_doit_emettre_repare(&r, false, false, 1400, 1380, P), "then silence");
}

void test_half_tx_repeat(void)
{
    TEST_SUITE("Bounded repair after a change (right)");
    test_changement_arme_des_repetitions();
    test_repos_jamais_arme_reste_muet();
    test_maintien_toujours_rafraichi_apres_les_repetitions();
    test_un_nouveau_changement_rearme();
}
