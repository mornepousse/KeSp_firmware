/* Encodage matrice → bitmap demi-matrice (logique pure).
 *
 * Les deux moitiés empaquettent leur matrice locale en un bitmap 4×7 avant de
 * l'émettre par radio. Le code était copié en ligne à plusieurs endroits
 * (matrix_scan.c) ; extrait ici pour une seule implémentation testée, que les
 * deux moitiés partageront sous la fusion-au-dongle
 * (docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md). */
#include "test_framework.h"
#include "../main/comm/rf/rf_packet.h"

#define R 4
#define C 7

static void test_une_touche(void)
{
    uint8_t st[R*C]; memset(st, 0, sizeof(st));
    st[2*C + 5] = 1;                       /* (2,5) enfoncée */
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    rf_matrix_to_bitmap(st, R, C, bm);
    TEST_ASSERT(rf_bitmap_get(bm, 2, 5), "(2,5) est dans le bitmap");
    TEST_ASSERT(!rf_bitmap_get(bm, 2, 4), "la voisine non");
    TEST_ASSERT(!rf_bitmap_get(bm, 1, 5), "celle du dessus non plus");
}

static void test_rien(void)
{
    uint8_t st[R*C]; memset(st, 0, sizeof(st));
    uint8_t bm[RF_HALF_BITMAP_BYTES]; memset(bm, 0xFF, sizeof(bm));  /* sali exprès */
    rf_matrix_to_bitmap(st, R, C, bm);
    for (uint8_t r = 0; r < R; r++)
        for (uint8_t c = 0; c < C; c++)
            TEST_ASSERT(!rf_bitmap_get(bm, r, c), "matrice vide → bitmap vide (efface le bruit)");
}

static void test_toutes(void)
{
    uint8_t st[R*C]; memset(st, 1, sizeof(st));
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    rf_matrix_to_bitmap(st, R, C, bm);
    for (uint8_t r = 0; r < R; r++)
        for (uint8_t c = 0; c < C; c++)
            TEST_ASSERT(rf_bitmap_get(bm, r, c), "toutes enfoncées → toutes dans le bitmap");
}

void test_matrix_bitmap(void)
{
    printf("\n-- matrice -> bitmap demi-matrice --\n");
    test_une_touche();
    test_rien();
    test_toutes();
}
