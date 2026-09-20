/* State of the remote half as seen by the master (pure logic).
 *
 * The left half merges its own matrix with the one the right half sends it
 * over radio. Three things must be right, and the third is the most
 * dangerous:
 *
 *   1. what is received is reproduced as-is;
 *   2. a newer packet replaces the previous one — the state is absolute, not
 *      differential, so a lost frame is caught up by the next one;
 *   3. WHEN THE LINK GOES SILENT, KEYS RELEASE. Without this a half that
 *      goes out of range or whose battery dies leaves the host on the last
 *      state received — and if it was "Shift held", it stays that way. rf_slot.h
 *      documents this fallback and warns that a mistargeted release would be
 *      worse than the problem: only what THIS half was holding gets released.
 */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"

static void bitmap_avec(uint8_t *bm, uint8_t r, uint8_t c)
{
    memset(bm, 0, RF_HALF_BITMAP_BYTES);
    rf_bitmap_set(bm, r, c, true);
}

static void test_etat_neuf_est_vide(void)
{
    half_state_t st = {0};
    for (uint8_t r = 0; r < RF_HALF_ROWS; r++)
        for (uint8_t c = 0; c < RF_HALF_COLS; c++)
            TEST_ASSERT(!half_state_pressed(&st, r, c), "nothing pressed at the start");
}

static void test_ce_qui_est_recu_est_restitue(void)
{
    half_state_t st = {0};
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    bitmap_avec(bm, 2, 5);
    half_state_recu(&st, bm, 1000);
    TEST_ASSERT(half_state_pressed(&st, 2, 5), "the received key is pressed");
    TEST_ASSERT(!half_state_pressed(&st, 2, 4), "its neighbor is not");
    TEST_ASSERT(!half_state_pressed(&st, 1, 5), "nor the one above");
}

static void test_un_paquet_remplace_le_precedent(void)
{
    /* The state is absolute: a lost frame is caught up by the next one, with
     * no accumulation or drift. */
    half_state_t st = {0};
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    bitmap_avec(bm, 0, 0);
    half_state_recu(&st, bm, 1000);
    bitmap_avec(bm, 3, 6);
    half_state_recu(&st, bm, 1010);
    TEST_ASSERT(half_state_pressed(&st, 3, 6), "the new key is pressed");
    TEST_ASSERT(!half_state_pressed(&st, 0, 0), "the old one is released");
}

static void test_le_silence_relache_tout(void)
{
    /* THE test of this file. */
    half_state_t st = {0};
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    bitmap_avec(bm, 1, 1);
    half_state_recu(&st, bm, 1000);

    TEST_ASSERT(!half_state_timeout(&st, 1200, 500), "not expired yet (200 < 500)");
    TEST_ASSERT(half_state_pressed(&st, 1, 1), "the key still holds");

    TEST_ASSERT(half_state_timeout(&st, 1600, 500), "expired (600 > 500) -> change");
    TEST_ASSERT(!half_state_pressed(&st, 1, 1), "everything is released");
}

static void test_expiration_ne_se_repete_pas(void)
{
    /* A second call must NOT re-signal a change: the engine would
     * release already-released keys on every cycle. */
    half_state_t st = {0};
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    bitmap_avec(bm, 1, 1);
    half_state_recu(&st, bm, 1000);
    TEST_ASSERT(half_state_timeout(&st, 1600, 500), "first expiration signaled");
    TEST_ASSERT(!half_state_timeout(&st, 1700, 500), "the second signals nothing");
    TEST_ASSERT(!half_state_timeout(&st, 9999, 500), "nor the following ones");
}

static void test_le_lien_peut_revenir(void)
{
    half_state_t st = {0};
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    bitmap_avec(bm, 1, 1);
    half_state_recu(&st, bm, 1000);
    half_state_timeout(&st, 1600, 500);
    /* The half comes back: it must be heard again. */
    bitmap_avec(bm, 2, 2);
    half_state_recu(&st, bm, 2000);
    TEST_ASSERT(half_state_pressed(&st, 2, 2), "the returned half is heard");
    TEST_ASSERT(!half_state_timeout(&st, 2100, 500), "and does not expire right away");
}

void test_half_state(void)
{
    printf("\n-- state of the remote half (fusion + fallback) --\n");
    test_etat_neuf_est_vide();
    test_ce_qui_est_recu_est_restitue();
    test_un_paquet_remplace_le_precedent();
    test_le_silence_relache_tout();
    test_expiration_ne_se_repete_pas();
    test_le_lien_peut_revenir();
}
