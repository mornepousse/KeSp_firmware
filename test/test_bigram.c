/* Test bigram tracking logic — rewired on main/input/key_stats.c */
#include "test_framework.h"
#include "key_stats.h"

/*
 * The bigram is a side effect of key_stats_record_press().
 * Each test calls reset_bigram_stats() at the start — resets the internal
 * last_key_idx to -1 and clears bigram_stats / bigram_total.
 * key_stats[][] accumulates in parallel; it is not tested here.
 */

/* Test: first keypress does not create a bigram */
static void test_bigram_first_key_no_bigram(void) {
    reset_bigram_stats();
    key_stats_record_press(2, 3);
    TEST_ASSERT_EQ(bigram_total, 0, "first keypress: no bigram");
}

/* Test: two consecutive keypresses create exactly one bigram */
static void test_bigram_two_consecutive(void) {
    reset_bigram_stats();
    int a = 2 * MATRIX_COLS + 3;
    int b = 1 * MATRIX_COLS + 5;
    key_stats_record_press(2, 3);  /* A */
    key_stats_record_press(1, 5);  /* B */
    TEST_ASSERT_EQ(bigram_stats[a][b], 1, "A->B bigram == 1");
    TEST_ASSERT_EQ(bigram_total, 1, "total == 1");
}

/* Test: repeated sequence correctly accumulates A->B and B->A */
static void test_bigram_repeated_sequence(void) {
    reset_bigram_stats();
    int a = 2 * MATRIX_COLS + 3;
    int b = 1 * MATRIX_COLS + 5;
    for (int i = 0; i < 10; i++) {
        key_stats_record_press(2, 3);
        key_stats_record_press(1, 5);
    }
    /* A->B: 10 times; B->A: 9 times (B is not followed by A on the last iteration) */
    TEST_ASSERT_EQ(bigram_stats[a][b], 10, "A->B == 10");
    TEST_ASSERT_EQ(bigram_stats[b][a], 9,  "B->A == 9");
    TEST_ASSERT_EQ(bigram_total, 19, "total == 19");
}

/* Test: saturation at UINT16_MAX — the counter never exceeds UINT16_MAX */
static void test_bigram_saturation(void) {
    reset_bigram_stats();
    /* Prepare internal last_key_idx = 0 (key 0,0) without creating a bigram */
    key_stats_record_press(0, 0);
    /* Preload the A(0)->B(1) counter to UINT16_MAX - 1 */
    bigram_stats[0][1] = UINT16_MAX - 1;
    /* Press B: must reach exactly UINT16_MAX */
    key_stats_record_press(0, 1);
    TEST_ASSERT_EQ(bigram_stats[0][1], UINT16_MAX, "reaches UINT16_MAX");
    /* Reset last_key_idx to 0 via press A, then try to increment beyond */
    key_stats_record_press(0, 0);  /* creates B->A bigram in passing, harmless */
    key_stats_record_press(0, 1);  /* A->B already at UINT16_MAX, must not increment */
    TEST_ASSERT_EQ(bigram_stats[0][1], UINT16_MAX, "stays at UINT16_MAX after saturation");
}

/* Test: self-bigram — same key twice records [idx][idx] */
static void test_bigram_self(void) {
    reset_bigram_stats();
    int idx = 2 * MATRIX_COLS + 5;
    key_stats_record_press(2, 5);
    key_stats_record_press(2, 5);
    TEST_ASSERT_EQ(bigram_stats[idx][idx], 1, "self-bigram (2,5)->(2,5) == 1");
}

/* Test: reset_bigram_stats clears the data and resets last_key_idx to -1 */
static void test_bigram_reset(void) {
    reset_bigram_stats();
    key_stats_record_press(0, 0);
    key_stats_record_press(1, 1);
    reset_bigram_stats();
    TEST_ASSERT_EQ(bigram_total, 0, "reset clears total");
    TEST_ASSERT_EQ(get_bigram_stats_max(), 0, "reset clears the counters");
    /* After reset, last_key_idx == -1: a single keypress does not create a bigram */
    key_stats_record_press(0, 0);
    TEST_ASSERT_EQ(bigram_total, 0, "first press post-reset: no bigram (last_key_idx reset to -1)");
}

void test_bigram(void) {
    TEST_SUITE("Bigram Tracking");
    TEST_RUN(test_bigram_first_key_no_bigram);
    TEST_RUN(test_bigram_two_consecutive);
    TEST_RUN(test_bigram_repeated_sequence);
    TEST_RUN(test_bigram_saturation);
    TEST_RUN(test_bigram_self);
    TEST_RUN(test_bigram_reset);
}
