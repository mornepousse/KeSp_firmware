/* Cadence de la trame de supervision clavier -> dongle (logique pure).
 *
 * Le dongle declare un slot PERDU apres RF_LINK_LOST_MS de silence total et
 * applique son repli — il relache les touches, pour qu'un clavier disparu ne
 * laisse pas une touche collee chez l'hote. Il attend donc une trame d'etat au
 * repos ; son propre commentaire le dit (« nettement au-dessus de la cadence
 * de la trame d'etat au repos »).
 *
 * Or PKT_TYPE_STATUS n'etait JAMAIS emis : il n'existait qu'en decodage. Rien
 * ne l'envoyait depuis le clavier.
 *
 * Tant qu'on tape, les rapports HID entretiennent le lien par accident. Mais
 * une touche MAINTENUE ne produit aucun changement, donc plus aucun rapport :
 * le silence depasse RF_LINK_LOST_MS et le dongle relache la touche. Constate
 * au banc le 2026-09-08 — Backspace maintenue remontait toute seule au bout
 * d'environ deux secondes, et la constante du dongle vaut 2000/2500 ms.
 *
 * C'est le MEME defaut que celui de la moitie droite, un cran plus loin dans la
 * chaine : « emettre sur changement » et « relacher sur silence » ne composent
 * pas, quel que soit le maillon. */
#include "test_framework.h"
#include "../main/comm/rf/rf_slot.h"

static void test_rien_avant_la_periode(void)
{
    TEST_ASSERT(!rf_status_doit_emettre(1500, 1000, RF_STATUS_PERIOD_MS),
                "500 ms apres : trop tot");
    TEST_ASSERT(!rf_status_doit_emettre(1999, 1000, RF_STATUS_PERIOD_MS),
                "999 ms apres : toujours trop tot");
}

static void test_emission_a_la_periode(void)
{
    TEST_ASSERT(rf_status_doit_emettre(2000, 1000, RF_STATUS_PERIOD_MS),
                "une periode pleine : on emet");
    TEST_ASSERT(rf_status_doit_emettre(9999, 1000, RF_STATUS_PERIOD_MS),
                "et a plus forte raison bien apres");
}

static void test_la_marge_couvre_une_perte(void)
{
    /* LE test de ce fichier. Une seule trame perdue ne doit pas suffire a faire
     * declarer le lien mort : il en faut au moins deux consecutives. */
    TEST_ASSERT(RF_STATUS_PERIOD_MS * 2 < RF_LINK_LOST_MS,
                "deux trames d'etat tiennent dans le budget de silence du dongle");
}

static void test_le_compteur_de_ms_peut_deborder(void)
{
    /* esp_timer_get_time()/1000 tronque a 32 bits deborde vers 49 jours : une
     * soustraction signee figerait la supervision ce jour-la, et le dongle
     * relacherait les touches en boucle. */
    /* dernier = 0xFFFFFF00, now = 0x000003B0 : 1200 ms se sont ecoules A TRAVERS
     * le bouclage. La soustraction non signee doit rendre 1200, pas une valeur
     * enorme ni negative. */
    TEST_ASSERT(rf_status_doit_emettre(0x000003B0u, 0xFFFFFF00u, RF_STATUS_PERIOD_MS),
                "1200 ms a travers le bouclage : on emet");
    /* Et le bouclage ne doit pas non plus faire emettre TROP TOT : 512 ms. */
    TEST_ASSERT(!rf_status_doit_emettre(0x00000100u, 0xFFFFFF00u, RF_STATUS_PERIOD_MS),
                "512 ms a travers le bouclage : toujours trop tot");
}

void test_rf_status_cadence(void)
{
    printf("\n-- cadence de la trame de supervision --\n");
    test_rien_avant_la_periode();
    test_emission_a_la_periode();
    test_la_marge_couvre_une_perte();
    test_le_compteur_de_ms_peut_deborder();
}
