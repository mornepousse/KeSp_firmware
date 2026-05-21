/* Tests for the NRF24 RF packet codec (dongle plan 2) */
#include "test_framework.h"
#include "../main/comm/rf/rf_packet.h"

static void test_rf_key_roundtrip(void)
{
    uint8_t buf[32];
    rf_key_event_t e = { .row = 3, .col = 5, .pressed = true, .is_retry = false, .seq = 42 };
    uint16_t n = rf_encode_key(buf, &e);
    TEST_ASSERT_EQ(n, 3, "key encode length");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_KEY, "key type");

    rf_key_event_t d;
    TEST_ASSERT(rf_decode_key(buf, n, &d), "key decode ok");
    TEST_ASSERT_EQ(d.row, 3, "key row");
    TEST_ASSERT_EQ(d.col, 5, "key col");
    TEST_ASSERT(d.pressed, "key pressed");
    TEST_ASSERT(!d.is_retry, "key not retry");
    TEST_ASSERT_EQ(d.seq, 42, "key seq");
}

static void test_rf_key_flags(void)
{
    uint8_t buf[32];
    rf_key_event_t e = { .row = 0, .col = 0, .pressed = false, .is_retry = true, .seq = 255 };
    uint16_t n = rf_encode_key(buf, &e);
    rf_key_event_t d;
    rf_decode_key(buf, n, &d);
    TEST_ASSERT(!d.pressed, "released flag");
    TEST_ASSERT(d.is_retry, "retry flag");
    TEST_ASSERT_EQ(d.seq, 255, "seq 255");
}

static void test_rf_heartbeat_roundtrip(void)
{
    uint8_t buf[32];
    rf_heartbeat_t h;
    memset(&h, 0, sizeof(h));
    rf_bitmap_set(h.bitmap, 4, 6, true);   /* last key of the half */
    rf_bitmap_set(h.bitmap, 0, 0, true);
    h.batt_dV = 74; h.link_q = 2; h.seq = 7;

    uint16_t n = rf_encode_heartbeat(buf, &h);
    TEST_ASSERT_EQ(n, 9, "heartbeat encode length");

    rf_heartbeat_t hd;
    TEST_ASSERT(rf_decode_heartbeat(buf, n, &hd), "hb decode ok");
    TEST_ASSERT(rf_bitmap_get(hd.bitmap, 4, 6), "hb bit 4,6");
    TEST_ASSERT(rf_bitmap_get(hd.bitmap, 0, 0), "hb bit 0,0");
    TEST_ASSERT(!rf_bitmap_get(hd.bitmap, 2, 3), "hb bit 2,3 clear");
    TEST_ASSERT_EQ(hd.batt_dV, 74, "hb batt");
    TEST_ASSERT_EQ(hd.link_q, 2, "hb link_q");
    TEST_ASSERT_EQ(hd.seq, 7, "hb seq");
}

static void test_rf_trackpad_roundtrip(void)
{
    uint8_t buf[32];
    rf_trackpad_t t = { .dx = -5, .dy = 12, .buttons = 0x05,
                        .scroll_v = -1, .scroll_h = 3, .seq = 9 };
    uint16_t n = rf_encode_trackpad(buf, &t);
    TEST_ASSERT_EQ(n, 7, "trackpad encode length");

    rf_trackpad_t td;
    TEST_ASSERT(rf_decode_trackpad(buf, n, &td), "tp decode ok");
    TEST_ASSERT_EQ(td.dx, -5, "tp dx negative");
    TEST_ASSERT_EQ(td.dy, 12, "tp dy");
    TEST_ASSERT_EQ(td.buttons, 0x05, "tp buttons");
    TEST_ASSERT_EQ(td.scroll_v, -1, "tp scroll_v negative");
    TEST_ASSERT_EQ(td.scroll_h, 3, "tp scroll_h");
}

static void test_rf_decode_rejects(void)
{
    uint8_t buf[32];
    rf_trackpad_t t = { .dx = 1, .dy = 1, .buttons = 0, .scroll_v = 0, .scroll_h = 0, .seq = 1 };
    uint16_t n = rf_encode_trackpad(buf, &t);

    rf_key_event_t d;
    TEST_ASSERT(!rf_decode_key(buf, n, &d), "reject wrong type");

    rf_heartbeat_t hd;
    TEST_ASSERT(!rf_decode_heartbeat(buf, 2, &hd), "reject short buffer");

    TEST_ASSERT_EQ(rf_packet_type(buf, 0), 0, "type of empty buffer");
}

static void test_rf_bitmap_all_positions(void)
{
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    memset(bm, 0, sizeof(bm));
    /* set every position, verify only that one reads back */
    for (uint8_t r = 0; r < RF_HALF_ROWS; r++) {
        for (uint8_t c = 0; c < RF_HALF_COLS; c++) {
            memset(bm, 0, sizeof(bm));
            rf_bitmap_set(bm, r, c, true);
            TEST_ASSERT(rf_bitmap_get(bm, r, c), "bit set reads back");
            int count = 0;
            for (uint8_t rr = 0; rr < RF_HALF_ROWS; rr++)
                for (uint8_t cc = 0; cc < RF_HALF_COLS; cc++)
                    if (rf_bitmap_get(bm, rr, cc)) count++;
            TEST_ASSERT_EQ(count, 1, "exactly one bit set");
        }
    }
}

void test_rf_packet(void)
{
    TEST_SUITE("RF packet codec");
    test_rf_key_roundtrip();
    test_rf_key_flags();
    test_rf_heartbeat_roundtrip();
    test_rf_trackpad_roundtrip();
    test_rf_decode_rejects();
    test_rf_bitmap_all_positions();
}
