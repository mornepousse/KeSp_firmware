/* Cadence de la tâche clavier : 10 ms tant qu'une minuterie peut courir, 100 ms
 * au repos — sinon le light sleep automatique n'arrive jamais (3 ticks libres
 * exigés, la boucle en laissait 1). */
#include "test_framework.h"
#include "../main/input/keyboard_cadence.h"

static void test_repos_apres_la_fenetre(void)
{
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(10000, 10000 - KBD_CADENCE_FENETRE_MS, false, false),
              KBD_CADENCE_REPOS_MS, "cadence");
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(100000, 0, false, false), KBD_CADENCE_REPOS_MS, "cadence");
}

static void test_actif_dans_la_fenetre(void)
{
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(10000, 10000, false, false), KBD_CADENCE_ACTIF_MS, "cadence");
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(10000, 10000 - KBD_CADENCE_FENETRE_MS + 1, false, false),
              KBD_CADENCE_ACTIF_MS, "cadence");
}

static void test_fenetre_couvre_le_leader(void)
{
    /* Le leader expire 1000 ms après sa frappe : encore cadencé à 10 ms. */
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(11000, 10000, false, false), KBD_CADENCE_ACTIF_MS, "cadence");
}

static void test_usb_et_test_matrice_restent_actifs(void)
{
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(100000, 0, true, false), KBD_CADENCE_ACTIF_MS, "cadence");
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(100000, 0, false, true), KBD_CADENCE_ACTIF_MS, "cadence");
}

static void test_debordement_du_compteur(void)
{
    /* now vient de repasser par zéro, l'activité date d'avant. */
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(100, 0xFFFFFFF0u, false, false), KBD_CADENCE_ACTIF_MS, "cadence");
}

void test_keyboard_cadence(void)
{
    TEST_SUITE("cadence de la tache clavier (tickless)");
    TEST_RUN(test_repos_apres_la_fenetre);
    TEST_RUN(test_actif_dans_la_fenetre);
    TEST_RUN(test_fenetre_couvre_le_leader);
    TEST_RUN(test_usb_et_test_matrice_restent_actifs);
    TEST_RUN(test_debordement_du_compteur);
}
