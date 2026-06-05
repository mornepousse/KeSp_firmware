/*
 * test_trackpad_map.c — Host tests for trackpad_map() pure function (v2).
 *
 * Contract under test:
 *   - 1-finger movement      → dx/dy (gain 1.0 with neutral cfg, clamped to int8)
 *   - 1-finger single tap    → buttons=LEFT, pending_release=true
 *   - 2-finger tap           → buttons=RIGHT, pending_release=true
 *   - 3-finger tap           → buttons=MIDDLE (synthesized via peak_fingers ≥ 3)
 *   - Pending release        → buttons=0, pending_release=false, force_send=true
 *   - 2-finger scroll        → scroll_v + scroll_h, dx=dy=0
 *   - Press-and-hold + move  → drag: LEFT held, dx/dy emitted while down
 *   - Drag release           → on n_fingers=0, buttons=0 packet emitted once
 *   - Activity gate          → all-zero output ⇒ send=false
 *
 * Run: cd test/build && cmake .. && make && ./test_runner
 */
#include "test_framework.h"
#include "../main/periph/trackpad/trackpad.h"

/* Mouse button bits (mirror trackpad_map.c's private defines) */
#define BTN_LEFT   0x01
#define BTN_RIGHT  0x02
#define BTN_MIDDLE 0x04

/* Helpers */
static trackpad_out_t    g_out;
static trackpad_state_t  g_state;

/* Neutral config: base=100, accel=0, gain_max=100 → gain 1.0 */
static const trackpad_cfg_t g_cfg = {
    .fmt = TRACKPAD_CFG_FMT, .base = 100, .accel = 0, .gain_max = 100
};

static void reset_state(void)
{
    g_state.pending_release = false;
    g_state.drag_active     = false;
    g_state.peak_fingers    = 0;
}

static bool call_map(uint8_t ge0, uint8_t ge1, uint8_t n_fingers,
                     int16_t rx, int16_t ry)
{
    return trackpad_map(ge0, ge1, n_fingers, rx, ry, &g_cfg, &g_state, &g_out);
}

