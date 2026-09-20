/* Fusion of TWO half-matrices at the dongle (pure logic) — "dongle fusion" brick.
 *
 * Design: docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md.
 *
 * In wireless mode, both halves emit their RAW matrix to the dongle, which
 * fuses them and runs the engine. Unlike matrix_apply_remote (one local half
 * + one remote), here NEITHER is local: the dongle receives two bitmaps and
 * must produce the (row, keymap column) positions that the engine indexes.
 * The left occupies columns 0-6 directly; the right occupies 7-13, mirrored
 * (same PCB flipped over), via the same rule as the master (half_col_to_keymap).
 */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"
#include "../main/comm/rf/rf_packet.h"

#define COLS 7

static void test_gauche_en_direct(void)
{
    uint8_t l[RF_HALF_BITMAP_BYTES] = {0}, r[RF_HALF_BITMAP_BYTES] = {0};
    rf_bitmap_set(l, 0, 0, true);   /* left (0,0) */
    rf_bitmap_set(l, 2, 6, true);   /* left (2,6) */
    uint8_t row[12], col[12];
    uint8_t n = fuse_halves(l, r, COLS, true, row, col, 12);
    TEST_ASSERT(n == 2, "two left keys");
    /* left columns = identity 0..6 */
    bool a = (row[0]==0 && col[0]==0) || (row[1]==0 && col[1]==0);
    bool b = (row[0]==2 && col[0]==6) || (row[1]==2 && col[1]==6);
    TEST_ASSERT(a && b, "left in direct columns 0-6");
}

static void test_droite_en_miroir(void)
{
    uint8_t l[RF_HALF_BITMAP_BYTES] = {0}, r[RF_HALF_BITMAP_BYTES] = {0};
    rf_bitmap_set(r, 0, 0, true);   /* right physical column 0 -> keymap 13 */
    rf_bitmap_set(r, 1, 6, true);   /* right physical column 6 -> keymap 7 */
    uint8_t row[12], col[12];
    uint8_t n = fuse_halves(l, r, COLS, true, row, col, 12);
    TEST_ASSERT(n == 2, "two right keys");
    bool a = (row[0]==0 && col[0]==13) || (row[1]==0 && col[1]==13);
    bool b = (row[0]==1 && col[0]==7)  || (row[1]==1 && col[1]==7);
    TEST_ASSERT(a && b, "right mirrored 13..7");
}

static void test_les_deux_ensemble(void)
{
    uint8_t l[RF_HALF_BITMAP_BYTES] = {0}, r[RF_HALF_BITMAP_BYTES] = {0};
    rf_bitmap_set(l, 1, 1, true);
    rf_bitmap_set(r, 1, 1, true);
    uint8_t row[12], col[12];
    uint8_t n = fuse_halves(l, r, COLS, true, row, col, 12);
    TEST_ASSERT(n == 2, "one on each side");
    /* left (1,1)->col1, right (1,1)->col 13-1=12 ; never the same column */
    TEST_ASSERT(col[0] != col[1], "the two halves don't collide");
}

static void test_plafond_respecte(void)
{
    uint8_t l[RF_HALF_BITMAP_BYTES] = {0}, r[RF_HALF_BITMAP_BYTES] = {0};
    for (uint8_t c = 0; c < COLS; c++) { rf_bitmap_set(l, 0, c, true); rf_bitmap_set(r, 0, c, true); }
    uint8_t row[6], col[6];
    uint8_t n = fuse_halves(l, r, COLS, true, row, col, 6);
    TEST_ASSERT(n == 6, "bounded to max, never overflows");
}

static void test_sans_miroir_est_un_decalage(void)
{
    uint8_t l[RF_HALF_BITMAP_BYTES] = {0}, r[RF_HALF_BITMAP_BYTES] = {0};
    rf_bitmap_set(r, 0, 0, true);
    uint8_t row[12], col[12];
    uint8_t n = fuse_halves(l, r, COLS, false, row, col, 12);
    TEST_ASSERT(n == 1 && row[0]==0 && col[0]==7, "without mirroring: col 0 -> 7");
}

void test_fuse_halves(void)
{
    printf("\n-- fusion de deux demi-matrices (dongle) --\n");
    test_gauche_en_direct();
    test_droite_en_miroir();
    test_les_deux_ensemble();
    test_plafond_respecte();
    test_sans_miroir_est_un_decalage();
}
