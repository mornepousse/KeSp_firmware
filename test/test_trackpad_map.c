/*
 * test_trackpad_map.c — Host tests for trackpad_map() pure function.
 *
 * Contract under test (spec §6.1, §4.3, §4.4, §4.5):
 *   - Scroll gesture (ge1 & GEST1_SCROLL) → scroll_v = clamp(rel_y / SCROLL_DIV)
 *     dx=0, dy=0. Scroll beats cursor.
 *   - Cursor (no scroll) → dx=clamp(rel_x), dy=clamp(rel_y), scroll_v=0.
 *   - Single tap (ge0 & GEST0_SINGLE_TAP) → buttons=0x01, pending_out=true, send=true.
 *   - Pending release → buttons=0x00, pending_out=false, send=true (force).
 *   - Activity gate: all-zero output → send=false.
 *   - Scroll-zero edge case: rel_y < SCROLL_DIV → scroll_v=0 → send=false.
 *
 * Run: cd test/build && cmake .. && make && ./test_runner
 */
#include "test_framework.h"
#include "../main/periph/trackpad/trackpad.h"

/* Helpers */
static rf_trackpad_t g_out;
static bool          g_pending;

static bool call_map(uint8_t ge0, uint8_t ge1, int16_t rx, int16_t ry)
{
    return trackpad_map(ge0, ge1, 0, rx, ry, &g_pending, &g_out);
}

void test_trackpad_map(void)
{
    TEST_SUITE("trackpad_map");

    /* ── Case 1: No touch — all zero, no send ─────────────────── */
    g_pending = false;
    bool sent = call_map(0x00, 0x00, 0, 0);
    TEST_ASSERT(!sent,             "no-touch: should_send must be false");
    TEST_ASSERT_EQ(g_out.dx,       0, "no-touch: dx must be 0");
    TEST_ASSERT_EQ(g_out.dy,       0, "no-touch: dy must be 0");
    TEST_ASSERT_EQ(g_out.buttons,  0, "no-touch: buttons must be 0");
    TEST_ASSERT_EQ(g_out.scroll_v, 0, "no-touch: scroll_v must be 0");

    /* ── Case 2: Movement — rel_x=50, rel_y=-20, no gesture ────── */
    g_pending = false;
    sent = call_map(0x00, 0x00, 50, -20);
    TEST_ASSERT(sent,               "movement: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx,  50,  "movement: dx must be 50");
    TEST_ASSERT_EQ(g_out.dy, -20,  "movement: dy must be -20");
    TEST_ASSERT_EQ(g_out.scroll_v, 0, "movement: scroll_v must be 0");
    TEST_ASSERT_EQ(g_out.buttons,  0, "movement: buttons must be 0");

    /* ── Case 3: Clamp positive — rel_x=200 → dx=127 ────────────── */
    g_pending = false;
    sent = call_map(0x00, 0x00, 200, 0);
    TEST_ASSERT(sent,               "clamp-pos: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx, 127,  "clamp-pos: dx must be 127");

    /* ── Case 4: Clamp negative — rel_x=-200 → dx=-127 ─────────── */
    g_pending = false;
    sent = call_map(0x00, 0x00, -200, 0);
    TEST_ASSERT(sent,               "clamp-neg: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx, -127, "clamp-neg: dx must be -127");

    /* ── Case 5: Single tap press ────────────────────────────────── */
    g_pending = false;
    sent = call_map(0x01, 0x00, 0, 0);   /* ge0 = GEST0_SINGLE_TAP */
    TEST_ASSERT(sent,                    "tap-press: should_send must be true");
    TEST_ASSERT_EQ(g_out.buttons, 0x01, "tap-press: buttons must be 0x01");
    TEST_ASSERT(g_pending,              "tap-press: pending must be true after tap");

    /* ── Case 6: Single tap release (pending=true, no gesture) ───── */
    /* g_pending is still true from Case 5 */
    sent = call_map(0x00, 0x00, 0, 0);
    TEST_ASSERT(sent,                    "tap-release: should_send must be true (forced)");
    TEST_ASSERT_EQ(g_out.buttons, 0x00, "tap-release: buttons must be 0x00");
    TEST_ASSERT(!g_pending,             "tap-release: pending must be false after release");

    /* ── Case 7: Scroll active — rel_x=30, rel_y=64 → scroll_v=8 ── */
    g_pending = false;
    sent = call_map(0x00, 0x02, 30, 64);   /* ge1 = GEST1_SCROLL */
    TEST_ASSERT(sent,                      "scroll: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx,        0,    "scroll: dx must be 0 (suppressed)");
    TEST_ASSERT_EQ(g_out.dy,        0,    "scroll: dy must be 0 (suppressed)");
    TEST_ASSERT_EQ(g_out.scroll_v,  8,    "scroll: scroll_v must be 64/8=8");

    /* ── Case 8: Scroll clamp — rel_y=1200 → scroll_v=127 ────────── */
    g_pending = false;
    sent = call_map(0x00, 0x02, 0, 1200);
    TEST_ASSERT(sent,                      "scroll-clamp: should_send must be true");
    TEST_ASSERT_EQ(g_out.scroll_v, 127,   "scroll-clamp: scroll_v must be 127");

    /* ── Case 9: Scroll zero activity gate ───────────────────────── */
    /* rel_y=3, SCROLL_DIV=8 → 3/8=0 → scroll_v=0 → send=false       */
    g_pending = false;
    sent = call_map(0x00, 0x02, 0, 3);
    TEST_ASSERT(!sent,                     "scroll-zero: should_send must be false (3/8=0)");
    TEST_ASSERT_EQ(g_out.scroll_v,  0,    "scroll-zero: scroll_v must be 0");
    TEST_ASSERT_EQ(g_out.dx,        0,    "scroll-zero: dx must be 0");

    /* ── Case 10: Scroll vs cursor mutual exclusion ───────────────── */
    /* Both scroll gesture and rel_x=50 → dx must be 0, scroll_v=8   */
    g_pending = false;
    sent = call_map(0x00, 0x02, 50, 64);
    TEST_ASSERT(sent,                      "scroll-mutex: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx,        0,    "scroll-mutex: dx must be 0 (scroll wins)");
    TEST_ASSERT_EQ(g_out.dy,        0,    "scroll-mutex: dy must be 0 (scroll wins)");
    TEST_ASSERT_EQ(g_out.scroll_v,  8,    "scroll-mutex: scroll_v must be 8");
    TEST_ASSERT_EQ(g_out.buttons,   0,    "scroll-mutex: buttons must be 0");
}
