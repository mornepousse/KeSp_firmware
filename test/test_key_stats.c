/* Test key statistics logic — rewired on main/input/key_stats.c */
#include "test_framework.h"
#include "key_stats.h"

/*
 * Each test calls reset_key_stats() at the start to isolate state.
 * reset_key_stats() calls save_key_stats(), which is a no-op under TEST_HOST.
 */

/* Test: record_press increments the right cell and the total */
static void test_key_stats_record_press(void) {
    reset_key_stats();
    key_stats_record_press(2, 3);
    key_stats_record_press(2, 3);
    key_stats_record_press(2, 3);
    TEST_ASSERT_EQ(get_key_stats_val(2, 3), 3, "key (2,3) pressed 3 times == 3");
    TEST_ASSERT_EQ(key_stats_total, 3, "total == 3");
}

/* Test: several different keys accumulate independently */
static void test_key_stats_multiple_keys(void) {
    reset_key_stats();
    key_stats_record_press(0, 0);
    key_stats_record_press(1, 5);
    key_stats_record_press(1, 5);
    key_stats_record_press(4, 12);
    TEST_ASSERT_EQ(get_key_stats_val(0, 0),  1, "key (0,0) == 1");
    TEST_ASSERT_EQ(get_key_stats_val(1, 5),  2, "key (1,5) == 2");
    TEST_ASSERT_EQ(get_key_stats_val(4, 12), 1, "key (4,12) == 1");
    TEST_ASSERT_EQ(key_stats_total, 4, "total == 4");
}

/* Test: get_key_stats_max returns the maximum over the matrix */
static void test_key_stats_max(void) {
    reset_key_stats();
    key_stats_record_press(0, 0);
    for (int i = 0; i < 100; i++) key_stats_record_press(2, 5);
    key_stats_record_press(3, 7);
    TEST_ASSERT_EQ(get_key_stats_max(), 100, "max == 100");
}

/* Test: reset_key_stats clears all cells and the total */
static void test_key_stats_reset(void) {
    reset_key_stats();
    key_stats_record_press(1, 1);
    key_stats_record_press(2, 2);
    reset_key_stats();
    TEST_ASSERT_EQ(get_key_stats_val(1, 1), 0, "reset clears key (1,1)");
    TEST_ASSERT_EQ(get_key_stats_val(2, 2), 0, "reset clears key (2,2)");
    TEST_ASSERT_EQ(key_stats_total, 0, "reset clears total");
    TEST_ASSERT_EQ(get_key_stats_max(), 0, "max == 0 after reset");
}

/* Test: OOB keystrokes ignored (row >= MATRIX_ROWS or col >= MATRIX_COLS) */
static void test_key_stats_bounds_ignored(void) {
    reset_key_stats();
    key_stats_record_press(MATRIX_ROWS, 0);
    key_stats_record_press(0, MATRIX_COLS);
    TEST_ASSERT_EQ(key_stats_total, 0, "OOB keystrokes do not increment the total");
    TEST_ASSERT_EQ(get_key_stats_max(), 0, "OOB keystrokes do not increment the max");
}

/* Test: get_key_stats_val returns 0 for OOB indices */
static void test_key_stats_val_oob(void) {
    TEST_ASSERT_EQ(get_key_stats_val(MATRIX_ROWS, 0), 0, "val OOB row == 0");
    TEST_ASSERT_EQ(get_key_stats_val(0, MATRIX_COLS), 0, "val OOB col == 0");
}

void test_key_stats(void) {
    TEST_SUITE("Key Statistics");
    TEST_RUN(test_key_stats_record_press);
    TEST_RUN(test_key_stats_multiple_keys);
    TEST_RUN(test_key_stats_max);
    TEST_RUN(test_key_stats_reset);
    TEST_RUN(test_key_stats_bounds_ignored);
    TEST_RUN(test_key_stats_val_oob);
}
