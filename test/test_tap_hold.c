/* Tap/Hold engine tests — REAL linked module (../main/input/tap_hold.c).
 * The clock is controllable: esp_timer_get_time() (defined here, a global symbol of
 * the runner) returns g_now_us, advanced via advance_ms() — lets us test the exact
 * timeout boundary, interruption, slot exhaustion, etc. on the real code. */
#include "test_framework.h"
#include "tap_hold.h"
#include "key_definitions.h"   /* K_MT / K_LT / K_OSM, MOD_* */
#include "keyboard_config.h"   /* LAYERS */
#include "key_features.h"      /* osm_is_active / osm_consume (branche OSM du tap) */

/* Shared controllable host clock (host_clock.c defines esp_timer_get_time) */
#include "host_clock.h"
static void advance_ms(uint32_t ms) { host_clock_advance_ms(ms); }

/* Layer globals defined by key_processor.c (linked) — saved/restored
 * around the LT test so as not to pollute the other suites. */
extern uint8_t current_layout;
extern uint8_t last_layer;

static void th_reset(void) { tap_hold_init(); host_clock_reset(); }

/* 1. MT released before the timeout → TAP: consume_tap returns the tap key. */
static void test_th_mt_tap(void) {
    th_reset();
    uint16_t mt = K_MT(MOD_LSFT, 0x04);   /* MT(Shift, A) */
    TEST_ASSERT(tap_hold_on_press(mt, 0, 0), "MT press → tracked");
    advance_ms(50);                        /* < 200ms */
    TEST_ASSERT(tap_hold_on_release(0, 0), "MT release tracked");
    TEST_ASSERT_EQ(tap_hold_consume_tap(), 0x04, "quick release → tap = A (0x04)");
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), 0, "no hold mod after a tap");
}

/* 2. MT held past the timeout → HOLD: the mod becomes active; release removes it. */
static void test_th_mt_hold_timeout(void) {
    th_reset();
    uint16_t mt = K_MT(MOD_LSFT, 0x04);
    tap_hold_on_press(mt, 0, 0);
    advance_ms(200);                       /* == TAP_HOLD_TIMEOUT_MS */
    tap_hold_tick();
    TEST_ASSERT(tap_hold_hold_just_activated(), "tick at timeout → hold activated");
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), MOD_LSFT, "MT hold → Shift active");
    bool is_hold = false;
    TEST_ASSERT_EQ(tap_hold_get_resolved(0, 0, &is_hold), mt, "resolved returns the MT keycode");
    TEST_ASSERT(is_hold, "resolved → is_hold=true");
    tap_hold_on_release(0, 0);
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), 0, "hold release → mod removed");
}

/* 3. Exact timeout boundary: 199ms → no hold; 200ms → hold. */
static void test_th_timeout_boundary(void) {
    th_reset();
    tap_hold_on_press(K_MT(MOD_LCTL, 0x05), 0, 0);
    advance_ms(199);
    tap_hold_tick();
    TEST_ASSERT(!tap_hold_hold_just_activated(), "199ms < timeout → no hold");
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), 0, "199ms → no mod");
    advance_ms(1);                         /* total 200ms */
    tap_hold_tick();
    TEST_ASSERT(tap_hold_hold_just_activated(), "200ms == timeout → hold");
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), MOD_LCTL, "200ms → Ctrl active");
    tap_hold_on_release(0, 0);
}

/* 4. Interruption (another key) → immediate HOLD, without waiting for the timeout. */
static void test_th_interrupt_forces_hold(void) {
    th_reset();
    tap_hold_on_press(K_MT(MOD_LALT, 0x06), 0, 0);
    advance_ms(10);                        /* well before the timeout */
    tap_hold_interrupt();
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), MOD_LALT, "interrupt → immediate Alt hold");
    tap_hold_on_release(0, 0);
}

