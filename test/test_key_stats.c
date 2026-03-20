/* Test key statistics logic */
#include "test_framework.h"

/* Simulated key_stats storage */
static uint32_t ks_data[MATRIX_ROWS][MATRIX_COLS];
static uint32_t ks_data_total;

static void reset_test_stats(void) {
    memset(ks_data, 0, sizeof(ks_data));
    ks_data_total = 0;
}

static uint32_t get_stats_max(void) {
    uint32_t max = 0;
    for (int r = 0; r < MATRIX_ROWS; r++)
        for (int c = 0; c < MATRIX_COLS; c++)
            if (ks_data[r][c] > max)
                max = ks_data[r][c];
    return max;
}

/* Simulate a keypress at (row, col) — mirrors callback logic */
static void simulate_keypress(uint8_t row, uint8_t col) {
    if (row < MATRIX_ROWS && col < MATRIX_COLS) {
        ks_data[row][col]++;
        ks_data_total++;
    }
}

/* Test: basic stats accumulation */
void test_stats_basic(void) {
    reset_test_stats();
    simulate_keypress(2, 3);
    simulate_keypress(2, 3);
    simulate_keypress(2, 3);
    TEST_ASSERT_EQ(ks_data[2][3], 3, "key pressed 3 times");
    TEST_ASSERT_EQ(ks_data_total, 3, "total is 3");
}

/* Test: multiple keys */
void test_stats_multiple_keys(void) {
    reset_test_stats();
    simulate_keypress(0, 0);
    simulate_keypress(1, 5);
    simulate_keypress(1, 5);
    simulate_keypress(4, 12);
    TEST_ASSERT_EQ(ks_data[0][0], 1, "key (0,0) pressed once");
    TEST_ASSERT_EQ(ks_data[1][5], 2, "key (1,5) pressed twice");
    TEST_ASSERT_EQ(ks_data[4][12], 1, "key (4,12) pressed once");
    TEST_ASSERT_EQ(ks_data_total, 4, "total is 4");
}

/* Test: max calculation */
void test_stats_max(void) {
    reset_test_stats();
    simulate_keypress(0, 0);
    for (int i = 0; i < 100; i++) simulate_keypress(2, 5);
    simulate_keypress(3, 7);
    TEST_ASSERT_EQ(get_stats_max(), 100, "max is 100");
}

/* Test: reset clears everything */
void test_stats_reset(void) {
    reset_test_stats();
    simulate_keypress(1, 1);
    simulate_keypress(2, 2);
    reset_test_stats();
    TEST_ASSERT_EQ(ks_data[1][1], 0, "reset clears key (1,1)");
    TEST_ASSERT_EQ(ks_data[2][2], 0, "reset clears key (2,2)");
    TEST_ASSERT_EQ(ks_data_total, 0, "reset clears total");
    TEST_ASSERT_EQ(get_stats_max(), 0, "max after reset is 0");
}

/* Test: bounds check */
void test_stats_bounds(void) {
    reset_test_stats();
    simulate_keypress(MATRIX_ROWS, 0);  /* out of bounds row */
    simulate_keypress(0, MATRIX_COLS);  /* out of bounds col */
    TEST_ASSERT_EQ(ks_data_total, 0, "OOB presses ignored");
}

/* Test: save threshold logic (simulates key_stats_check_save) */
void test_stats_save_threshold(void) {
    reset_test_stats();
    uint32_t last_saved_total = 0;

    /* Simulate 99 keypresses — should not trigger save */
    for (int i = 0; i < 99; i++) simulate_keypress(0, 0);
    uint32_t diff = ks_data_total - last_saved_total;
    TEST_ASSERT(diff < 100, "99 presses: no save trigger");

    /* One more — should trigger */
    simulate_keypress(0, 0);
    diff = ks_data_total - last_saved_total;
    TEST_ASSERT(diff >= 100, "100 presses: save triggered");
}

void test_key_stats(void) {
    TEST_SUITE("Key Statistics");
    TEST_RUN(test_stats_basic);
    TEST_RUN(test_stats_multiple_keys);
    TEST_RUN(test_stats_max);
    TEST_RUN(test_stats_reset);
    TEST_RUN(test_stats_bounds);
    TEST_RUN(test_stats_save_threshold);
}
