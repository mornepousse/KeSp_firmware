/* Tap Dance engine tests — REAL module linked (../main/input/tap_dance.c).
 * Shared controllable clock (host_clock): we advance time to cross
 * TAP_DANCE_TIMEOUT_MS and resolve dances on the real code. */
#include "test_framework.h"
#include "tap_dance.h"
#include "host_clock.h"

static void td_reset(void) { tap_dance_init(); host_clock_reset(); }

/* Configures slot 0 with 1tap=A / 2tap=B / 3tap=C / hold=ESC. */
static void configure_slot0(void) {
    const uint8_t actions[4] = { 0x04, 0x05, 0x06, 0x29 };
    tap_dance_set(0, actions);
}

/* tap_dance_pending(): the keyboard task keeps its 10 ms tick only while a
 * dance is counting (keyboard_cadence.h, 2026-09-25). */
static void test_td_pending_pendant_le_comptage(void) {
    td_reset(); configure_slot0();
    TEST_ASSERT(!tap_dance_pending(), "idle: nothing pending");
    tap_dance_on_press(0, 0, 0);
    TEST_ASSERT(tap_dance_pending(), "pressed: counting, pending");
    tap_dance_on_release(0, 0);
    TEST_ASSERT(tap_dance_pending(), "released, waiting for another tap: still pending");
    host_clock_advance_ms(200);
    tap_dance_tick();
    TEST_ASSERT(!tap_dance_pending(), "resolved at the timeout: no longer pending");
    tap_dance_consume();
}

/* 1 tap, released, timeout -> action[0] = A. */
static void test_td_single_tap(void) {
    td_reset(); configure_slot0();
    TEST_ASSERT(tap_dance_on_press(0, 0, 0), "TD press slot 0 absorbed");
    tap_dance_on_release(0, 0);
    host_clock_advance_ms(200);            /* == TAP_DANCE_TIMEOUT_MS */
    tap_dance_tick();
    TEST_ASSERT(tap_dance_just_resolved(), "1 tap + timeout -> resolved");
    TEST_ASSERT_EQ(tap_dance_consume(), 0x04, "1 tap -> action[0] = A");
}

/* 2 fast taps, timeout -> action[1] = B. */
static void test_td_double_tap(void) {
    td_reset(); configure_slot0();
    tap_dance_on_press(0, 0, 0);           /* count=1, last_tap=0 */
    tap_dance_on_release(0, 0);
    host_clock_advance_ms(50);             /* < timeout -> no resolution */
    tap_dance_on_press(0, 0, 0);           /* count=2, last_tap=50 */
    tap_dance_on_release(0, 0);
    host_clock_advance_ms(200);            /* elapsed since last_tap = 200 */
    tap_dance_tick();
    TEST_ASSERT_EQ(tap_dance_consume(), 0x05, "2 taps -> action[1] = B");
}

/* 3 taps -> action[2] = C. */
static void test_td_triple_tap(void) {
    td_reset(); configure_slot0();
    tap_dance_on_press(0, 0, 0); tap_dance_on_release(0, 0);
    host_clock_advance_ms(50);
    tap_dance_on_press(0, 0, 0); tap_dance_on_release(0, 0);
    host_clock_advance_ms(50);
    tap_dance_on_press(0, 0, 0); tap_dance_on_release(0, 0);
    host_clock_advance_ms(200);
    tap_dance_tick();
    TEST_ASSERT_EQ(tap_dance_consume(), 0x06, "3 taps -> action[2] = C");
}

/* > MAX_TAPS -> immediate resolution on action[MAX_TAPS-1], no tick. */
static void test_td_max_taps_clamped(void) {
    td_reset(); configure_slot0();
    for (int i = 0; i < TAP_DANCE_MAX_TAPS + 1; i++) {   /* 4 presses */
        tap_dance_on_press(0, 0, 0);
        tap_dance_on_release(0, 0);
    }
    TEST_ASSERT_EQ(tap_dance_consume(), 0x06,
                   "4 taps (> MAX) -> clamp action[MAX_TAPS-1] = C");
}

