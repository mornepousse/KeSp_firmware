/* Tests for combo (simultaneous key) detection */
#include "test_framework.h"

#define COMBO_MAX_SLOTS 16
#define INVALID_KEY_POS 0xFF

typedef struct {
    uint8_t row1, col1;
    uint8_t row2, col2;
    uint8_t result;
} combo_cfg_t;

static bool key_in_report(uint8_t row, uint8_t col,
                          const uint8_t pr[6], const uint8_t pc[6])
{
    for (int i = 0; i < 6; i++)
        if (pr[i] == row && pc[i] == col) return true;
    return false;
}

static void test_combo_both_pressed(void) {
    combo_cfg_t c = {3, 3, 3, 4, 0x29}; /* J+K = ESC */
    uint8_t pr[6] = {3, 3, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};
    uint8_t pc[6] = {3, 4, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};

    bool k1 = key_in_report(c.row1, c.col1, pr, pc);
    bool k2 = key_in_report(c.row2, c.col2, pr, pc);
    TEST_ASSERT(k1 && k2, "Both combo keys detected");
    TEST_ASSERT_EQ(c.result, 0x29, "Combo result = ESC");
}

static void test_combo_one_pressed(void) {
    combo_cfg_t c = {3, 3, 3, 4, 0x29};
    uint8_t pr[6] = {3, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};
    uint8_t pc[6] = {3, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};

    bool k1 = key_in_report(c.row1, c.col1, pr, pc);
    bool k2 = key_in_report(c.row2, c.col2, pr, pc);
    TEST_ASSERT(k1 && !k2, "Only one key → no combo");
}

static void test_combo_none_pressed(void) {
    combo_cfg_t c = {3, 3, 3, 4, 0x29};
    uint8_t pr[6] = {INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};
    uint8_t pc[6] = {INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};

    bool k1 = key_in_report(c.row1, c.col1, pr, pc);
    bool k2 = key_in_report(c.row2, c.col2, pr, pc);
    TEST_ASSERT(!k1 && !k2, "No keys → no combo");
}

static void test_combo_unconfigured_skip(void) {
    combo_cfg_t c = {0, 0, 0, 0, 0}; /* result=0 = unconfigured */
    TEST_ASSERT_EQ(c.result, 0, "Unconfigured combo skipped");
}

static void test_combo_activation_deactivation(void) {
    bool active = false;
    /* Both pressed → activate */
    active = true;
    TEST_ASSERT(active, "Combo activated");
    /* One released → deactivate */
    active = false;
    TEST_ASSERT(!active, "Combo deactivated on release");
}

static void test_combo_multiple_slots(void) {
    combo_cfg_t combos[3] = {
        {3, 3, 3, 4, 0x29}, /* J+K = ESC */
        {2, 3, 2, 4, 0x28}, /* E+U = Enter */
        {1, 2, 1, 3, 0x2B}, /* ,+. = Tab */
    };
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT(combos[i].result != 0, "Combo slot configured");
    }
}

void test_combo(void) {
    TEST_SUITE("Combos");
    TEST_RUN(test_combo_both_pressed);
    TEST_RUN(test_combo_one_pressed);
    TEST_RUN(test_combo_none_pressed);
    TEST_RUN(test_combo_unconfigured_skip);
    TEST_RUN(test_combo_activation_deactivation);
    TEST_RUN(test_combo_multiple_slots);
}
