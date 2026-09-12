/* Fusion de DEUX demi-matrices au dongle (logique pure) — brick « dongle fusion ».
 *
 * Design : docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md.
 *
 * En mode sans fil, les deux moitiés émettent leur matrice BRUTE au dongle, qui
 * fusionne et fait tourner le moteur. Contrairement à matrix_apply_remote (une
 * moitié locale + une distante), ici AUCUNE n'est locale : le dongle reçoit deux
 * bitmaps et doit produire les positions (row, colonne keymap) que le moteur
 * indexe. La gauche occupe les colonnes 0-6 en direct ; la droite les 7-13, en
 * miroir (même PCB retourné), via la même règle que le maître (half_col_to_keymap).
 */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"
#include "../main/comm/rf/rf_packet.h"

#define COLS 7

static void test_gauche_en_direct(void)
{
    uint8_t l[RF_HALF_BITMAP_BYTES] = {0}, r[RF_HALF_BITMAP_BYTES] = {0};
    rf_bitmap_set(l, 0, 0, true);   /* gauche (0,0) */
    rf_bitmap_set(l, 2, 6, true);   /* gauche (2,6) */
    uint8_t row[12], col[12];
    uint8_t n = fuse_halves(l, r, COLS, true, row, col, 12);
    TEST_ASSERT(n == 2, "deux touches gauche");
    /* colonnes gauche = identité 0..6 */
    bool a = (row[0]==0 && col[0]==0) || (row[1]==0 && col[1]==0);
    bool b = (row[0]==2 && col[0]==6) || (row[1]==2 && col[1]==6);
    TEST_ASSERT(a && b, "gauche en colonnes 0-6 directes");
}

static void test_droite_en_miroir(void)
{
    uint8_t l[RF_HALF_BITMAP_BYTES] = {0}, r[RF_HALF_BITMAP_BYTES] = {0};
    rf_bitmap_set(r, 0, 0, true);   /* droite colonne physique 0 → keymap 13 */
    rf_bitmap_set(r, 1, 6, true);   /* droite colonne physique 6 → keymap 7 */
    uint8_t row[12], col[12];
    uint8_t n = fuse_halves(l, r, COLS, true, row, col, 12);
    TEST_ASSERT(n == 2, "deux touches droite");
    bool a = (row[0]==0 && col[0]==13) || (row[1]==0 && col[1]==13);
    bool b = (row[0]==1 && col[0]==7)  || (row[1]==1 && col[1]==7);
    TEST_ASSERT(a && b, "droite en miroir 13..7");
}

static void test_les_deux_ensemble(void)
{
    uint8_t l[RF_HALF_BITMAP_BYTES] = {0}, r[RF_HALF_BITMAP_BYTES] = {0};
    rf_bitmap_set(l, 1, 1, true);
    rf_bitmap_set(r, 1, 1, true);
    uint8_t row[12], col[12];
    uint8_t n = fuse_halves(l, r, COLS, true, row, col, 12);
    TEST_ASSERT(n == 2, "une de chaque côté");
    /* gauche (1,1)->col1, droite (1,1)->col 13-1=12 ; jamais la même colonne */
    TEST_ASSERT(col[0] != col[1], "les deux moitiés n'entrent pas en collision");
}

static void test_plafond_respecte(void)
{
    uint8_t l[RF_HALF_BITMAP_BYTES] = {0}, r[RF_HALF_BITMAP_BYTES] = {0};
    for (uint8_t c = 0; c < COLS; c++) { rf_bitmap_set(l, 0, c, true); rf_bitmap_set(r, 0, c, true); }
    uint8_t row[6], col[6];
    uint8_t n = fuse_halves(l, r, COLS, true, row, col, 6);
    TEST_ASSERT(n == 6, "borné à max, jamais de débordement");
}

static void test_sans_miroir_est_un_decalage(void)
{
    uint8_t l[RF_HALF_BITMAP_BYTES] = {0}, r[RF_HALF_BITMAP_BYTES] = {0};
    rf_bitmap_set(r, 0, 0, true);
    uint8_t row[12], col[12];
    uint8_t n = fuse_halves(l, r, COLS, false, row, col, 12);
    TEST_ASSERT(n == 1 && row[0]==0 && col[0]==7, "sans miroir : col 0 -> 7");
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
