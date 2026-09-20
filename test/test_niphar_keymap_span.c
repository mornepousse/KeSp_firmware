/* Niphargus combined keymap contract (pure logic).
 *
 * The left half is the keyboard's only keymap engine: the spec describes it
 * as "the only one to produce a HID report", the right half being just a
 * scanner that reports its raw matrix. The left half must therefore carry
 * keycodes for all 52 keys, even though it only SCANS 26.
 *
 * Hence two notions not to confuse, which keyboard_config.h had already named
 * without ever wiring them up — KEYMAP_COLS was equal to MATRIX_COLS there
 * and used nowhere:
 *
 *   MATRIX_COLS  columns THIS board physically scans                (7)
 *   KEYMAP_COLS  columns the keymap covers, both halves             (14)
 *
 * Convention adopted: columns 0..6 = left half, 7..13 = right half.
 * The coordinate received from the right half is translated by an offset of
 * MATRIX_COLS.
 */
#include "test_framework.h"

#ifndef GPIO_NUM_0
#define GPIO_NUM_0 0
#endif
#include "../boards/niphar_left/board.h"
#include "../main/input/keyboard_config.h"

static void test_scan_reste_a_sept_colonnes(void)
{
    /* Widening the keymap must NOT widen the scan: the left half only has
     * seven columns wired, and driving beyond that would touch other
     * functions (GPIO13 = battery gauge, 14 = screen CS, 15/16 = nRF24). */
    TEST_ASSERT_EQ(MATRIX_ROWS, 4, "left half scans 4 rows");
    TEST_ASSERT_EQ(MATRIX_COLS, 7, "left half scans 7 columns, unchanged");
}

static void test_keymap_couvre_les_deux_moities(void)
{
    TEST_ASSERT_EQ(KEYMAP_COLS, 2 * MATRIX_COLS, "the keymap covers both halves");
    TEST_ASSERT_EQ(KEYMAP_COLS, 14, "i.e. 14 columns");
    /* 4 rows x 14 columns = 56 keymap positions for 52 real keys: the four
     * missing ones are the gaps in the last two rows (7/7/6/6). */
    TEST_ASSERT_EQ(MATRIX_ROWS * KEYMAP_COLS, 56, "56 keymap positions");
}

static void test_les_tailles_derivees_suivent(void)
{
    /* ⚠ these two macros are used NOWHERE today (checked with grep):
     * they are dead. We lock them onto the keymap anyway, because their name
     * promises to size a key report — and the day someone uses them, they'd
     * better cover the 52 keys rather than the 26 scanned.
     *
     * Keystroke statistics, on the other hand, follow the SCANNED matrix:
     * key_stats is sized in MATRIX_COLS, and looping over it in KEYMAP_COLS
     * produced an array overrun that the compiler caught (cdc_binary_cmds.c,
     * "iteration 7 invokes undefined behavior"). */
    TEST_ASSERT_EQ(REPORT_COUNT_BYTES, MATRIX_ROWS * KEYMAP_COLS,
                   "the stats cover the entire keymap");
    TEST_ASSERT_EQ(REPORT_LEN, MOD_LED_BYTES + MATRIX_ROWS * KEYMAP_COLS,
                   "the report covers the entire keymap");
}

void test_niphar_keymap_span(void)
{
    printf("\n-- Niphargus combined keymap (left = engine for the 52 keys) --\n");
    test_scan_reste_a_sept_colonnes();
    test_keymap_couvre_les_deux_moities();
    test_les_tailles_derivees_suivent();
}
