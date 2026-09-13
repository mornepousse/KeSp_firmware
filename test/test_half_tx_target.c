/* Bascule de cible d'émission de la moitié droite (fusion) — logique pure.
 *
 * La droite parle au dongle (KaSe.01). Débranche le dongle et tape sur la
 * gauche en USB : le dongle n'acquitte plus, et la gauche-USB — qui écoute
 * pourtant KaSe.03 — n'entend rien puisque plus personne n'y réémet. Repli :
 * après N envois consécutifs sans ACK, la droite RÉARME sa puce et BASCULE vers
 * l'autre auditeur. La gauche écoute déjà KaSe.03 ; la droite y émet alors le
 * heartbeat pré-fusion qu'elle décode.
 *
 * L'invariant : une seule cible à la fois (jamais de double frappe), et un ACK
 * remet le compteur à zéro (un glitch isolé ne fait pas basculer).
 *
 * Logique pure, testée host. Impl : main/comm/rf/half_link.h.
 */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"

static void test_depart_sur_le_dongle(void)
{
    half_tx_fsm_t s = { HALF_TX_TO_DONGLE, 0 };
    TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "au repos la droite vise le dongle");
    /* Un ACK ne fait jamais basculer, quoi qu'il arrive. */
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT(!half_tx_target_step(&s, true, HALF_TX_SWITCH_FAILS),
                    "un ACK ne réarme jamais");
        TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "et ne change jamais la cible");
    }
}

static void test_seuil_avant_bascule(void)
{
    half_tx_fsm_t s = { HALF_TX_TO_DONGLE, 0 };
    /* seuil-1 échecs : pas encore de bascule. */
    for (unsigned i = 0; i < HALF_TX_SWITCH_FAILS - 1u; i++) {
        TEST_ASSERT(!half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                    "avant le seuil : pas de réarmement");
        TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "avant le seuil : toujours le dongle");
    }
    /* L'échec du seuil : réarmement + bascule vers la gauche. */
    TEST_ASSERT(half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                "au seuil : réarmer");
    TEST_ASSERT(s.cible == HALF_TX_TO_LEFT, "au seuil : bascule vers la gauche KaSe.03");
}

static void test_un_ack_annule_le_compte(void)
{
    half_tx_fsm_t s = { HALF_TX_TO_DONGLE, 0 };
    /* Presque au seuil… */
    for (unsigned i = 0; i < HALF_TX_SWITCH_FAILS - 1u; i++)
        half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS);
    /* …un seul ACK et le compteur repart de zéro : un glitch isolé ne bascule pas. */
    TEST_ASSERT(!half_tx_target_step(&s, true, HALF_TX_SWITCH_FAILS), "ACK : rien");
    TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "ACK : cible inchangée");
    /* Il faut de nouveau tout le seuil pour basculer. */
    for (unsigned i = 0; i < HALF_TX_SWITCH_FAILS - 1u; i++)
        TEST_ASSERT(!half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                    "après l'ACK, il faut de nouveau atteindre le seuil");
    TEST_ASSERT(half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                "seuil complet atteint : bascule");
    TEST_ASSERT(s.cible == HALF_TX_TO_LEFT, "bascule vers la gauche");
}

static void test_rebascule_auto_cicatrisante(void)
{
    /* Dongle absent → on est passé sur la gauche. Si la gauche cesse à son tour
     * d'acquitter (USB débranché → elle n'écoute plus), on rebascule au dongle. */
    half_tx_fsm_t s = { HALF_TX_TO_LEFT, 0 };
    for (unsigned i = 0; i < HALF_TX_SWITCH_FAILS - 1u; i++)
        TEST_ASSERT(!half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                    "sur la gauche, avant le seuil : pas de bascule");
    TEST_ASSERT(half_tx_target_step(&s, false, HALF_TX_SWITCH_FAILS),
                "seuil sur la gauche : rebascule");
    TEST_ASSERT(s.cible == HALF_TX_TO_DONGLE, "retour au dongle");

    /* Aucune configuration n'a deux cibles : l'enum est binaire, la bascule est
     * une négation — invariant structurel « une seule cible à la fois ». */
    TEST_ASSERT(HALF_TX_TO_DONGLE != HALF_TX_TO_LEFT, "les deux cibles sont distinctes");
}

void test_half_tx_target(void)
{
    TEST_SUITE("Bascule de cible TX de la droite (repli sans dongle)");
    test_depart_sur_le_dongle();
    test_seuil_avant_bascule();
    test_un_ack_annule_le_compte();
    test_rebascule_auto_cicatrisante();
}