/* 5. LT held → active layer; release → restored. */
static void test_th_lt_hold_layer(void) {
    th_reset();
    uint8_t save_cur = current_layout, save_last = last_layer;
    current_layout = 0;
    tap_hold_on_press(K_LT(2, 0x2C), 0, 0);   /* LT(2, Space) */
    advance_ms(200);
    tap_hold_tick();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 2, "LT hold → active layer 2");
    tap_hold_on_release(0, 0);
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), -1, "LT release → no more active layer");
    current_layout = save_cur; last_layer = save_last;
}

/* 6. OSM tapped → consume_tap arms the one-shot mod (no direct keycode). */
static void test_th_osm_tap_arms(void) {
    th_reset();
    (void)osm_consume();                   /* empty OSM baseline */
    tap_hold_on_press(K_OSM(MOD_LGUI), 0, 0);
    advance_ms(30);
    tap_hold_on_release(0, 0);
    TEST_ASSERT_EQ(tap_hold_consume_tap(), 0, "OSM tap returns no direct keycode");
    TEST_ASSERT(osm_is_active(), "OSM tap → one-shot GUI armed");
    (void)osm_consume();                   /* cleans up */
}

/* 6b. An OSM tap (slot 0) + an MT tap (slot 1) in the same cycle: consume_tap
 * must not stop on the armed OSM (return 0) and lose the MT tap (audit M4). */
static void test_th_consume_osm_then_mt(void) {
    th_reset();
    (void)osm_consume();
    tap_hold_on_press(K_OSM(MOD_LGUI), 0, 0);       /* slot 0 = OSM */
    tap_hold_on_press(K_MT(MOD_LSFT, 0x04), 0, 1);  /* slot 1 = MT(Shift, A) */
    advance_ms(30);
    tap_hold_on_release(0, 0);   /* OSM tap */
    tap_hold_on_release(0, 1);   /* MT tap */
    TEST_ASSERT_EQ(tap_hold_consume_tap(), 0x04,
                   "consume_tap skips the armed OSM → returns the MT tap (0x04) (M4)");
    TEST_ASSERT(osm_is_active(), "OSM still armed along the way");
    (void)osm_consume();
}

/* 7. Normal key (not LT/MT/OSM) → not tracked. */
static void test_th_ignores_normal_key(void) {
    th_reset();
    TEST_ASSERT(!tap_hold_on_press(0x04, 0, 0), "normal key A → not tracked");
}

/* 8. Slot exhaustion: TAP_HOLD_MAX_PENDING taken → the next one fails. */
static void test_th_slot_exhaustion(void) {
    th_reset();
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++)
        TEST_ASSERT(tap_hold_on_press(K_MT(MOD_LSFT, 0x04), 0, i), "free slot taken");
    TEST_ASSERT(!tap_hold_on_press(K_MT(MOD_LSFT, 0x04), 1, 0),
                "beyond TAP_HOLD_MAX_PENDING → refused");
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) tap_hold_on_release(0, i);
}

/* 9. LT with an OUT-OF-BOUNDS layer (>= LAYERS=10) → ignored (otherwise OOB read
 *    of keymaps[] + current_layout stuck on an illegal layer). */
static void test_th_lt_layer_out_of_bounds(void) {
    th_reset();
    uint8_t save_cur = current_layout, save_last = last_layer;
    current_layout = 0;
    tap_hold_on_press(K_LT(15, 0x2C), 0, 0);   /* layer 15 >= LAYERS */
    advance_ms(200);
    tap_hold_tick();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), -1,
                   "LT layer 15 (>= LAYERS) → ignored, no active layer");
    TEST_ASSERT_EQ(current_layout, 0, "current_layout not corrupted by out-of-bounds LT");
    tap_hold_on_release(0, 0);
    current_layout = save_cur; last_layer = save_last;
}

