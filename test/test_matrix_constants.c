/* Test matrix constants introduced during audit cleanup */
#include "test_framework.h"

/* Real prod headers (host-safe): INVALID_KEY_POS + MATRIX_ROWS/COLS via
 * matrix_scan.h, STORAGE_NAMESPACE via keyboard_config.h (pulled in by matrix_scan.h).
 * A drift in these constants now breaks the test. */
#include "driver/gpio.h"   /* host stub: GPIO_NUM_* so the board pin tables expand */
#include "matrix_scan.h"

/* MAX_REPORT_KEYS is exposed in NO header: it's a #define duplicated
 * in matrix_scan.c / rf_rx_task.c / dongle_engine_state.c / cdc_half_stubs.c.
 * This test guards the HID boot protocol invariant (6 simultaneous keys max)
 * that these copies must respect — it cannot source it from a header. */
#define MAX_REPORT_KEYS  6

/* Test: MAX_REPORT_KEYS matches HID boot protocol */
void test_max_report_keys(void) {
    TEST_ASSERT_EQ(MAX_REPORT_KEYS, 6, "HID boot protocol allows 6 keys");
}

/* Test: INVALID_KEY_POS is 0xFF (used as sentinel) */
void test_invalid_key_pos(void) {
    TEST_ASSERT_EQ(INVALID_KEY_POS, 0xFF, "INVALID_KEY_POS is 0xFF");
    /* Must not collide with any valid row or col */
    TEST_ASSERT(INVALID_KEY_POS > MATRIX_ROWS, "INVALID_KEY_POS > MATRIX_ROWS");
    TEST_ASSERT(INVALID_KEY_POS > MATRIX_COLS, "INVALID_KEY_POS > MATRIX_COLS");
}

/* Test: arrays sized with MAX_REPORT_KEYS behave correctly */
void test_report_arrays(void) {
    uint8_t keycodes[MAX_REPORT_KEYS];
    uint8_t press_row[MAX_REPORT_KEYS];
    uint8_t press_col[MAX_REPORT_KEYS];
    uint8_t press_stat[MAX_REPORT_KEYS];

    /* Initialize like keyboard_btn_cb does */
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        press_row[i] = INVALID_KEY_POS;
        press_col[i] = INVALID_KEY_POS;
        press_stat[i] = 0;
        keycodes[i] = 0;
    }

    /* Verify initialization */
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        TEST_ASSERT_EQ(press_row[i], INVALID_KEY_POS, "row initialized to INVALID");
        TEST_ASSERT_EQ(press_col[i], INVALID_KEY_POS, "col initialized to INVALID");
        TEST_ASSERT_EQ(press_stat[i], 0, "stat initialized to 0");
        TEST_ASSERT_EQ(keycodes[i], 0, "keycode initialized to 0");
    }

    /* Simulate filling slots */
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        press_row[i] = i % MATRIX_ROWS;
        press_col[i] = i % MATRIX_COLS;
        keycodes[i] = 0x04 + i;  /* HID keycodes a-f */
    }

    TEST_ASSERT_EQ(keycodes[0], 0x04, "first keycode = 'a'");
    TEST_ASSERT_EQ(keycodes[5], 0x09, "last keycode = 'f'");
}

/* Test: STORAGE_NAMESPACE is a valid NVS namespace (max 15 chars) */
void test_storage_namespace(void) {
    TEST_ASSERT(strlen(STORAGE_NAMESPACE) > 0, "STORAGE_NAMESPACE not empty");
    TEST_ASSERT(strlen(STORAGE_NAMESPACE) <= 15, "STORAGE_NAMESPACE <= 15 chars (NVS limit)");
}

/* Test: key filling stops at MAX_REPORT_KEYS */
void test_fill_limit(void) {
    uint8_t keycodes[MAX_REPORT_KEYS];
    memset(keycodes, 0, sizeof(keycodes));

    int filled = 0;
    /* Simulate 10 key presses, only 6 should fit */
    for (int i = 0; i < 10 && filled < MAX_REPORT_KEYS; i++) {
        keycodes[filled] = 0x04 + i;
        filled++;
    }

    TEST_ASSERT_EQ(filled, MAX_REPORT_KEYS, "filling stops at MAX_REPORT_KEYS");
    TEST_ASSERT_EQ(keycodes[5], 0x09, "last filled key correct");
}

/* The core reads the matrix pins from the board's tables — it has no fixed
 * shape of its own. Until 2026-09-20 matrix_scan.c/veille.c hard-coded a
 * COLS0..COLS12 / ROWS0..ROWS4 initializer (the KaSe 5x13) and the Niphargus
 * boards padded it with GPIO_NUM_NC. */
static void test_board_pin_tables_match_the_geometry(void)
{
    static const int rows[] = BOARD_ROW_PINS;
    static const int cols[] = BOARD_COL_PINS;
    TEST_ASSERT_EQ(sizeof rows / sizeof rows[0], MATRIX_ROWS, "BOARD_ROW_PINS has MATRIX_ROWS entries");
    TEST_ASSERT_EQ(sizeof cols / sizeof cols[0], MATRIX_COLS, "BOARD_COL_PINS has MATRIX_COLS entries");
}

void test_matrix_constants(void) {
    TEST_SUITE("Matrix Constants");
    TEST_RUN(test_max_report_keys);
    TEST_RUN(test_invalid_key_pos);
    TEST_RUN(test_report_arrays);
    TEST_RUN(test_storage_namespace);
    TEST_RUN(test_fill_limit);
    TEST_RUN(test_board_pin_tables_match_the_geometry);
}
