/* Size of the keymaps blob in NVS — the SAME on both sides.
 *
 * On 2026-09-07: the remapping software wrote 1120 bytes
 * (LAYERS x ROWS x KEYMAP_COLS x 2, i.e. 14 columns on the master) and the
 * firmware read back 560 (MATRIX_COLS, 7). load_keymaps' size guard
 * therefore rejected the blob at EVERY boot and kept the factory values:
 * the user remapped, and their work disappeared on the next reboot
 * with no error visible anywhere.
 *
 * Both paths now go through KEYMAP_BLOB_BYTES. This test locks in
 * what distinguishes the two formulas: on a master board, where KEYMAP_COLS
 * covers BOTH halves, the size must absolutely not follow MATRIX_COLS. */
#include "test_framework.h"
#include "../main/input/keymap.h"
#include "../main/input/keyboard_config.h"

static void test_la_taille_suit_le_tableau_reel(void)
{
    uint16_t km[LAYERS][MATRIX_ROWS][KEYMAP_COLS];
    TEST_ASSERT(KEYMAP_BLOB_BYTES == sizeof(km),
                "the blob is exactly the size of the keymaps array");
}

static void test_la_taille_ne_suit_PAS_la_matrice_locale(void)
{
    /* THE test of this file. It only bites on a board whose keymap
     * covers more than its own matrix — the master of a split — but that is
     * precisely the only case where the confusion was possible. */
    size_t selon_matrice_locale =
        (size_t)LAYERS * MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t);
    if (KEYMAP_COLS != MATRIX_COLS)
        TEST_ASSERT(KEYMAP_BLOB_BYTES != selon_matrice_locale,
                    "the size is not computed from the local matrix");
    else
        TEST_ASSERT(KEYMAP_BLOB_BYTES == selon_matrice_locale,
                    "on a non-split board, the two coincide");
}

static void test_le_blob_couvre_toutes_les_couches(void)
{
    TEST_ASSERT(KEYMAP_BLOB_BYTES ==
                    (size_t)LAYERS * MATRIX_ROWS * KEYMAP_COLS * sizeof(uint16_t),
                "all layers, all rows, all columns");
}

void test_keymap_blob_size(void)
{
    printf("\n-- size of the keymaps blob (NVS) --\n");
    test_la_taille_suit_le_tableau_reel();
    test_la_taille_ne_suit_PAS_la_matrice_locale();
    test_le_blob_couvre_toutes_les_couches();
}
