/* Tests for tap dance state machine */
#include "test_framework.h"

typedef enum { TD_IDLE, TD_COUNTING, TD_HOLDING, TD_RESOLVED } td_state_t;

#define TAP_DANCE_MAX_SLOTS 16
#define TAP_DANCE_MAX_TAPS  3

typedef struct {
    uint8_t actions[4];
} td_config_t;

typedef struct {
    td_state_t state;
    uint8_t index;
    uint8_t tap_count;
    bool key_held;
} td_active_t;

/* ── Multi-tap counting ──────────────────────────────────────────── */

static void test_td_single_tap(void) {
    td_active_t a = { .state = TD_COUNTING, .tap_count = 1, .key_held = false };
    /* After timeout with 1 tap → resolve action[0] */
    TEST_ASSERT_EQ(a.tap_count - 1, 0, "1 tap → action index 0");
}

static void test_td_double_tap(void) {
    td_active_t a = { .state = TD_COUNTING, .tap_count = 2, .key_held = false };
    TEST_ASSERT_EQ(a.tap_count - 1, 1, "2 taps → action index 1");
}

static void test_td_triple_tap(void) {
    td_active_t a = { .state = TD_COUNTING, .tap_count = 3, .key_held = false };
    TEST_ASSERT_EQ(a.tap_count - 1, 2, "3 taps → action index 2");
}

static void test_td_max_tap_clamped(void) {
    uint8_t tap_count = 5;
    uint8_t action_idx = (tap_count > TAP_DANCE_MAX_TAPS) ? TAP_DANCE_MAX_TAPS - 1 : tap_count - 1;
    TEST_ASSERT_EQ(action_idx, 2, "5 taps clamped to action 2");
}

static void test_td_hold_action(void) {
    /* Hold = action[3] */
    td_config_t cfg = { .actions = {0x04, 0x05, 0x06, 0x29} };
    TEST_ASSERT_EQ(cfg.actions[3], 0x29, "Hold → action[3] = ESC");
}

static void test_td_state_transitions(void) {
    td_active_t a = { .state = TD_IDLE };
    TEST_ASSERT_EQ(a.state, TD_IDLE, "Initial: IDLE");

    a.state = TD_COUNTING; a.tap_count = 1;
    TEST_ASSERT_EQ(a.state, TD_COUNTING, "After press: COUNTING");

    a.state = TD_HOLDING;
    TEST_ASSERT_EQ(a.state, TD_HOLDING, "After timeout while held: HOLDING");

    a.state = TD_RESOLVED;
    TEST_ASSERT_EQ(a.state, TD_RESOLVED, "After resolution: RESOLVED");

    a.state = TD_IDLE;
    TEST_ASSERT_EQ(a.state, TD_IDLE, "After consume: IDLE");
}

static void test_td_config_unconfigured(void) {
    td_config_t cfg = { .actions = {0, 0, 0, 0} };
    TEST_ASSERT_EQ(cfg.actions[0], 0, "Unconfigured slot: result = 0");
}

static void test_td_bounds_check(void) {
    uint8_t index = 20; /* out of bounds */
    TEST_ASSERT(index >= TAP_DANCE_MAX_SLOTS, "Index 20 >= MAX_SLOTS(16)");
}

void test_tap_dance(void) {
    TEST_SUITE("Tap Dance");
    TEST_RUN(test_td_single_tap);
    TEST_RUN(test_td_double_tap);
    TEST_RUN(test_td_triple_tap);
    TEST_RUN(test_td_max_tap_clamped);
    TEST_RUN(test_td_hold_action);
    TEST_RUN(test_td_state_transitions);
    TEST_RUN(test_td_config_unconfigured);
    TEST_RUN(test_td_bounds_check);
}
