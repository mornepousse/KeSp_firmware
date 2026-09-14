/* Écran Sharp memory-LCD des moitiés — logique pure.
 *
 * Trois choses qu'un bug rendrait visibles à l'écran sans jamais planter :
 *  - rev8 : le panneau lit LSB-first, l'ESP32 émet MSB-first. Une inversion
 *    fausse = commandes ignorées, écran muet, aucune erreur nulle part.
 *  - la coupure du nom de couche sur 68 px : 4 caractères par ligne, 3 lignes,
 *    puis « … » — l'utilisateur a préféré des lignes lisibles à un texte tourné.
 *  - le diff du modèle : on ne redessine QUE si un champ affiché a changé —
 *    chaque redessin est une transaction sur le bus partagé avec la radio.
 * Spec : docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md */
#include "test_framework.h"
#include "../main/display/memlcd/memlcd_model.h"
#include <string.h>

static void test_rev8(void)
{
    TEST_ASSERT_EQ(memlcd_rev8(0x01), 0x80, "bit0 → bit7");
    TEST_ASSERT_EQ(memlcd_rev8(0x80), 0x01, "bit7 → bit0");
    TEST_ASSERT_EQ(memlcd_rev8(0xA5), 0xA5, "0xA5 est un palindrome binaire");
    TEST_ASSERT_EQ(memlcd_rev8(0x0F), 0xF0, "nibbles échangés bit à bit");
    TEST_ASSERT_EQ(memlcd_rev8(0x86), 0x61, "0x86 (1000 0110) → 0x61 (0110 0001)");
    for (unsigned b = 0; b < 256; b++)
        TEST_ASSERT_EQ(memlcd_rev8(memlcd_rev8((uint8_t)b)), (uint8_t)b, "involution");
}

static void test_couper_nom(void)
{
    char l[MEMLCD_NOM_LIGNES][MEMLCD_NOM_BUF];
    TEST_ASSERT_EQ(memlcd_couper_nom("DVORAK", l), 2, "6 lettres → 2 lignes");
    TEST_ASSERT(strcmp(l[0], "DVOR") == 0 && strcmp(l[1], "AK") == 0, "DVOR / AK");
    TEST_ASSERT(l[2][0] == '\0', "3e ligne vide");
    TEST_ASSERT_EQ(memlcd_couper_nom("NAV", l), 1, "3 lettres → 1 ligne");
    TEST_ASSERT(strcmp(l[0], "NAV") == 0, "NAV");
    TEST_ASSERT_EQ(memlcd_couper_nom("", l), 1, "vide → 1 ligne vide (jamais 0)");
    TEST_ASSERT(l[0][0] == '\0', "ligne vide");
    TEST_ASSERT_EQ(memlcd_couper_nom(NULL, l), 1, "NULL → comme vide, pas de plantage");
    TEST_ASSERT_EQ(memlcd_couper_nom("ABCDEFGHIJKL", l), 3, "12 lettres → 3 lignes pleines");
    TEST_ASSERT(strcmp(l[2], "IJKL") == 0, "3e ligne pleine, sans …");
    TEST_ASSERT_EQ(memlcd_couper_nom("ABCDEFGHIJKLMNOP", l), 3, "16 lettres → 3 lignes, tronqué");
    TEST_ASSERT(strcmp(l[2], "IJK\xE2\x80\xA6") == 0, "3e ligne = 3 lettres + … (UTF-8)");
}

static void test_model_diff(void)
{
    memlcd_model_t a = { .route_rf = 1, .dongle_vu = 1, .batt_local_dv = 40,
                         .batt_autre_dv = 42, .couche = 1, .nom = "DVORAK", .is_left = 1 };
    memlcd_model_t b = a;
    TEST_ASSERT(!memlcd_model_diff(&a, &b), "identiques → pas de redessin");
    b.batt_local_dv = 39;  TEST_ASSERT(memlcd_model_diff(&a, &b), "tension locale change → redessin");
    b = a; b.batt_autre_chg = 2; TEST_ASSERT(memlcd_model_diff(&a, &b), "charge de l'autre change → redessin");
    b = a; b.couche = 2;   TEST_ASSERT(memlcd_model_diff(&a, &b), "couche change → redessin");
    b = a; strcpy(b.nom, "NAV"); TEST_ASSERT(memlcd_model_diff(&a, &b), "nom change → redessin");
    b = a; b.dongle_vu = 0; TEST_ASSERT(memlcd_model_diff(&a, &b), "dongle perdu → redessin");
    b = a; b.is_left = 0;  TEST_ASSERT(!memlcd_model_diff(&a, &b), "is_left n'est pas une donnée affichée qui bouge");
}

void test_memlcd_model(void)
{
    TEST_SUITE("Écran memory-LCD : logique pure");
    test_rev8();
    test_couper_nom();
    test_model_diff();
}
