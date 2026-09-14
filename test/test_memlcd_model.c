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
                         .couche = 1, .nom = "DVORAK", .is_left = 1 };
    memlcd_model_t b = a;
    TEST_ASSERT(!memlcd_model_diff(&a, &b), "identiques → pas de redessin");
    b.batt_local_dv = 39;  TEST_ASSERT(memlcd_model_diff(&a, &b), "tension locale change → redessin");
    b = a; b.batt_local_chg = 2; TEST_ASSERT(memlcd_model_diff(&a, &b), "état de charge change → redessin");
    b = a; b.couche = 2;   TEST_ASSERT(memlcd_model_diff(&a, &b), "couche change → redessin");
    b = a; strcpy(b.nom, "NAV"); TEST_ASSERT(memlcd_model_diff(&a, &b), "nom change → redessin");
    b = a; b.dongle_vu = 0; TEST_ASSERT(memlcd_model_diff(&a, &b), "dongle perdu → redessin");
    b = a; b.is_left = 0;  TEST_ASSERT(!memlcd_model_diff(&a, &b), "is_left n'est pas une donnée affichée qui bouge");
}

/* Le panneau est PHYSIQUEMENT 68 lignes de 160 pixels (catalogue Sharp, doc
 * lemia 6844 p. 5 : « LS011B7DH03 160 × 68 », H = sens des données) ; on le
 * monte debout. Le tampon portrait (68 × 160, 9 octets par rangée, bit 7 =
 * x = 0, 1 = encre) se transpose donc en 68 lignes de 20 octets, bit 7 = D1,
 * 1 = BLANC (app note doc 6845 p. 10 : D(n) = L → noir). */
static void test_fb_to_panel(void)
{
    static uint8_t fb[MEMLCD_H * MEMLCD_LINE_BYTES];
    static uint8_t panel[MEMLCD_PANEL_LINES * MEMLCD_PANEL_LINE_BYTES];
    TEST_ASSERT_EQ(MEMLCD_PANEL_LINES, 68, "68 lignes de grille");
    TEST_ASSERT_EQ(MEMLCD_PANEL_LINE_BYTES, 20, "160 pixels par ligne = 20 octets");

    memset(fb, 0, sizeof fb);
    memlcd_fb_to_panel(fb, panel, false);
    bool blanc = true;
    for (size_t i = 0; i < sizeof panel; i++) if (panel[i] != 0xFF) blanc = false;
    TEST_ASSERT(blanc, "tampon vide → panneau tout blanc (1 = blanc chez Sharp)");

    /* pixel portrait (x=0, y=0) : coin haut-gauche → ligne 0, colonne 159 (rotation 90°) */
    fb[0] = 0x80;
    memlcd_fb_to_panel(fb, panel, false);
    TEST_ASSERT_EQ(panel[0 * MEMLCD_PANEL_LINE_BYTES + 19], 0xFE, "(0,0) → ligne 0, D160 (bit 0 du dernier octet) noir");
    TEST_ASSERT_EQ(panel[1 * MEMLCD_PANEL_LINE_BYTES + 19], 0xFF, "la ligne 1 n'est pas touchée");

    /* pixel (x=67, y=159) : coin bas-droit → ligne 67, colonne 0 (D1 = bit 7 de l'octet 0) */
    memset(fb, 0, sizeof fb);
    fb[159 * MEMLCD_LINE_BYTES + 8] = 0x10;   /* x = 67 = octet 8, bit (7 - 3) */
    memlcd_fb_to_panel(fb, panel, false);
    TEST_ASSERT_EQ(panel[67 * MEMLCD_PANEL_LINE_BYTES + 0], 0x7F, "(67,159) → ligne 67, D1 noir");

    /* rotation 180 : (0,0) → ligne 67, colonne 0 */
    memset(fb, 0, sizeof fb); fb[0] = 0x80;
    memlcd_fb_to_panel(fb, panel, true);
    TEST_ASSERT_EQ(panel[67 * MEMLCD_PANEL_LINE_BYTES + 0], 0x7F, "rot180 : (0,0) → ligne 67, D1");
    TEST_ASSERT_EQ(panel[0 * MEMLCD_PANEL_LINE_BYTES + 19], 0xFF, "rot180 : la ligne 0 reste blanche");
}

void test_memlcd_model(void)
{
    TEST_SUITE("Écran memory-LCD : logique pure");
    test_rev8();
    test_couper_nom();
    test_model_diff();
    test_fb_to_panel();
}
