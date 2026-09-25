/* Sleep vetos: a named register, like esp_pm locks. A veto that is set
 * blocks all sleep; it's a STATE by name, not a counter — each module
 * only sets its own, setting it twice then clearing once = cleared. */
#include "test_framework.h"
#include "../main/power/veille_veto.h"
#include <string.h>

static void test_sans_veto_rien_ne_bloque(void)
{
    veille_vetos_t v = {0};
    TEST_ASSERT(!veille_bloquee(&v), "empty");
}

static void test_un_veto_bloque_jusqu_a_sa_levee(void)
{
    veille_vetos_t v = {0};
    veille_veto_poser(&v, VEILLE_VETO_USB, true);
    TEST_ASSERT(veille_bloquee(&v), "usb set");
    veille_veto_poser(&v, VEILLE_VETO_LIEN, true);
    veille_veto_poser(&v, VEILLE_VETO_USB, false);
    TEST_ASSERT(veille_bloquee(&v), "link still holds");
    veille_veto_poser(&v, VEILLE_VETO_LIEN, false);
    TEST_ASSERT(!veille_bloquee(&v), "all cleared");
}

static void test_lever_un_veto_absent_est_sans_effet(void)
{
    veille_vetos_t v = {0};
    veille_veto_poser(&v, VEILLE_VETO_SYNC, false);
    TEST_ASSERT(!veille_bloquee(&v), "still empty");
    veille_veto_poser(&v, VEILLE_VETO_TEST, true);
    veille_veto_poser(&v, VEILLE_VETO_TEST, true);   /* two sets */
    veille_veto_poser(&v, VEILLE_VETO_TEST, false);  /* one clear is enough: state, not a counter */
    TEST_ASSERT(!veille_bloquee(&v), "state, not a counter");
}

static void test_noms_pour_le_hb(void)
{
    char buf[VEILLE_VETOS_STR_MAX]; veille_vetos_t v = {0};
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "-") == 0, "none -> -");
    veille_veto_poser(&v, VEILLE_VETO_USB, true);
    veille_veto_poser(&v, VEILLE_VETO_LIEN, true);
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "usb+link") == 0, "usb+link");
    veille_veto_poser(&v, VEILLE_VETO_SYNC, true);
    veille_veto_poser(&v, VEILLE_VETO_TEST, true);
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "usb+link+sync+test") == 0, "all four");
    veille_veto_poser(&v, VEILLE_VETO_PAIR, true);
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "usb+link+sync+test+pair") == 0, "all five fit");
    veille_veto_poser(&v, VEILLE_VETO_USB, false); veille_veto_poser(&v, VEILLE_VETO_LIEN, false);
    veille_veto_poser(&v, VEILLE_VETO_SYNC, false); veille_veto_poser(&v, VEILLE_VETO_TEST, false);
    TEST_ASSERT(veille_bloquee(&v), "pairing alone blocks sleep");
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "pair") == 0, "pair");
    char petit[6];
    TEST_ASSERT(strlen(veille_vetos_str(&v, petit, sizeof petit)) < sizeof petit, "bound: never overflows");
}

/* A key held with nothing else happening produces no matrix change: the
 * inactivity counter kept growing and the half slept WITH the key down, woke at
 * once on its high row, re-armed the radio, and slept again at the next
 * threshold. Rare at 15 s, common at 5 s (Backspace held while the host
 * auto-repeats, a layer key held while reading). A held key is a veto, like
 * any other reason to stay awake (2026-09-25). */
static void test_touche_tenue_est_un_veto(void)
{
    veille_vetos_t v = {0};
    char buf[VEILLE_VETOS_STR_MAX];
    veille_veto_poser(&v, VEILLE_VETO_TOUCHE, true);
    TEST_ASSERT(veille_bloquee(&v), "a held key alone blocks sleep");
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "key") == 0, "named key in the HB");
    veille_veto_poser(&v, VEILLE_VETO_USB, true);  veille_veto_poser(&v, VEILLE_VETO_LIEN, true);
    veille_veto_poser(&v, VEILLE_VETO_SYNC, true); veille_veto_poser(&v, VEILLE_VETO_TEST, true);
    veille_veto_poser(&v, VEILLE_VETO_PAIR, true);
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "usb+link+sync+test+pair+key") == 0,
                "all six fit in VEILLE_VETOS_STR_MAX");
    veille_veto_poser(&v, VEILLE_VETO_TOUCHE, false);
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "usb+link+sync+test+pair") == 0,
                "released: the key veto goes, the others stay");
}

static void test_detection_touche_tenue(void)
{
    uint8_t etat[4 * 7] = {0};
    TEST_ASSERT(!veille_touche_tenue(etat, sizeof etat), "nothing held");
    etat[0] = 1;
    TEST_ASSERT(veille_touche_tenue(etat, sizeof etat), "first cell held");
    etat[0] = 0; etat[27] = 1;
    TEST_ASSERT(veille_touche_tenue(etat, sizeof etat), "last cell held");
    etat[27] = 0; etat[26] = 1;
    TEST_ASSERT(veille_touche_tenue(etat, sizeof etat), "the space bar (3,5) held");
    TEST_ASSERT(!veille_touche_tenue(NULL, 28), "no matrix: nothing held");
}

void test_veille_veto(void)
{
    TEST_SUITE("sleep vetos");
    TEST_RUN(test_sans_veto_rien_ne_bloque);
    TEST_RUN(test_un_veto_bloque_jusqu_a_sa_levee);
    TEST_RUN(test_lever_un_veto_absent_est_sans_effet);
    TEST_RUN(test_noms_pour_le_hb);
    TEST_RUN(test_touche_tenue_est_un_veto);
    TEST_RUN(test_detection_touche_tenue);
}
