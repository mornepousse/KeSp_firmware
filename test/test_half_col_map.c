/* Keymap column of a key coming from the remote half (pure logic).
 *
 * The left indexes keymaps[layer][row][col] over 14 columns: 0-6 for its
 * own scan, 7-13 for what the right sends it. What remains is figuring out WHICH
 * DIRECTION to order the right's 7 columns in, and that is where the hardware
 * decides.
 *
 * The two halves are the same PCB flipped over. Column 0 of the left is its
 * LEFTMOST key; by symmetry, column 0 of the right is its
 * RIGHTMOST key. A naive shift (col + 7) therefore lays out the right
 * half backwards: you type the home row and out comes ";lkjh" instead of
 * "hjkl;". This is the defect found on the bench on 2026-09-07.
 *
 * The mirroring is a property of the WIRING, not of the protocol: the right
 * sends its physical coordinates and does not need to know where it is
 * mounted. The conversion therefore belongs to the master, and the flag comes from its board.h. */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"

#define COLS 7

static void test_sans_miroir_c_est_un_simple_decalage(void)
{
    /* The case of a half wired the same way as the left. */
    TEST_ASSERT(half_col_to_keymap(0, COLS, false) == 7,  "col 0 -> 7");
    TEST_ASSERT(half_col_to_keymap(6, COLS, false) == 13, "col 6 -> 13");
}

static void test_avec_miroir_l_ordre_s_inverse(void)
{
    /* THE test of this file: column 0 of the right is the rightmost
     * on the keyboard, hence the last column of the keymap. */
    TEST_ASSERT(half_col_to_keymap(0, COLS, true) == 13, "col 0 (outer edge) -> 13");
    TEST_ASSERT(half_col_to_keymap(6, COLS, true) == 7,  "col 6 (inner edge) -> 7");
    TEST_ASSERT(half_col_to_keymap(3, COLS, true) == 10, "the middle column is fixed");
}

static void test_le_mapping_reste_une_bijection(void)
{
    /* Whichever direction, the 7 remote columns must cover
     * exactly 7..13, with no gap and no collision — otherwise one key overwrites
     * another or is never reachable. */
    for (int miroir = 0; miroir <= 1; miroir++) {
        int vu[2 * COLS];
        memset(vu, 0, sizeof(vu));
        for (uint8_t c = 0; c < COLS; c++) {
            uint8_t k = half_col_to_keymap(c, COLS, miroir != 0);
            TEST_ASSERT(k >= COLS && k < 2 * COLS, "stays within the remote half");
            TEST_ASSERT(!vu[k], "no collision");
            vu[k] = 1;
        }
        for (uint8_t k = COLS; k < 2 * COLS; k++)
            TEST_ASSERT(vu[k], "no gap");
    }
}

static void test_le_miroir_est_le_decalage_de_la_colonne_symetrique(void)
{
    /* Property that ties the two branches together instead of testing them
     * separately: mirroring the half is exactly equivalent to shifting the
     * symmetric column. If either formula drifts one day, this one bites. */
    for (uint8_t c = 0; c < COLS; c++)
        TEST_ASSERT(half_col_to_keymap(c, COLS, true)
                    == half_col_to_keymap((uint8_t)(COLS - 1 - c), COLS, false),
                    "mirror(c) == shift(symmetric of c)");
}

void test_half_col_map(void)
{
    printf("\n-- keymap column of the remote half (mirror) --\n");
    test_sans_miroir_c_est_un_simple_decalage();
    test_avec_miroir_l_ordre_s_inverse();
    test_le_mapping_reste_une_bijection();
    test_le_miroir_est_le_decalage_de_la_colonne_symetrique();
}
