/* Tests for heartbeat reconciliation (dongle plan 2) */
#include "test_framework.h"
#include "../main/comm/rf/heartbeat.h"
#include <string.h>

/* Capture callback invocations */
static int g_press_count, g_release_count;
static uint8_t g_last_row, g_last_col;

static void cap_press(void *c, uint8_t h, uint8_t r, uint8_t col)
{ (void)c; (void)h; g_press_count++; g_last_row = r; g_last_col = col; }
static void cap_release(void *c, uint8_t h, uint8_t r, uint8_t col)
{ (void)c; (void)h; g_release_count++; g_last_row = r; g_last_col = col; }

static void test_hb_apply_key_dedup(void)
{
    hb_half_state_t st; memset(&st, 0, sizeof(st));
    rf_key_event_t e = { .row = 2, .col = 3, .pressed = true, .is_retry = false, .seq = 1 };
    TEST_ASSERT(hb_apply_key(&st, &e), "first press changes state");
    TEST_ASSERT(!hb_apply_key(&st, &e), "same seq is dup");
    TEST_ASSERT(rf_bitmap_get(st.local_bitmap, 2, 3), "bit stays set");

    rf_key_event_t e2 = { .row = 2, .col = 3, .pressed = false, .is_retry = false, .seq = 2 };
    TEST_ASSERT(hb_apply_key(&st, &e2), "release with new seq changes");
    TEST_ASSERT(!rf_bitmap_get(st.local_bitmap, 2, 3), "bit cleared");
}

static void test_hb_retry_not_dup(void)
{
    hb_half_state_t st; memset(&st, 0, sizeof(st));
    rf_key_event_t e = { .row = 1, .col = 1, .pressed = true, .is_retry = false, .seq = 5 };
    TEST_ASSERT(hb_apply_key(&st, &e), "press");
    /* retransmit of release with SAME seq but is_retry set must not be dropped */
    rf_key_event_t r = { .row = 1, .col = 1, .pressed = false, .is_retry = true, .seq = 5 };
    TEST_ASSERT(hb_apply_key(&st, &r), "retry bypasses dedup");
    TEST_ASSERT(!rf_bitmap_get(st.local_bitmap, 1, 1), "released via retry");
}

static void test_hb_reconcile_missed_press(void)
{
    hb_half_state_t st; memset(&st, 0, sizeof(st));
    hb_callbacks_t cb = { cap_press, cap_release, NULL };
    g_press_count = g_release_count = 0;

    rf_heartbeat_t h; memset(&h, 0, sizeof(h));
    rf_bitmap_set(h.bitmap, 1, 1, true);   /* dongle missed this press event */
    hb_reconcile(&st, HB_HALF_LEFT, &h, &cb, 1000);

    TEST_ASSERT_EQ(g_press_count, 1, "forced one press");
    TEST_ASSERT_EQ(g_last_row, 1, "press row");
    TEST_ASSERT_EQ(g_last_col, 1, "press col");
    TEST_ASSERT(st.link_up, "link marked up");
    TEST_ASSERT(rf_bitmap_get(st.local_bitmap, 1, 1), "local bitmap synced");
}

static void test_hb_reconcile_stuck_release(void)
{
    hb_half_state_t st; memset(&st, 0, sizeof(st));
    hb_callbacks_t cb = { cap_press, cap_release, NULL };

    /* local thinks (3,3) is pressed but heartbeat says nothing pressed */
    rf_bitmap_set(st.local_bitmap, 3, 3, true);
    g_press_count = g_release_count = 0;
    rf_heartbeat_t h; memset(&h, 0, sizeof(h));
    hb_reconcile(&st, HB_HALF_LEFT, &h, &cb, 1100);

    TEST_ASSERT_EQ(g_release_count, 1, "forced one release");
    TEST_ASSERT(!rf_bitmap_get(st.local_bitmap, 3, 3), "stuck key cleared");
}

static void test_hb_timeout_releases_all(void)
{
    hb_half_state_t st; memset(&st, 0, sizeof(st));
    hb_callbacks_t cb = { cap_press, cap_release, NULL };

    /* establish link with 2 keys pressed via reconcile */
    rf_heartbeat_t h; memset(&h, 0, sizeof(h));
    rf_bitmap_set(h.bitmap, 0, 0, true);
    rf_bitmap_set(h.bitmap, 4, 6, true);
    hb_reconcile(&st, HB_HALF_LEFT, &h, &cb, 1000);
    TEST_ASSERT(st.link_up, "link up after hb");

    g_release_count = 0;
    /* not yet timed out */
    hb_check_timeout(&st, HB_HALF_LEFT, &cb, 1200, 250);  /* 200ms < 250 */
    TEST_ASSERT_EQ(g_release_count, 0, "no release before timeout");
    TEST_ASSERT(st.link_up, "still up");

    /* now timed out → release both keys, link down */
    hb_check_timeout(&st, HB_HALF_LEFT, &cb, 1300, 250);  /* 300ms > 250 */
    TEST_ASSERT_EQ(g_release_count, 2, "released both keys");
    TEST_ASSERT(!st.link_up, "link down after timeout");

    /* second timeout call is a no-op (link already down) */
    g_release_count = 0;
    hb_check_timeout(&st, HB_HALF_LEFT, &cb, 2000, 250);
    TEST_ASSERT_EQ(g_release_count, 0, "no double release");
}

/* OOB guard: rf_decode_key emits 4-bit row/col (0..15) straight from the wire,
 * but local_bitmap only sizes RF_HALF_ROWS×RF_HALF_COLS. Out-of-range frames
 * must be dropped before any bitmap access (else OOB read/write past the 5-byte
 * bitmap). Without the guard, row=15/col=15 would access bitmap[15] and return
 * true; with it, false. (Pentest 2026-06-25, OTA OOB write.) */
static void test_hb_apply_key_oob_rejected(void)
{
    hb_half_state_t st; memset(&st, 0, sizeof(st));
    rf_key_event_t bad_row = { .row = RF_HALF_ROWS, .col = 0, .pressed = true, .seq = 1 };
    TEST_ASSERT(!hb_apply_key(&st, &bad_row), "row >= RF_HALF_ROWS rejected");
    rf_key_event_t bad_col = { .row = 0, .col = RF_HALF_COLS, .pressed = true, .seq = 2 };
    TEST_ASSERT(!hb_apply_key(&st, &bad_col), "col >= RF_HALF_COLS rejected");
    rf_key_event_t maxv = { .row = 15, .col = 15, .pressed = true, .seq = 3 };
    TEST_ASSERT(!hb_apply_key(&st, &maxv), "row=15 col=15 rejected (idx 120 OOB)");
    rf_key_event_t okv = { .row = RF_HALF_ROWS - 1, .col = RF_HALF_COLS - 1,
                           .pressed = true, .seq = 4 };
    TEST_ASSERT(hb_apply_key(&st, &okv), "max in-range key still accepted");
}

void test_heartbeat(void)
{
    TEST_SUITE("Heartbeat reconciliation");
    test_hb_apply_key_dedup();
    test_hb_retry_not_dup();
    test_hb_reconcile_missed_press();
    test_hb_reconcile_stuck_release();
    test_hb_timeout_releases_all();
    test_hb_apply_key_oob_rejected();
}
