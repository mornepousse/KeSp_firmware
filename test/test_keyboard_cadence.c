/* Keyboard task cadence: 10 ms only while a TIMED feature is actually waiting
 * on the clock (tap-hold undecided, tap-dance counting, leader sequence,
 * macro queued), 100 ms otherwise — so that automatic light sleep can slip in
 * between keystrokes (it needs 3 free ticks; a 10 ms loop leaves 1).
 *
 * Until 2026-09-25 ANY keystroke armed 1.5 s at 10 ms (KBD_CADENCE_FENETRE_MS),
 * "in case" a timer was running: typing every ~200 ms, the window never closed
 * and the half never slept while typing — the working day measured at
 * ~20 mA. A plain letter starts no timer; matrix changes wake the task by
 * notification anyway, so the first keystroke never waits. */
#include "test_framework.h"
#include "../main/input/keyboard_cadence.h"

static void test_une_touche_simple_n_arme_plus_rien(void)
{
    /* The heart of the change: no timer pending, no USB, no test mode ->
     * rest cadence, whatever happened a moment ago. */
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(false, false, false), KBD_CADENCE_REPOS_MS,
                   "a plain keystroke no longer keeps the task at 10 ms");
}

static void test_un_minuteur_en_attente_garde_10_ms(void)
{
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(false, false, true), KBD_CADENCE_ACTIF_MS,
                   "tap-hold / tap-dance / leader / macro waiting -> 10 ms");
}

static void test_usb_et_test_matrice_restent_actifs(void)
{
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(true, false, false), KBD_CADENCE_ACTIF_MS,
                   "USB: the left drains the dongle's re-sent right half every 10 ms");
    TEST_ASSERT_EQ(kbd_cadence_attente_ms(false, true, false), KBD_CADENCE_ACTIF_MS,
                   "matrix test mode");
}

static void test_le_repos_laisse_dormir(void)
{
    /* The rest cadence must leave the 3 free ticks light sleep needs. */
    TEST_ASSERT(KBD_CADENCE_REPOS_MS >= CADENCE_REPOS_MIN_MS, "rest wait >= 3 ticks");
}

void test_keyboard_cadence(void)
{
    TEST_SUITE("keyboard task cadence (tickless)");
    TEST_RUN(test_une_touche_simple_n_arme_plus_rien);
    TEST_RUN(test_un_minuteur_en_attente_garde_10_ms);
    TEST_RUN(test_usb_et_test_matrice_restent_actifs);
    TEST_RUN(test_le_repos_laisse_dormir);
}