void test_trackpad_map(void)
{
    TEST_SUITE("trackpad_map");

    /* ── Case 1: No touch — all zero, no send ─────────────────── */
    reset_state();
    bool sent = call_map(0x00, 0x00, 0, 0, 0);
    TEST_ASSERT(!sent,             "no-touch: should_send must be false");
    TEST_ASSERT_EQ(g_out.dx,       0, "no-touch: dx must be 0");
    TEST_ASSERT_EQ(g_out.dy,       0, "no-touch: dy must be 0");
    TEST_ASSERT_EQ(g_out.buttons,  0, "no-touch: buttons must be 0");
    TEST_ASSERT_EQ(g_out.scroll_v, 0, "no-touch: scroll_v must be 0");
    TEST_ASSERT_EQ(g_out.scroll_h, 0, "no-touch: scroll_h must be 0");

    /* ── Case 2: Movement — rel_x=50, rel_y=-20, no gesture ────── */
    reset_state();
    sent = call_map(0x00, 0x00, 1, 50, -20);
    TEST_ASSERT(sent,               "movement: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx,  50,  "movement: dx must be 50 (gain=1.0)");
    TEST_ASSERT_EQ(g_out.dy, -20,  "movement: dy must be -20");
    TEST_ASSERT_EQ(g_out.scroll_v, 0, "movement: scroll_v must be 0");
    TEST_ASSERT_EQ(g_out.buttons,  0, "movement: buttons must be 0");

    /* ── Case 2b: 1-finger move rel=(10,-4) → dx=10, dy=-4 ──────── */
    reset_state();
    sent = call_map(0x00, 0x00, 1, 10, -4);
    TEST_ASSERT(sent,               "move-10: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx,  10,  "move-10: dx must be 10");
    TEST_ASSERT_EQ(g_out.dy,  -4,  "move-10: dy must be -4");

    /* ── Case 3: Clamp positive — rel_x=200 → dx=127 ────────────── */
    reset_state();
    sent = call_map(0x00, 0x00, 1, 200, 0);
    TEST_ASSERT(sent,               "clamp-pos: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx, 127,  "clamp-pos: dx must be 127");

    /* ── Case 4: Clamp negative — rel_x=-200 → dx=-127 ─────────── */
    reset_state();
    sent = call_map(0x00, 0x00, 1, -200, 0);
    TEST_ASSERT(sent,               "clamp-neg: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx, -127, "clamp-neg: dx must be -127");

    /* ── Case 5: Single-finger tap press → LEFT ─────────────────── */
    reset_state();
    /* peak_fingers stays at 1 because we never lifted (n_fingers=1 at tap) */
    sent = call_map(0x01, 0x00, 1, 0, 0);   /* ge0 = SINGLE_TAP */
    TEST_ASSERT(sent,                    "tap-press: should_send must be true");
    TEST_ASSERT_EQ(g_out.buttons, BTN_LEFT, "tap-press: LEFT");
    TEST_ASSERT(g_state.pending_release, "tap-press: pending must be true");

    /* ── Case 6: Tap release (no gesture, pending=true) ─────────── */
    sent = call_map(0x00, 0x00, 0, 0, 0);
    TEST_ASSERT(sent,                     "tap-release: should_send must be true (forced)");
    TEST_ASSERT_EQ(g_out.buttons, 0x00,  "tap-release: buttons must be 0");
    TEST_ASSERT(!g_state.pending_release,"tap-release: pending must be false");

    /* ── Case 7: 2-finger scroll — vertical + horizontal ────────── */
    reset_state();
    sent = call_map(0x00, 0x02, 2, 32, 64);   /* ge1 = SCROLL */
    TEST_ASSERT(sent,                      "scroll: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx,        0,    "scroll: dx must be 0 (suppressed)");
    TEST_ASSERT_EQ(g_out.dy,        0,    "scroll: dy must be 0 (suppressed)");
    TEST_ASSERT_EQ(g_out.scroll_v,  8,    "scroll: scroll_v must be 64/8=8");
    TEST_ASSERT_EQ(g_out.scroll_h,  4,    "scroll: scroll_h must be 32/8=4");

    /* ── Case 8: Scroll clamp — rel_y=1200 → scroll_v=127 ────────── */
    reset_state();
    sent = call_map(0x00, 0x02, 2, 0, 1200);
    TEST_ASSERT(sent,                      "scroll-clamp: should_send must be true");
    TEST_ASSERT_EQ(g_out.scroll_v, 127,   "scroll-clamp: scroll_v must be 127");

    /* ── Case 9: Scroll zero activity gate ───────────────────────── */
    /* rel=3, SCROLL_DIV=8 → 3/8=0 → no axis activity → send=false   */
    reset_state();
    sent = call_map(0x00, 0x02, 2, 3, 3);
    TEST_ASSERT(!sent,                     "scroll-zero: should_send must be false");
    TEST_ASSERT_EQ(g_out.scroll_v,  0,    "scroll-zero: scroll_v must be 0");
    TEST_ASSERT_EQ(g_out.scroll_h,  0,    "scroll-zero: scroll_h must be 0");

    /* ── Case 10: 2-finger tap → RIGHT click ─────────────────────── */
    reset_state();
    sent = call_map(0x00, 0x01, 2, 0, 0);    /* ge1 = TWO_FINGER_TAP */
    TEST_ASSERT(sent,                       "2f-tap: should_send must be true");
    TEST_ASSERT_EQ(g_out.buttons, BTN_RIGHT,"2f-tap: RIGHT click");
    TEST_ASSERT(g_state.pending_release,   "2f-tap: pending must be true");

    /* Release */
    sent = call_map(0x00, 0x00, 0, 0, 0);
    TEST_ASSERT_EQ(g_out.buttons, 0x00,    "2f-tap-release: buttons=0");

    /* ── Case 11: 3-finger tap → MIDDLE click via peak_fingers ───── */
    reset_state();
    /* Simulate a 3-finger touch session: 3 fingers down (no tap evt yet) */
    sent = call_map(0x00, 0x00, 3, 0, 0);  /* peak_fingers becomes 3 */
    /* No event, no movement → send=false but state updated */
    TEST_ASSERT_EQ(g_state.peak_fingers, 3, "3f-tap: peak updated to 3");
    /* Now the chip emits SingleTap on lift; n_fingers=0 by then but peak persists */
    sent = call_map(0x01, 0x00, 0, 0, 0);  /* ge0 = SINGLE_TAP, fingers released */
    TEST_ASSERT(sent,                          "3f-tap: should_send true");
    TEST_ASSERT_EQ(g_out.buttons, BTN_MIDDLE, "3f-tap: MIDDLE click");
    TEST_ASSERT(g_state.pending_release,      "3f-tap: pending must be true");
    /* peak_fingers should reset to 0 because n_fingers became 0 */
    TEST_ASSERT_EQ(g_state.peak_fingers, 0,   "3f-tap: peak resets when fingers=0");

    /* Release packet */
    sent = call_map(0x00, 0x00, 0, 0, 0);
    TEST_ASSERT_EQ(g_out.buttons, 0x00,       "3f-tap-release: buttons=0");

    /* ── Case 12: Drag — press-and-hold engages LEFT button hold ── */
    reset_state();
    /* press_hold + 1 finger → drag starts, LEFT held */
    sent = call_map(0x02, 0x00, 1, 10, 5);    /* ge0 = PRESS_HOLD */
    TEST_ASSERT(sent,                         "drag-start: should_send must be true");
    TEST_ASSERT_EQ(g_out.buttons, BTN_LEFT,   "drag-start: LEFT held");
    TEST_ASSERT(g_state.drag_active,          "drag-start: drag_active=true");
    TEST_ASSERT_EQ(g_out.dx, 10,              "drag-start: dx=10 (move while dragging)");
    TEST_ASSERT_EQ(g_out.dy,  5,              "drag-start: dy=5");

    /* Continue dragging — fingers still down, no press_hold flag now */
    sent = call_map(0x00, 0x00, 1, 7, -3);
    TEST_ASSERT(sent,                         "drag-cont: should_send must be true");
    TEST_ASSERT_EQ(g_out.buttons, BTN_LEFT,   "drag-cont: LEFT still held");
    TEST_ASSERT(g_state.drag_active,          "drag-cont: drag_active still true");
    TEST_ASSERT_EQ(g_out.dx,  7,              "drag-cont: dx=7");
    TEST_ASSERT_EQ(g_out.dy, -3,              "drag-cont: dy=-3");

    /* End drag — fingers lifted (n_fingers=0) */
    sent = call_map(0x00, 0x00, 0, 0, 0);
    TEST_ASSERT(sent,                         "drag-end: should_send must be true (release)");
    TEST_ASSERT_EQ(g_out.buttons, 0x00,       "drag-end: buttons=0 (release)");
    TEST_ASSERT(!g_state.drag_active,         "drag-end: drag_active=false");

    /* ── Case 13: Scroll vs cursor mutual exclusion ───────────────── */
    reset_state();
    sent = call_map(0x00, 0x02, 2, 50, 64);
    TEST_ASSERT(sent,                      "scroll-mutex: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx,        0,    "scroll-mutex: dx must be 0");
    TEST_ASSERT_EQ(g_out.dy,        0,    "scroll-mutex: dy must be 0");
    TEST_ASSERT_EQ(g_out.scroll_v,  8,    "scroll-mutex: scroll_v must be 8");
    TEST_ASSERT_EQ(g_out.scroll_h,  6,    "scroll-mutex: scroll_h must be 50/8=6");
    TEST_ASSERT_EQ(g_out.buttons,   0,    "scroll-mutex: buttons must be 0");
}
