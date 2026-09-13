/* Réparation bornée après changement, côté moitié DROITE — logique pure.
 *
 * Une trame de changement refusée par l'ESB (~1 % au banc, après 15
 * retransmissions matérielles) n'avait qu'une seule chance : la règle de
 * maintien (half_tx_doit_emettre) ne rejoue l'état que pour une touche TENUE,
 * jamais pour un appui bref. Un appui bref dont la trame était refusée était
 * perdu à jamais (banc 2026-09-13, même cause que la gauche).
 *
 * Remède : un changement ARME un nombre borné de répétitions, consommées une
 * par tick même si rien n'est tenu ; ensuite, retour à la règle de maintien.
 * Le repos jamais armé reste muet — c'est la prémisse §2.3 (pari R1). */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"

#define P 100u   /* période de réaffirmation des maintiens */

static void test_changement_arme_des_repetitions(void)
{
    half_tx_repeat_t r = {0};
    TEST_ASSERT(half_tx_doit_emettre_repare(&r, true, false, 1000, 1000, P),
                "le changement émet, même s'il ne reste rien d'enfoncé");
    for (unsigned i = 0; i < HALF_TX_REPEATS; i++)
        TEST_ASSERT(half_tx_doit_emettre_repare(&r, false, false, 1020 + 20 * i, 1000 + 20 * i, P),
                    "répétition sans maintien : la trame perdue a une seconde chance");
    TEST_ASSERT(!half_tx_doit_emettre_repare(&r, false, false, 1300, 1280, P),
                "répétitions épuisées, rien de tenu : silence");
}

static void test_repos_jamais_arme_reste_muet(void)
{
    /* LE test côté R1 : un état neuf ne produit jamais rien. */
    half_tx_repeat_t r = {0};
    for (int i = 0; i < 200; i++)
        TEST_ASSERT(!half_tx_doit_emettre_repare(&r, false, false, 1000 + 20 * i, 1000, P),
                    "jamais armé : muet");
}

static void test_maintien_toujours_rafraichi_apres_les_repetitions(void)
{
    half_tx_repeat_t r = {0};
    half_tx_doit_emettre_repare(&r, true, true, 1000, 1000, P);                 /* arme */
    for (unsigned i = 0; i < HALF_TX_REPEATS; i++)
        half_tx_doit_emettre_repare(&r, false, true, 1000, 1000, P);            /* consomme */
    TEST_ASSERT(!half_tx_doit_emettre_repare(&r, false, true, 1050, 1000, P),
                "tenu, 50 ms : trop tôt pour la réaffirmation");
    TEST_ASSERT(half_tx_doit_emettre_repare(&r, false, true, 1100, 1000, P),
                "tenu, 100 ms : réaffirmé — la règle de maintien est intacte");
}

static void test_un_nouveau_changement_rearme(void)
{
    half_tx_repeat_t r = {0};
    half_tx_doit_emettre_repare(&r, true, true, 1000, 1000, P);
    half_tx_doit_emettre_repare(&r, false, true, 1020, 1000, P);                /* 1 consommée */
    TEST_ASSERT(half_tx_doit_emettre_repare(&r, true, false, 1030, 1020, P),
                "un relâchement (changement) émet et réarme");
    for (unsigned i = 0; i < HALF_TX_REPEATS; i++)
        TEST_ASSERT(half_tx_doit_emettre_repare(&r, false, false, 1050 + 20 * i, 1030, P),
                    "jeu COMPLET de répétitions après réarmement");
    TEST_ASSERT(!half_tx_doit_emettre_repare(&r, false, false, 1400, 1380, P), "puis silence");
}

void test_half_tx_repeat(void)
{
    TEST_SUITE("Réparation bornée après changement (droite)");
    test_changement_arme_des_repetitions();
    test_repos_jamais_arme_reste_muet();
    test_maintien_toujours_rafraichi_apres_les_repetitions();
    test_un_nouveau_changement_rearme();
}
