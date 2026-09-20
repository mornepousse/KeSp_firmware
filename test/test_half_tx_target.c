/* TX target switch of the right half (fusion) — pure logic.
 *
 * The right half talks to the dongle (KaSe.01). Unplug the dongle and type on the
 * left half over USB: the dongle no longer ACKs, and the left-USB — which is
 * nonetheless listening on KaSe.03 — hears nothing since nobody is transmitting
 * there any more. Fallback: after N consecutive sends without ACK, the right half
 * RE-ARMS its chip and SWITCHES to the other listener. The left half is already
 * listening on KaSe.03; the right half then transmits the pre-fusion heartbeat that it decodes.
 *
 * The invariant: only one target at a time (never a double transmission), and
 * an ACK resets the counter to zero (an isolated glitch does not switch).
 *
 * Pure logic, tested on host. Impl: main/comm/rf/half_link.h.
 */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"

static void test_depart_sur_le_dongle(void)
{
    half_tx_fsm_t s = { HALF_TX_TO_DONGLE, 0 };
    TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "at rest the right half targets the dongle");
    /* An ACK never causes a switch, whatever happens. */
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT(!half_tx_target_step(&s, true, HALF_TX_SWITCH_FAILS),
                    "an ACK never re-arms");
        TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "and never changes the target");
    }
}

static void test_seuil_avant_bascule(void)
{
    half_tx_fsm_t s = { HALF_TX_TO_DONGLE, 0 };
    /* threshold-1 failures: no switch yet. */
    for (unsigned i = 0; i < HALF_TX_SWITCH_FAILS - 1u; i++) {
        TEST_ASSERT(!half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                    "before the threshold: no re-arm");
        TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "before the threshold: still the dongle");
    }
    /* The threshold failure: re-arm + switch to the left half. */
    TEST_ASSERT(half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                "at the threshold: re-arm");
    TEST_ASSERT(s.cible == HALF_TX_TO_LEFT, "at the threshold: switch to the left KaSe.03");
}

static void test_un_ack_annule_le_compte(void)
{
    half_tx_fsm_t s = { HALF_TX_TO_DONGLE, 0 };
    /* Almost at the threshold... */
    for (unsigned i = 0; i < HALF_TX_SWITCH_FAILS - 1u; i++)
        half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS);
    /* ...a single ACK and the counter resets to zero: an isolated glitch does not switch. */
    TEST_ASSERT(!half_tx_target_step(&s, true, HALF_TX_SWITCH_FAILS), "ACK: nothing");
    TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "ACK: target unchanged");
    /* The full threshold must be reached again to switch. */
    for (unsigned i = 0; i < HALF_TX_SWITCH_FAILS - 1u; i++)
        TEST_ASSERT(!half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                    "after the ACK, the threshold must be reached again");
    TEST_ASSERT(half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                "full threshold reached: switch");
    TEST_ASSERT(s.cible == HALF_TX_TO_LEFT, "switch to the left half");
}

static void test_rebascule_auto_cicatrisante(void)
{
    /* Dongle absent -> switched to the left half. If the left half in turn
     * stops ACKing (USB unplugged -> it is no longer listening), switch back to the dongle. */
    half_tx_fsm_t s = { HALF_TX_TO_LEFT, 0 };
    for (unsigned i = 0; i < HALF_TX_SWITCH_FAILS - 1u; i++)
        TEST_ASSERT(!half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                    "on the left half, before the threshold: no switch");
    TEST_ASSERT(half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                "threshold on the left half: switch back");
    TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "back to the dongle");
}

/* THE test that guards the anti-oscillation: after a switch, the counter must
 * reset to zero, otherwise the slightest lost packet would make the right
 * half oscillate between the two listeners on every missed frame (instead of
 * every full threshold). This chains along WITHOUT resetting the FSM —
 * unlike the other cases, it is continuity that is being exercised here. */
static void test_le_compteur_repart_apres_bascule(void)
{
    half_tx_fsm_t s = { HALF_TX_TO_DONGLE, 0 };
    for (unsigned i = 0; i < HALF_TX_SWITCH_FAILS - 1u; i++)
        half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS);
    TEST_ASSERT(half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS), "1st switch at the threshold");
    TEST_ASSERT(s.cible == HALF_TX_TO_LEFT, "to the left half");
    /* ONE more failure must NOT switch back: the counter has reset to zero. */
    TEST_ASSERT(!half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                "a single failure after the switch does not switch back");
    TEST_ASSERT(s.cible == HALF_TX_TO_LEFT, "still on the left half");
    for (unsigned i = 1; i < HALF_TX_SWITCH_FAILS - 1u; i++)
        TEST_ASSERT(!half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                    "still not before the full threshold");
    TEST_ASSERT(half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS), "2nd full switch");
    TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "back to the dongle");
}

void test_half_tx_target(void)
{
    TEST_SUITE("Right half TX target switch (fallback without a dongle)");
    test_depart_sur_le_dongle();
    test_seuil_avant_bascule();
    test_un_ack_annule_le_compte();
    test_rebascule_auto_cicatrisante();
    test_le_compteur_repart_apres_bascule();
}
