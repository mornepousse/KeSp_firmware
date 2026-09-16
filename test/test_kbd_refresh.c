/* Répétition bornée du dernier rapport HID sur le lien radio (logique pure).
 *
 * Mesuré au banc le 2026-09-05 : le relais réémettait le dernier rapport à
 * 100 Hz en continu, clavier au repos, dès la première frappe de la vie de la
 * carte. Cela rendait fausse la prémisse de §2.3 du design Niphargus
 * (« les émissions sont événementielles, pas périodiques »), dont dépend le
 * pari R1 — une moitié gauche qui émet en permanence est sourde en permanence,
 * et n'entendrait jamais la droite. */
#include "test_framework.h"
#include "../main/comm/rf/kbd_relay_tx.h"

static void test_repos_ne_reemet_pas(void)
{
    /* LE test de ce fichier : un état neuf, jamais armé, ne doit produire
     * aucune émission. C'est ce qui rend la prémisse de §2.3 vraie. */
    kbd_refresh_t r = {0};
    for (int i = 0; i < 100; i++)
        TEST_ASSERT(!kbd_refresh_step(&r), "au repos, aucune reemission");
}

static void test_arme_puis_se_tait(void)
{
    kbd_refresh_t r = {0};
    kbd_refresh_arm(&r, 3);
    TEST_ASSERT(kbd_refresh_step(&r), "repetition 1");
    TEST_ASSERT(kbd_refresh_step(&r), "repetition 2");
    TEST_ASSERT(kbd_refresh_step(&r), "repetition 3");
    /* Puis le silence, définitivement — pas de flux permanent. */
    for (int i = 0; i < 50; i++)
        TEST_ASSERT(!kbd_refresh_step(&r), "silence apres la derniere repetition");
}

static void test_rearmement_repart_du_plein(void)
{
    /* Une frappe pendant la fenêtre de répétition doit remettre le compteur au
     * plein, pas l'incrémenter ni le laisser filer. */
    kbd_refresh_t r = {0};
    kbd_refresh_arm(&r, 3);
    TEST_ASSERT(kbd_refresh_step(&r), "premiere repetition consommee");
    kbd_refresh_arm(&r, 3);
    TEST_ASSERT(kbd_refresh_step(&r), "rearme : 1");
    TEST_ASSERT(kbd_refresh_step(&r), "rearme : 2");
    TEST_ASSERT(kbd_refresh_step(&r), "rearme : 3");
    TEST_ASSERT(!kbd_refresh_step(&r), "puis silence");
}

static void test_zero_repetition_est_muet(void)
{
    /* Armer à zéro désactive l'auto-réparation sans casser la mécanique. */
    kbd_refresh_t r = {0};
    kbd_refresh_arm(&r, 0);
    TEST_ASSERT(!kbd_refresh_step(&r), "arme a zero -> muet");
}

static void test_cadence_du_relais(void)
{
    /* 100 ms au repos ; 10 ms dès qu'il y a quelque chose à répéter, une touche
     * tenue, une sync — ET tant que la gauche écoute la droite réémise (route
     * USB) : c'est ce sondage qui vide la FIFO du nRF24 (3 trames), et à
     * 100 ms l'appui et le relâchement d'une touche de la droite arrivaient
     * dans le même tour, ne laissant que le relâchement (banc 2026-09-16 :
     * « je perds plein de touches de la droite » en USB). */
    TEST_ASSERT_EQ(kbd_relay_cadence_ms(false, false, false, false), KBD_RELAY_REPOS_MS, "repos");
    TEST_ASSERT_EQ(kbd_relay_cadence_ms(true,  false, false, false), KBD_RELAY_REFRESH_MS, "reparation en cours");
    TEST_ASSERT_EQ(kbd_relay_cadence_ms(false, true,  false, false), KBD_RELAY_REFRESH_MS, "touche tenue");
    TEST_ASSERT_EQ(kbd_relay_cadence_ms(false, false, true,  false), KBD_RELAY_REFRESH_MS, "sync");
    TEST_ASSERT_EQ(kbd_relay_cadence_ms(false, false, false, true),  KBD_RELAY_REFRESH_MS, "ecoute USB de la droite");
}

void test_kbd_refresh(void)
{
    printf("\n-- repetition bornee du rapport HID (radio) --\n");
    test_cadence_du_relais();
    test_repos_ne_reemet_pas();
    test_arme_puis_se_tait();
    test_rearmement_repart_du_plein();
    test_zero_repetition_est_muet();
}
