/* Test bigram tracking logic */
#include "test_framework.h"

/* Simulated bigram state */
static uint16_t bg_data[NUM_KEYS][NUM_KEYS];
static uint32_t bg_total;
static int16_t bg_last_key;

static void reset_test_bigram(void) {
    memset(bg_data, 0, sizeof(bg_data));
    bg_total = 0;
    bg_last_key = -1;
}

static uint16_t get_bigram_max(void) {
    uint16_t max = 0;
    for (int i = 0; i < NUM_KEYS; i++)
        for (int j = 0; j < NUM_KEYS; j++)
            if (bg_data[i][j] > max)
                max = bg_data[i][j];
    return max;
}

/* Simulate a keypress — mirrors the bigram tracking in keyboard_btn_cb */
static void simulate_bigram_press(uint8_t row, uint8_t col) {
    int16_t curr_idx = row * MATRIX_COLS + col;
    if (bg_last_key >= 0 && bg_last_key < NUM_KEYS) {
        if (bg_data[bg_last_key][curr_idx] < UINT16_MAX) {
            bg_data[bg_last_key][curr_idx]++;
            bg_total++;
        }
    }
    bg_last_key = curr_idx;
}

/* Test: first keypress creates no bigram */
void test_bigram_first_key(void) {
    reset_test_bigram();
    simulate_bigram_press(2, 3);
    TEST_ASSERT_EQ(bg_total, 0, "first key: no bigram");
    TEST_ASSERT_EQ(bg_last_key, 2 * MATRIX_COLS + 3, "last_key set");
}

/* Test: second keypress creates one bigram */
void test_bigram_two_keys(void) {
    reset_test_bigram();
    simulate_bigram_press(2, 3);  /* key A */
    simulate_bigram_press(1, 5);  /* key B */
    int a = 2 * MATRIX_COLS + 3;
    int b = 1 * MATRIX_COLS + 5;
    TEST_ASSERT_EQ(bg_data[a][b], 1, "A->B bigram count = 1");
    TEST_ASSERT_EQ(bg_total, 1, "total = 1");
}

/* Test: repeated sequence accumulates */
void test_bigram_repeated(void) {
    reset_test_bigram();
    int a = 2 * MATRIX_COLS + 3;
    int b = 1 * MATRIX_COLS + 5;
    for (int i = 0; i < 10; i++) {
        simulate_bigram_press(2, 3);
        simulate_bigram_press(1, 5);
    }
    /* A->B happens 10 times, B->A happens 9 times (B is followed by A on repeat) */
    TEST_ASSERT_EQ(bg_data[a][b], 10, "A->B = 10");
    TEST_ASSERT_EQ(bg_data[b][a], 9, "B->A = 9");
    TEST_ASSERT_EQ(bg_total, 19, "total = 19");
}

/* Test: key index calculation */
void test_bigram_key_index(void) {
    /* row * 13 + col */
    TEST_ASSERT_EQ(0 * 13 + 0, 0, "key (0,0) = index 0");
    TEST_ASSERT_EQ(0 * 13 + 12, 12, "key (0,12) = index 12");
    TEST_ASSERT_EQ(1 * 13 + 0, 13, "key (1,0) = index 13");
    TEST_ASSERT_EQ(4 * 13 + 12, 64, "key (4,12) = index 64");
    TEST_ASSERT_EQ(NUM_KEYS, 65, "NUM_KEYS = 65");
}

/* Test: uint16_t saturation (no overflow) */
void test_bigram_saturation(void) {
    reset_test_bigram();
    int a = 0, b = 1;
    bg_data[a][b] = UINT16_MAX - 1;
    bg_last_key = a;

    /* Simulate press that should increment to UINT16_MAX */
    simulate_bigram_press(0, 1);
    TEST_ASSERT_EQ(bg_data[a][b], UINT16_MAX, "saturates at UINT16_MAX");

    /* Next press should NOT increment past UINT16_MAX */
    bg_last_key = a;
    simulate_bigram_press(0, 1);
    TEST_ASSERT_EQ(bg_data[a][b], UINT16_MAX, "stays at UINT16_MAX");
}

/* Test: reset clears everything */
void test_bigram_reset(void) {
    reset_test_bigram();
    simulate_bigram_press(0, 0);
    simulate_bigram_press(1, 1);
    reset_test_bigram();
    TEST_ASSERT_EQ(bg_total, 0, "reset clears total");
    TEST_ASSERT_EQ(bg_last_key, -1, "reset clears last_key");
    TEST_ASSERT_EQ(get_bigram_max(), 0, "reset clears all entries");
}

/* Test: self-bigram (same key pressed twice) */
void test_bigram_self(void) {
    reset_test_bigram();
    simulate_bigram_press(2, 5);
    simulate_bigram_press(2, 5);
    int idx = 2 * MATRIX_COLS + 5;
    TEST_ASSERT_EQ(bg_data[idx][idx], 1, "self-bigram recorded");
}

void test_bigram(void) {
    TEST_SUITE("Bigram Tracking");
    TEST_RUN(test_bigram_first_key);
    TEST_RUN(test_bigram_two_keys);
    TEST_RUN(test_bigram_repeated);
    TEST_RUN(test_bigram_key_index);
    TEST_RUN(test_bigram_saturation);
    TEST_RUN(test_bigram_reset);
    TEST_RUN(test_bigram_self);
}
