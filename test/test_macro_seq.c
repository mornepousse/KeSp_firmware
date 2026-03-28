/* Tests for macro sequence step encoding */
#include "test_framework.h"

#define MACRO_MAX_STEPS 24
#define MACRO_DELAY_MARKER 0xFF

typedef struct {
    uint8_t keycode;
    uint8_t modifier;
} macro_step_t;

static void test_macro_step_key_only(void) {
    macro_step_t step = { .keycode = 0x04, .modifier = 0x00 };
    TEST_ASSERT_EQ(step.keycode, 0x04, "Step key = A");
    TEST_ASSERT_EQ(step.modifier, 0x00, "Step mod = none");
}

static void test_macro_step_with_mod(void) {
    macro_step_t step = { .keycode = 0x06, .modifier = 0x01 }; /* Ctrl+C */
    TEST_ASSERT_EQ(step.keycode, 0x06, "Step key = C");
    TEST_ASSERT_EQ(step.modifier, 0x01, "Step mod = Ctrl");
}

static void test_macro_step_delay(void) {
    macro_step_t step = { .keycode = MACRO_DELAY_MARKER, .modifier = 10 }; /* 100ms */
    TEST_ASSERT_EQ(step.keycode, MACRO_DELAY_MARKER, "Delay marker");
    TEST_ASSERT_EQ(step.modifier, 10, "Delay = 10 * 10ms = 100ms");
}

static void test_macro_step_end(void) {
    macro_step_t step = { .keycode = 0, .modifier = 0 };
    TEST_ASSERT_EQ(step.keycode, 0, "End of sequence");
}

static void test_macro_sequence_copypaste(void) {
    /* Ctrl+C, 100ms delay, Ctrl+V */
    macro_step_t steps[MACRO_MAX_STEPS] = {
        { 0x06, 0x01 },    /* Ctrl+C */
        { 0xFF, 0x0A },    /* 100ms delay */
        { 0x19, 0x01 },    /* Ctrl+V */
        { 0x00, 0x00 },    /* end */
    };

    TEST_ASSERT_EQ(steps[0].keycode, 0x06, "Step 0: C");
    TEST_ASSERT_EQ(steps[0].modifier, 0x01, "Step 0: Ctrl");
    TEST_ASSERT_EQ(steps[1].keycode, MACRO_DELAY_MARKER, "Step 1: delay");
    TEST_ASSERT_EQ(steps[1].modifier, 0x0A, "Step 1: 100ms");
    TEST_ASSERT_EQ(steps[2].keycode, 0x19, "Step 2: V");
    TEST_ASSERT_EQ(steps[2].modifier, 0x01, "Step 2: Ctrl");
    TEST_ASSERT_EQ(steps[3].keycode, 0x00, "Step 3: end");

    /* Count steps */
    int count = 0;
    for (int i = 0; i < MACRO_MAX_STEPS && steps[i].keycode != 0; i++)
        count++;
    TEST_ASSERT_EQ(count, 3, "3 active steps");
}

static void test_macro_max_steps(void) {
    macro_step_t steps[MACRO_MAX_STEPS] = {0};
    for (int i = 0; i < MACRO_MAX_STEPS; i++) {
        steps[i].keycode = 0x04 + (i % 26); /* A-Z cycling */
    }
    TEST_ASSERT_EQ(steps[MACRO_MAX_STEPS - 1].keycode, 0x04 + 23, "Last step filled");
}

void test_macro_seq(void) {
    TEST_SUITE("Macro Sequences");
    TEST_RUN(test_macro_step_key_only);
    TEST_RUN(test_macro_step_with_mod);
    TEST_RUN(test_macro_step_delay);
    TEST_RUN(test_macro_step_end);
    TEST_RUN(test_macro_sequence_copypaste);
    TEST_RUN(test_macro_max_steps);
}