/* 9 bis. CR-1: the out-of-bounds LT SURVIVES in pending[] in TH_HOLD, because
 *        activate_hold() sets e->state = TH_HOLD BEFORE testing the bound. The
 *        guard only blocks the activate_seq bump and the immediate recompute.
 *        When a valid LT held at the same time is released, deactivate_hold()
 *        calls recompute_lt_layer(), which re-scans pending[] and keeps its
 *        FIRST candidate via `!top`, regardless of its activate_seq — so
 *        the out-of-bounds entry, the only one left. current_layout then goes
 *        out of the bounds of keymaps[], and every subsequent read reads garbage. */
static void test_th_lt_oob_wins_recompute_after_valid_release(void) {
    th_reset();
    uint8_t save_cur = current_layout, save_last = last_layer;
    current_layout = 0;

    /* A valid LT enters hold. */
    tap_hold_on_press(K_LT(1, 0x2C), 0, 0);
    advance_ms(200);
    tap_hold_tick();
    TEST_ASSERT_EQ(current_layout, 1, "valid LT → layer 1 active");

    /* An out-of-bounds LT enters hold in turn: it must change nothing. */
    tap_hold_on_press(K_LT(15, 0x2D), 0, 1);
    advance_ms(200);
    tap_hold_tick();
    TEST_ASSERT_EQ(current_layout, 1, "out-of-bounds LT → does not take over");

    /* We release the VALID LT. The recompute must not elect the out-of-bounds one. */
    tap_hold_on_release(0, 0);

    TEST_ASSERT(current_layout < LAYERS,
                "after releasing the valid LT, current_layout stays within bounds");
    TEST_ASSERT(tap_hold_get_active_layer() < (int8_t)LAYERS,
                "active_hold_layer stays within bounds");

    tap_hold_on_release(0, 1);
    current_layout = save_cur; last_layer = save_last;
}

/* 10. Two LTs held simultaneously (audit bug E1): releasing the most recent one
 *     must revert to the layer of the LT still held, NOT lose everything;
 *     releasing both returns to base. The old code set active_hold_layer=-1 on
 *     the 1st release → layer lost, then keyboard stuck. */
static void test_th_two_lt_concurrent(void) {
    th_reset();
    uint8_t save_cur = current_layout, save_last = last_layer;
    current_layout = 0;
    tap_hold_on_press(K_LT(1, 0x2C), 0, 0);   /* P1 = LT(1) */
    advance_ms(200); tap_hold_tick();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 1, "P1 held → layer 1");
    tap_hold_on_press(K_LT(2, 0x2D), 0, 1);   /* P2 = LT(2), more recent */
    advance_ms(200); tap_hold_tick();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 2, "P2 held → layer 2 (more recent)");
    tap_hold_on_release(0, 1);                 /* releases P2 (P1 still held) */
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 1, "P2 release → back to layer 1 (P1 held)");
    tap_hold_on_release(0, 0);                 /* releases P1 */
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), -1, "P1 release → no more LT layer");
    TEST_ASSERT_EQ(current_layout, 0, "back to base 0 (not stuck)");
    current_layout = save_cur; last_layer = save_last;
}

void test_tap_hold(void) {
    TEST_SUITE("Tap/Hold State Machine — real module");
    TEST_RUN(test_th_mt_tap);
    TEST_RUN(test_th_mt_hold_timeout);
    TEST_RUN(test_th_timeout_boundary);
    TEST_RUN(test_th_interrupt_forces_hold);
    TEST_RUN(test_th_lt_hold_layer);
    TEST_RUN(test_th_lt_layer_out_of_bounds);
    TEST_RUN(test_th_lt_oob_wins_recompute_after_valid_release);
    TEST_RUN(test_th_two_lt_concurrent);
    TEST_RUN(test_th_osm_tap_arms);
    TEST_RUN(test_th_consume_osm_then_mt);
    TEST_RUN(test_th_ignores_normal_key);
    TEST_RUN(test_th_slot_exhaustion);
    tap_hold_init();   /* leaves the module clean for the following suites */
}
