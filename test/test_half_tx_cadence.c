/* Quand la moitie droite doit-elle emettre ? (logique pure)
 *
 * Deux regles ecrites separement le 2026-09-05 se contredisaient :
 *
 *   - la droite emet SUR CHANGEMENT de matrice, jamais periodiquement. C'est la
 *     premisse §2.3 du design et la seule qui rende le pari R1 tenable : au
 *     repos, un clavier ne produit aucun trafic.
 *   - la gauche RELACHE les touches distantes apres 250 ms de silence, pour
 *     qu'une moitie morte ne laisse pas « Maj » enfoncee chez l'hote.
 *
 * Maintenir une touche plus de 250 ms ne produit aucun changement, donc aucune
 * trame, donc la gauche conclut au silence et relache une touche qui est
 * pourtant physiquement enfoncee. Pas de repetition, et les modificateurs de la
 * droite lachent en pleine frappe.
 *
 * La regle correcte distingue le REPOS de l'INACTIVITE : silence quand rien
 * n'est enfonce, rafraichissement tant que quelque chose l'est. Le repos reste
 * muet — R1 tient — mais un maintien est reaffirme avant que la gauche ne
 * puisse en douter. */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"

#define PERIODE 100u

static void test_un_changement_emet_toujours(void)
{
    TEST_ASSERT(half_tx_doit_emettre(true, false, 1000, 1000, PERIODE),
                "un relachement emet, meme s'il ne reste rien d'enfonce");
    TEST_ASSERT(half_tx_doit_emettre(true, true, 1000, 999, PERIODE),
                "un appui emet sans attendre la periode");
}

static void test_le_repos_est_muet(void)
{
    /* LE test de ce fichier cote R1 : rien d'enfonce, rien a dire, meme apres
     * une eternite. C'est ce qui rend le lien gratuit au repos. */
    TEST_ASSERT(!half_tx_doit_emettre(false, false, 1000, 900, PERIODE),
                "rien d'enfonce : pas d'emission");
    TEST_ASSERT(!half_tx_doit_emettre(false, false, 999999, 0, PERIODE),
                "et toujours rien, quel que soit le temps ecoule");
}

static void test_un_maintien_est_rafraichi(void)
{
    /* LE test de ce fichier cote fiabilite. */
    TEST_ASSERT(!half_tx_doit_emettre(false, true, 1050, 1000, PERIODE),
                "50 ms apres : trop tot, la gauche n'a pas encore de doute");
    TEST_ASSERT(half_tx_doit_emettre(false, true, 1100, 1000, PERIODE),
                "100 ms apres : on reaffirme le maintien");
    TEST_ASSERT(half_tx_doit_emettre(false, true, 5000, 1000, PERIODE),
                "et longtemps apres, a plus forte raison");
}

static void test_la_periode_tient_sous_le_delai_de_la_gauche(void)
{
    /* Contrainte de conception, pas de gout : la gauche relache a 250 ms. Une
     * periode de rafraichissement doit laisser passer AU MOINS une trame
     * perdue avant que ce delai ne tombe, sinon un seul paquet manque suffit a
     * relacher une touche tenue. */
    TEST_ASSERT(HALF_TX_REFRESH_MS * 2 < HALF_LINK_TIMEOUT_MS,
                "deux rafraichissements tiennent dans le delai de relachement");
}

static void test_le_compteur_de_ms_peut_deborder(void)
{
    /* esp_timer_get_time()/1000 tronque a 32 bits deborde au bout de ~49 jours.
     * L'ecart doit se calculer en arithmetique non signee modulaire, sinon le
     * clavier cesse d'emettre ses maintiens ce jour-la. */
    uint32_t avant = 0xFFFFFFF0u;      /* juste avant le debordement */
    uint32_t apres = 0x00000060u;      /* 112 ms plus tard, apres bouclage */
    TEST_ASSERT(half_tx_doit_emettre(false, true, apres, avant, PERIODE),
                "le debordement du compteur ne fige pas l'emission");
}

void test_half_tx_cadence(void)
{
    printf("\n-- cadence d'emission de la moitie droite --\n");
    test_un_changement_emet_toujours();
    test_le_repos_est_muet();
    test_un_maintien_est_rafraichi();
    test_la_periode_tient_sous_le_delai_de_la_gauche();
    test_le_compteur_de_ms_peut_deborder();
}
