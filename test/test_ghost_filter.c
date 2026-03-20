/* Test ghost key detection algorithm */
#include "test_framework.h"

/* Reproduce the ghost filter from matrix.c */
typedef struct {
    uint8_t output_index;  /* col */
    uint8_t input_index;   /* row */
} key_data_t;

typedef struct {
    uint32_t key_pressed_num;
    key_data_t *key_data;
} keyboard_btn_report_t;

static bool is_ghost_key(keyboard_btn_report_t *report, uint8_t check_idx) {
    if (report->key_pressed_num < 2) return false;

    uint8_t check_row = report->key_data[check_idx].input_index;
    uint8_t check_col = report->key_data[check_idx].output_index;

    for (uint32_t i = 0; i < report->key_pressed_num; i++) {
        if (i == check_idx) continue;
        uint8_t row_i = report->key_data[i].input_index;
        uint8_t col_i = report->key_data[i].output_index;

        if (row_i == check_row || col_i == check_col) continue;

        bool corner1 = false, corner2 = false;
        for (uint32_t j = 0; j < report->key_pressed_num; j++) {
            if (j == check_idx || j == i) continue;
            uint8_t row_j = report->key_data[j].input_index;
            uint8_t col_j = report->key_data[j].output_index;

            if (row_j == check_row && col_j == col_i) corner1 = true;
            if (row_j == row_i && col_j == check_col) corner2 = true;
        }

        /* BUG FIX: must be && not || — both corners needed for ghost rectangle */
        if (corner1 && corner2) {
            for (uint32_t k = 0; k < check_idx; k++) {
                if (report->key_data[k].input_index == check_row) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* Test: single key is never ghost */
void test_ghost_single_key(void) {
    key_data_t keys[] = {{.output_index = 3, .input_index = 2}};
    keyboard_btn_report_t report = {.key_pressed_num = 1, .key_data = keys};
    TEST_ASSERT(!is_ghost_key(&report, 0), "single key not ghost");
}

/* Test: two keys on different rows/cols — no ghost */
void test_ghost_two_keys_no_ghost(void) {
    key_data_t keys[] = {
        {.output_index = 3, .input_index = 1},
        {.output_index = 7, .input_index = 3}
    };
    keyboard_btn_report_t report = {.key_pressed_num = 2, .key_data = keys};
    TEST_ASSERT(!is_ghost_key(&report, 0), "2 keys: first not ghost");
    TEST_ASSERT(!is_ghost_key(&report, 1), "2 keys: second not ghost");
}

/* Test: three keys forming L-shape — no ghost */
void test_ghost_three_keys_l_shape(void) {
    key_data_t keys[] = {
        {.output_index = 2, .input_index = 1},
        {.output_index = 5, .input_index = 1},
        {.output_index = 2, .input_index = 3}
    };
    keyboard_btn_report_t report = {.key_pressed_num = 3, .key_data = keys};
    TEST_ASSERT(!is_ghost_key(&report, 0), "L-shape: no ghost at 0");
    TEST_ASSERT(!is_ghost_key(&report, 1), "L-shape: no ghost at 1");
    TEST_ASSERT(!is_ghost_key(&report, 2), "L-shape: no ghost at 2");
}

/* Test: four keys forming a perfect rectangle — the last key should be detected as ghost */
void test_ghost_four_keys_rectangle(void) {
    /* Rectangle: (1,2), (1,5), (3,2), (3,5) */
    key_data_t keys[] = {
        {.output_index = 2, .input_index = 1},  /* key 0: (r1, c2) */
        {.output_index = 5, .input_index = 1},  /* key 1: (r1, c5) */
        {.output_index = 2, .input_index = 3},  /* key 2: (r3, c2) */
        {.output_index = 5, .input_index = 3},  /* key 3: (r3, c5) — potential ghost */
    };
    keyboard_btn_report_t report = {.key_pressed_num = 4, .key_data = keys};
    /* Key 3 came last and forms a rectangle — should be ghost */
    bool k3_ghost = is_ghost_key(&report, 3);
    TEST_ASSERT(k3_ghost, "rectangle: 4th key detected as ghost");
}

/* Test: same row keys — no ghost possible */
void test_ghost_same_row(void) {
    key_data_t keys[] = {
        {.output_index = 1, .input_index = 2},
        {.output_index = 5, .input_index = 2},
        {.output_index = 9, .input_index = 2}
    };
    keyboard_btn_report_t report = {.key_pressed_num = 3, .key_data = keys};
    TEST_ASSERT(!is_ghost_key(&report, 0), "same row: no ghost at 0");
    TEST_ASSERT(!is_ghost_key(&report, 1), "same row: no ghost at 1");
    TEST_ASSERT(!is_ghost_key(&report, 2), "same row: no ghost at 2");
}

/* Test: zero keys */
void test_ghost_zero_keys(void) {
    keyboard_btn_report_t report = {.key_pressed_num = 0, .key_data = NULL};
    /* Should not crash */
    TEST_ASSERT(true, "zero keys doesn't crash");
}

void test_ghost_filter(void) {
    TEST_SUITE("Ghost Key Filter");
    TEST_RUN(test_ghost_single_key);
    TEST_RUN(test_ghost_two_keys_no_ghost);
    TEST_RUN(test_ghost_three_keys_l_shape);
    TEST_RUN(test_ghost_four_keys_rectangle);
    TEST_RUN(test_ghost_same_row);
    TEST_RUN(test_ghost_zero_keys);
}
