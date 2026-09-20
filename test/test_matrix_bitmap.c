/* Matrix -> half-matrix bitmap encoding (pure logic).
 *
 * Both halves pack their local matrix into a 4x7 bitmap before
 * transmitting it by radio. The code was copied inline in several places
 * (matrix_scan.c); extracted here into a single tested implementation, which
 * both halves will share under the fusion-to-dongle setup
 * (docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md). */
#include "test_framework.h"
#include "../main/comm/rf/rf_packet.h"

#define R 4
#define C 7

static void test_une_touche(void)
{
    uint8_t st[R*C]; memset(st, 0, sizeof(st));
    st[2*C + 5] = 1;                       /* (2,5) pressed */
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    rf_matrix_to_bitmap(st, R, C, bm);
    TEST_ASSERT(rf_bitmap_get(bm, 2, 5), "(2,5) is in the bitmap");
    TEST_ASSERT(!rf_bitmap_get(bm, 2, 4), "the neighbor isn't");
    TEST_ASSERT(!rf_bitmap_get(bm, 1, 5), "nor the one above");
}

static void test_rien(void)
{
    uint8_t st[R*C]; memset(st, 0, sizeof(st));
    uint8_t bm[RF_HALF_BITMAP_BYTES]; memset(bm, 0xFF, sizeof(bm));  /* deliberately dirtied */
    rf_matrix_to_bitmap(st, R, C, bm);
    for (uint8_t r = 0; r < R; r++)
        for (uint8_t c = 0; c < C; c++)
            TEST_ASSERT(!rf_bitmap_get(bm, r, c), "empty matrix -> empty bitmap (clears the noise)");
}

static void test_toutes(void)
{
    uint8_t st[R*C]; memset(st, 1, sizeof(st));
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    rf_matrix_to_bitmap(st, R, C, bm);
    for (uint8_t r = 0; r < R; r++)
        for (uint8_t c = 0; c < C; c++)
            TEST_ASSERT(rf_bitmap_get(bm, r, c), "all pressed -> all in the bitmap");
}

void test_matrix_bitmap(void)
{
    printf("\n-- matrice -> bitmap demi-matrice --\n");
    test_une_touche();
    test_rien();
    test_toutes();
}