/* Held past the timeout -> hold action = action[3] = ESC. */
static void test_td_hold_action(void) {
    td_reset(); configure_slot0();
    tap_dance_on_press(0, 0, 0);           /* key_held = true, no release */
    host_clock_advance_ms(200);
    tap_dance_tick();                      /* held + timeout -> HOLDING -> resolve(3) */
    TEST_ASSERT(tap_dance_just_resolved(), "hold + timeout -> resolved");
    TEST_ASSERT_EQ(tap_dance_consume(), 0x29, "hold -> action[3] = ESC");
}

/* Unconfigured slot (all-zero actions) -> nothing resolved, consume = 0. */
static void test_td_unconfigured_slot(void) {
    td_reset();
    const uint8_t zero[4] = { 0, 0, 0, 0 };
    tap_dance_set(1, zero);
    tap_dance_on_press(1, 0, 0);
    tap_dance_on_release(0, 0);
    host_clock_advance_ms(200);
    tap_dance_tick();
    TEST_ASSERT(!tap_dance_just_resolved(), "unconfigured slot -> no resolution");
    TEST_ASSERT_EQ(tap_dance_consume(), 0, "unconfigured slot -> consume = 0");
}

/* Out-of-bounds index -> on_press refused. */
static void test_td_bounds_reject(void) {
    td_reset();
    TEST_ASSERT(!tap_dance_on_press(TAP_DANCE_MAX_SLOTS, 0, 0),
                "index == MAX_SLOTS -> refused");
    TEST_ASSERT(!tap_dance_on_press(20, 0, 0), "index 20 -> refused");
}

/* Another dance (different index) during COUNTING -> rejected. */
static void test_td_different_key_rejected(void) {
    td_reset(); configure_slot0();
    TEST_ASSERT(tap_dance_on_press(0, 0, 0), "dance 0 started");
    TEST_ASSERT(!tap_dance_on_press(1, 0, 1),
                "other index during COUNTING -> rejected");
    tap_dance_on_release(0, 0);
}

/* 4th tap resolves in on_press; a tick landing before consumption must NOT
 * clear the resolution (M2 audit: tick was blindly setting resolved_flag=false
 * -> the 3-tap keycode was never sent). */
static void test_td_max_taps_survives_tick(void) {
    td_reset(); configure_slot0();
    for (int i = 0; i < TAP_DANCE_MAX_TAPS + 1; i++) {   /* 4 presses -> resolves in on_press */
        tap_dance_on_press(0, 0, 0);
        tap_dance_on_release(0, 0);
    }
    tap_dance_tick();   /* a tick lands before the consumer reads the resolution */
    TEST_ASSERT(tap_dance_just_resolved(),
                "4th tap resolved in on_press survives a tick (M2)");
    TEST_ASSERT_EQ(tap_dance_consume(), 0x06,
                   "4 taps -> action[2]=C consumed even after a tick");
}

/* Interruption by another key during a count -> resolves the current dance
 * at its current tap count (QMK logic), on top of rejecting the new key (M9). */
static void test_td_interrupt_resolves_current(void) {
    td_reset(); configure_slot0();
    tap_dance_on_press(0, 0, 0);           /* dance 0, tap_count=1 */
    tap_dance_on_release(0, 0);
    tap_dance_on_press(0, 0, 0);           /* tap_count=2 */
    /* ANOTHER key (index 1) interrupts before the timeout */
    TEST_ASSERT(!tap_dance_on_press(1, 0, 1), "other key -> rejected (return false)");
    TEST_ASSERT(tap_dance_just_resolved(), "interruption -> current dance resolved (M9)");
    TEST_ASSERT_EQ(tap_dance_consume(), 0x05, "2 interrupted taps -> action[1]=B");
}

void test_tap_dance(void) {
    TEST_SUITE("Tap Dance — real module");
    TEST_RUN(test_td_pending_pendant_le_comptage);
    TEST_RUN(test_td_single_tap);
    TEST_RUN(test_td_interrupt_resolves_current);
    TEST_RUN(test_td_max_taps_survives_tick);
    TEST_RUN(test_td_double_tap);
    TEST_RUN(test_td_triple_tap);
    TEST_RUN(test_td_max_taps_clamped);
    TEST_RUN(test_td_hold_action);
    TEST_RUN(test_td_unconfigured_slot);
    TEST_RUN(test_td_bounds_reject);
    TEST_RUN(test_td_different_key_rejected);
    tap_dance_init();   /* leaves the module clean */
}
