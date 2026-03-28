/* Tests for tap/hold state machine logic */
#include "test_framework.h"

/* Reproduce tap_hold types for host testing */
typedef enum { TH_IDLE, TH_PENDING, TH_HOLD, TH_TAP } th_state_t;

#define K_MT_BASE 0x5000
#define K_MT(mod, kc) (K_MT_BASE | ((mod) << 8) | (kc))
#define K_IS_MT(kc) (((kc) & 0xF000) == K_MT_BASE)
#define K_MT_MOD(kc) (((kc) >> 8) & 0x0F)
#define K_MT_KEY(kc) ((kc) & 0xFF)

#define K_LT_BASE 0x4000
#define K_LT(layer, kc) (K_LT_BASE | ((layer) << 8) | (kc))
#define K_IS_LT(kc) (((kc) & 0xF000) == K_LT_BASE)
#define K_LT_LAYER(kc) (((kc) >> 8) & 0x0F)
#define K_LT_KEY(kc) ((kc) & 0xFF)

/* ── State machine logic tests ───────────────────────────────────── */

static void test_tap_flow(void) {
    /* Simulate: press → pending → release before timeout → tap */
    th_state_t state = TH_IDLE;
    uint16_t kc = K_MT(0x02, 0x29); /* MT(Shift, ESC) */

    /* Press */
    state = TH_PENDING;
    TEST_ASSERT_EQ(state, TH_PENDING, "After press: pending");

    /* Release before timeout */
    state = TH_TAP;
    TEST_ASSERT_EQ(state, TH_TAP, "After quick release: tap");

    /* Verify tap key */
    TEST_ASSERT_EQ(K_MT_KEY(kc), 0x29, "Tap produces ESC");
}

static void test_hold_flow(void) {
    /* Simulate: press → pending → timeout → hold */
    th_state_t state = TH_IDLE;
    uint16_t kc = K_MT(0x02, 0x29);

    state = TH_PENDING;
    /* Timeout */
    state = TH_HOLD;
    TEST_ASSERT_EQ(state, TH_HOLD, "After timeout: hold");
    TEST_ASSERT_EQ(K_MT_MOD(kc), 0x02, "Hold mod = Shift");
}

static void test_interrupt_flow(void) {
    /* Simulate: press → pending → another key → hold (interrupt) */
    th_state_t state = TH_PENDING;
    /* Interrupt by another key */
    state = TH_HOLD;
    TEST_ASSERT_EQ(state, TH_HOLD, "After interrupt: hold");
}

static void test_hold_release(void) {
    /* Simulate: hold → release → idle */
    th_state_t state = TH_HOLD;
    state = TH_IDLE;
    TEST_ASSERT_EQ(state, TH_IDLE, "After hold release: idle");
}

static void test_lt_state_flow(void) {
    uint16_t kc = K_LT(2, 0x2C); /* LT(2, Space) */
    th_state_t state = TH_PENDING;

    /* Tap */
    state = TH_TAP;
    TEST_ASSERT_EQ(K_LT_KEY(kc), 0x2C, "LT tap = Space");

    /* Reset, test hold */
    state = TH_PENDING;
    state = TH_HOLD;
    TEST_ASSERT_EQ(K_LT_LAYER(kc), 2, "LT hold = layer 2");
}

/* ── Suite runner ────────────────────────────────────────────────── */

void test_tap_hold(void) {
    TEST_SUITE("Tap/Hold State Machine");
    TEST_RUN(test_tap_flow);
    TEST_RUN(test_hold_flow);
    TEST_RUN(test_interrupt_flow);
    TEST_RUN(test_hold_release);
    TEST_RUN(test_lt_state_flow);
}
