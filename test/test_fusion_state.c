/* Tests for fusion_state — the heart of the dongle's new work (fusion phase 1).
 *
 * The dongle receives two RAW half-matrices (PKT_TYPE_MATRIX), each carrying
 * its half identity. fusion_state routes each frame to the right half-state,
 * expires a half that has gone silent (without touching the other), and produces the
 * merged list (row, keymap column) that the engine will index — left in direct
 * columns, right in high columns via the PCB mirror.
 *
 * Design: docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md
 */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"
#include "../main/comm/rf/rf_packet.h"
#include <string.h>

/* Builds a decoded matrix frame with one key pressed. */
static rf_matrix_t mk(uint8_t half, uint8_t row, uint8_t col)
{
    rf_matrix_t m;
    memset(&m, 0, sizeof(m));
    m.half = half;
    rf_bitmap_set(m.bitmap, row, col, true);
    return m;
}

/* Is a position (row, col_keymap) in the merged list? */
static bool has(const uint8_t *rows, const uint8_t *cols, uint8_t n,
                uint8_t row, uint8_t col)
{
    for (uint8_t i = 0; i < n; i++)
        if (rows[i] == row && cols[i] == col) return true;
    return false;
}

static void test_fusion_route_et_collecte(void)
{
    fusion_state_t fs;
    memset(&fs, 0, sizeof(fs));

    /* Left: (1,2) → direct column 2. Right: (1,0) → mirrored high column. */
    rf_matrix_t l = mk(RF_HALF_LEFT, 1, 2);
    rf_matrix_t r = mk(RF_HALF_RIGHT, 1, 0);
    TEST_ASSERT(fusion_apply(&fs, &l, 1000), "left frame applied");
    TEST_ASSERT(fusion_apply(&fs, &r, 1000), "right frame applied");

    uint8_t rows[16], cols[16];
    uint8_t n = fusion_collect(&fs, RF_HALF_COLS, true, rows, cols, 16);
    TEST_ASSERT_EQ(n, 2, "two keys merged");
    TEST_ASSERT(has(rows, cols, n, 1, 2), "left in direct column 2");
    /* mirror: col 0 of the right → 2*7-1-0 = 13 */
    TEST_ASSERT(has(rows, cols, n, 1, 13), "right col 0 → keymap 13 (mirror)");
}

static void test_fusion_identite_inconnue_ignoree(void)
{
    fusion_state_t fs;
    memset(&fs, 0, sizeof(fs));
    rf_matrix_t bad = mk(5, 0, 0);   /* neither LEFT nor RIGHT */
    TEST_ASSERT(!fusion_apply(&fs, &bad, 1000), "unknown identity rejected");

    uint8_t rows[16], cols[16];
    uint8_t n = fusion_collect(&fs, RF_HALF_COLS, true, rows, cols, 16);
    TEST_ASSERT_EQ(n, 0, "nothing stored from a rejected frame");
}

static void test_fusion_timeout_par_moitie(void)
{
    fusion_state_t fs;
    memset(&fs, 0, sizeof(fs));

    /* Left at t=1000 (held), right at t=1000 then silent. */
    rf_matrix_t l = mk(RF_HALF_LEFT, 0, 0);
    rf_matrix_t r = mk(RF_HALF_RIGHT, 2, 3);
    fusion_apply(&fs, &l, 1000);
    fusion_apply(&fs, &r, 1000);

    /* The left refreshes at t=1300 (so it would expire at 1700); the right
     * goes silent after t=1000 (it expires at 1400). */
    fusion_apply(&fs, &l, 1300);

    /* At t=1450, the right exceeds 400 ms of silence but not the left. */
    bool expired = fusion_timeout(&fs, 1450, HALF_LINK_TIMEOUT_MS);
    TEST_ASSERT(expired, "one half expired → recompute signal");

    uint8_t rows[16], cols[16];
    uint8_t n = fusion_collect(&fs, RF_HALF_COLS, true, rows, cols, 16);
    TEST_ASSERT_EQ(n, 1, "only the left stays pressed");
    TEST_ASSERT(has(rows, cols, n, 0, 0), "left (0,0) still there");

    /* Second call at t=1500: the right does not re-signal, and the left
     * (silent since 1300) has not yet reached 400 ms. */
    TEST_ASSERT(!fusion_timeout(&fs, 1500, HALF_LINK_TIMEOUT_MS),
                "no double expiration signal, left still alive");
    /* At t=1800, the left in turn exceeds 400 ms of silence. */
    TEST_ASSERT(fusion_timeout(&fs, 1800, HALF_LINK_TIMEOUT_MS),
                "the left expires in turn after its silence");
    n = fusion_collect(&fs, RF_HALF_COLS, true, rows, cols, 16);
    TEST_ASSERT_EQ(n, 0, "nothing left after both halves expired");
}

void test_fusion_state(void)
{
    TEST_SUITE("Fusion state (dongle, two half-matrices)");
    test_fusion_route_et_collecte();
    test_fusion_identite_inconnue_ignoree();
    test_fusion_timeout_par_moitie();
}
