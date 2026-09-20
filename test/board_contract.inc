/* Generic board contract — included once per board by test_board_contract_<name>.c
 * after: the host GPIO stub, the board's board.h, and
 *   #define BOARD_CONTRACT_NAME "<name>"
 *   #define BOARD_CONTRACT_FN   test_board_contract_<name>
 * Every board is checked the same way from its BOARD_PINS(X) list; the
 * hardware-specific facts (which GPIO is which) stay in the board's own pin
 * test. A legacy board may waive the strapping rule with
 * BOARD_PINS_WAIVE_STRAPPING (KaSe V1/V2 drive GPIO3/45/46 in their matrix —
 * boot-safe by wiring, decided long before this contract). */
static const int bc_pins[] = {
#define X(p) p,
    BOARD_PINS(X)
#undef X
};
#define BC_N (sizeof bc_pins / sizeof bc_pins[0])

static int bc_strapping(int g) { return g == 0 || g == 3 || g == 45 || g == 46; }

static void bc_no_duplicate(void)
{
    for (unsigned i = 0; i < BC_N; i++)
        for (unsigned j = i + 1; j < BC_N; j++)
            TEST_ASSERT(bc_pins[i] != bc_pins[j], BOARD_CONTRACT_NAME ": a GPIO is used twice");
}
static void bc_valid_numbers(void)
{
    for (unsigned i = 0; i < BC_N; i++)
        TEST_ASSERT(bc_pins[i] >= 0 && bc_pins[i] <= 48, BOARD_CONTRACT_NAME ": GPIO out of the ESP32-S3 range (0..48)");
}
static void bc_no_strapping(void)
{
#ifdef BOARD_PINS_WAIVE_STRAPPING
    TEST_ASSERT(1, BOARD_CONTRACT_NAME ": strapping rule waived by the board (legacy pinout)");
#else
    for (unsigned i = 0; i < BC_N; i++)
        TEST_ASSERT(!bc_strapping(bc_pins[i]), BOARD_CONTRACT_NAME ": strapping GPIO (0/3/45/46) in use");
#endif
}
static void bc_no_reserved(void)
{
    for (unsigned i = 0; i < BC_N; i++) {
#ifdef BOARD_USES_NATIVE_USB
        TEST_ASSERT(bc_pins[i] != 19 && bc_pins[i] != 20, BOARD_CONTRACT_NAME ": GPIO19/20 are the native USB");
#endif
#ifdef BOARD_PSRAM_OCTAL
        TEST_ASSERT(!(bc_pins[i] >= 35 && bc_pins[i] <= 37), BOARD_CONTRACT_NAME ": GPIO35-37 belong to the octal PSRAM");
#endif
    }
    (void)bc_pins;
}
#ifdef BOARD_ROW_PINS   /* a physical matrix (the dongle declares MATRIX_ROWS for its engine, without pins) */
static void bc_geometry(void)
{
    static const int r[] = BOARD_ROW_PINS;
    static const int c[] = BOARD_COL_PINS;
    TEST_ASSERT_EQ(sizeof r / sizeof r[0], MATRIX_ROWS, BOARD_CONTRACT_NAME ": BOARD_ROW_PINS vs MATRIX_ROWS");
    TEST_ASSERT_EQ(sizeof c / sizeof c[0], MATRIX_COLS, BOARD_CONTRACT_NAME ": BOARD_COL_PINS vs MATRIX_COLS");
#ifdef KEYMAP_COLS
    TEST_ASSERT(KEYMAP_COLS >= MATRIX_COLS, BOARD_CONTRACT_NAME ": KEYMAP_COLS covers the matrix");
#endif
}
#endif

void BOARD_CONTRACT_FN(void)
{
    TEST_SUITE("board contract: " BOARD_CONTRACT_NAME);
    TEST_RUN(bc_no_duplicate);
    TEST_RUN(bc_valid_numbers);
    TEST_RUN(bc_no_strapping);
    TEST_RUN(bc_no_reserved);
#ifdef BOARD_ROW_PINS   /* a physical matrix (the dongle declares MATRIX_ROWS for its engine, without pins) */
    TEST_RUN(bc_geometry);
#endif
}
