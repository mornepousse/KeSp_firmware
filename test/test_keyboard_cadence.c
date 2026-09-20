/* Keyboard task cadence: 10 ms while a timer can still run, 100 ms
 * at idle — otherwise automatic light sleep never happens (3 free ticks
 * are required, the loop was leaving only 1). */
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
    /* The leader expires 1000 ms after its keypress: still clocked at 10 ms. */
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(11000, 10000, false, false), KBD_CADENCE_ACTIF_MS, "cadence");
}

static void test_usb_et_test_matrice_restent_actifs(void)
{
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(100000, 0, true, false), KBD_CADENCE_ACTIF_MS, "cadence");
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(100000, 0, false, true), KBD_CADENCE_ACTIF_MS, "cadence");
}

static void test_debordement_du_compteur(void)
{
    /* now just wrapped back to zero, the activity predates it. */
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(100, 0xFFFFFFF0u, false, false), KBD_CADENCE_ACTIF_MS, "cadence");
}

void test_keyboard_cadence(void)
{
    TEST_SUITE("keyboard task cadence (tickless)");
    TEST_RUN(test_repos_apres_la_fenetre);
    TEST_RUN(test_actif_dans_la_fenetre);
    TEST_RUN(test_fenetre_couvre_le_leader);
    TEST_RUN(test_usb_et_test_matrice_restent_actifs);
    TEST_RUN(test_debordement_du_compteur);
}
