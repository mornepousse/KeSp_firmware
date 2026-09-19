/* Vetos de veille : un registre nommé, comme les verrous esp_pm. Un veto posé
 * bloque toute veille ; c'est un ÉTAT par nom, pas un compteur — chaque module
 * ne pose que le sien, poser deux fois puis lever une fois = levé. */
#include "test_framework.h"
#include "../main/power/veille_veto.h"
#include <string.h>

static void test_sans_veto_rien_ne_bloque(void)
{
    veille_vetos_t v = {0};
    TEST_ASSERT(!veille_bloquee(&v), "vide");
}

static void test_un_veto_bloque_jusqu_a_sa_levee(void)
{
    veille_vetos_t v = {0};
    veille_veto_poser(&v, VEILLE_VETO_USB, true);
    TEST_ASSERT(veille_bloquee(&v), "usb pose");
    veille_veto_poser(&v, VEILLE_VETO_LIEN, true);
    veille_veto_poser(&v, VEILLE_VETO_USB, false);
    TEST_ASSERT(veille_bloquee(&v), "lien tient encore");
    veille_veto_poser(&v, VEILLE_VETO_LIEN, false);
    TEST_ASSERT(!veille_bloquee(&v), "tout leve");
}

static void test_lever_un_veto_absent_est_sans_effet(void)
{
    veille_vetos_t v = {0};
    veille_veto_poser(&v, VEILLE_VETO_SYNC, false);
    TEST_ASSERT(!veille_bloquee(&v), "toujours vide");
    veille_veto_poser(&v, VEILLE_VETO_TEST, true);
    veille_veto_poser(&v, VEILLE_VETO_TEST, true);   /* deux poses */
    veille_veto_poser(&v, VEILLE_VETO_TEST, false);  /* une levee suffit : etat, pas compteur */
    TEST_ASSERT(!veille_bloquee(&v), "etat, pas compteur");
}

static void test_noms_pour_le_hb(void)
{
    char buf[24]; veille_vetos_t v = {0};
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "-") == 0, "aucun -> -");
    veille_veto_poser(&v, VEILLE_VETO_USB, true);
    veille_veto_poser(&v, VEILLE_VETO_LIEN, true);
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "usb+lien") == 0, "usb+lien");
    veille_veto_poser(&v, VEILLE_VETO_SYNC, true);
    veille_veto_poser(&v, VEILLE_VETO_TEST, true);
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "usb+lien+sync+test") == 0, "les quatre");
    char petit[6];
    TEST_ASSERT(strlen(veille_vetos_str(&v, petit, sizeof petit)) < sizeof petit, "borne : jamais de debordement");
}

void test_veille_veto(void)
{
    TEST_SUITE("vetos de veille");
    TEST_RUN(test_sans_veto_rien_ne_bloque);
    TEST_RUN(test_un_veto_bloque_jusqu_a_sa_levee);
    TEST_RUN(test_lever_un_veto_absent_est_sans_effet);
    TEST_RUN(test_noms_pour_le_hb);
}
