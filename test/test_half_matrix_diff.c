/*
 * Host-side unit tests for half_diff_emit (pure bitmap diff logic).
 *
 * TEST_HOST is defined in test/CMakeLists.txt CFLAGS (-DTEST_HOST).
 * This activates:
 *   - keyboard_btn_data_t stub typedef in half_scan_task.h
 *   - half_diff_emit declaration in half_scan_task.h
 * half_scan_task.c compiles half_diff_emit only (the FreeRTOS task is
 * excluded by #ifndef TEST_HOST guards).
 */
#include "test_framework.h"
#include "../main/comm/rf/half_scan_task.h"
#include "../main/comm/rf/rf_packet.h"
#include <string.h>

/* Capture emitted events */
typedef struct { uint8_t row, col; bool pressed; } emit_event_t;
static emit_event_t g_events[32];
static int g_evt_count;

static void capture_emit(uint8_t row, uint8_t col, bool pressed, void *ctx)
{
    (void)ctx;
    if (g_evt_count < 32) {
        g_events[g_evt_count].row     = row;
        g_events[g_evt_count].col     = col;
        g_events[g_evt_count].pressed = pressed;
        g_evt_count++;
    }
}

void test_half_matrix_diff(void)
{
    TEST_SUITE("half_matrix_diff");

    uint8_t bitmap[RF_HALF_BITMAP_BYTES];
    memset(bitmap, 0, sizeof(bitmap));
    g_evt_count = 0;

    /* ROW2COL: output_index = row (0..4), input_index = col (0..6). */
    /* ── Case 1: press two keys — no releases ──────────────── */
    keyboard_btn_data_t pressed[2] = {
        { .output_index = 1, .input_index = 2 },
        { .output_index = 3, .input_index = 5 },
    };
    half_diff_emit(bitmap, pressed, 2, NULL, 0, capture_emit, NULL);

    TEST_ASSERT_EQ(g_evt_count, 2, "two press events emitted");
    TEST_ASSERT(g_events[0].pressed, "first event is press");
    TEST_ASSERT_EQ(g_events[0].row, 1, "first press row");
    TEST_ASSERT_EQ(g_events[0].col, 2, "first press col");
    TEST_ASSERT(g_events[1].pressed, "second event is press");
    TEST_ASSERT_EQ(g_events[1].row, 3, "second press row");
    TEST_ASSERT_EQ(g_events[1].col, 5, "second press col");

    /* Bitmap should reflect both pressed */
    TEST_ASSERT(rf_bitmap_get(bitmap, 1, 2), "bitmap bit 1,2 set");
    TEST_ASSERT(rf_bitmap_get(bitmap, 3, 5), "bitmap bit 3,5 set");
    TEST_ASSERT(!rf_bitmap_get(bitmap, 0, 0), "bitmap bit 0,0 clear");

    /* ── Case 2: release first key, press a new one ─────────── */
    g_evt_count = 0;
    keyboard_btn_data_t pressed2[2] = {
        { .output_index = 3, .input_index = 5 },
        { .output_index = 4, .input_index = 0 },
    };
    keyboard_btn_data_t released2[1] = {
        { .output_index = 1, .input_index = 2 },
    };
    half_diff_emit(bitmap, pressed2, 2, released2, 1, capture_emit, NULL);

    /* Releases come first in emit order */
    TEST_ASSERT_EQ(g_evt_count, 2, "one release + one new press = 2 events");
    TEST_ASSERT(!g_events[0].pressed, "first event is release");
    TEST_ASSERT_EQ(g_events[0].row, 1, "released row");
    TEST_ASSERT_EQ(g_events[0].col, 2, "released col");
    TEST_ASSERT(g_events[1].pressed, "second event is press");
    TEST_ASSERT_EQ(g_events[1].row, 4, "new press row");
    TEST_ASSERT_EQ(g_events[1].col, 0, "new press col");

    /* Bitmap reflects final state: 3,5 + 4,0 pressed; 1,2 released */
    TEST_ASSERT(!rf_bitmap_get(bitmap, 1, 2), "1,2 cleared after release");
    TEST_ASSERT(rf_bitmap_get(bitmap, 3, 5), "3,5 still set");
    TEST_ASSERT(rf_bitmap_get(bitmap, 4, 0), "4,0 newly set");

    /* ── Case 3: out-of-bounds row/col are silently ignored ── */
    g_evt_count = 0;
    keyboard_btn_data_t oob[1] = {
        { .output_index = 7, .input_index = 10 },   /* row=7 > 4, col=10 > 6 */
    };
    half_diff_emit(bitmap, oob, 1, NULL, 0, capture_emit, NULL);
    TEST_ASSERT_EQ(g_evt_count, 0, "out-of-bounds key ignored");

    /* ── Case 4: all-release, bitmap becomes zero ───────────── */
    g_evt_count = 0;
    keyboard_btn_data_t rel_all[2] = {
        { .output_index = 3, .input_index = 5 },
        { .output_index = 4, .input_index = 0 },
    };
    half_diff_emit(bitmap, NULL, 0, rel_all, 2, capture_emit, NULL);
    TEST_ASSERT_EQ(g_evt_count, 2, "two releases emitted");
    uint8_t zero[RF_HALF_BITMAP_BYTES] = {0};
    TEST_ASSERT(memcmp(bitmap, zero, RF_HALF_BITMAP_BYTES) == 0, "bitmap fully cleared");
}
