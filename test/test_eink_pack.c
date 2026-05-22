/*
 * test_eink_pack.c — Host tests for eink_fb_set_px() 1bpp pixel packing.
 *
 * Run: cd test/build && cmake .. && make && ./test_runner
 *
 * Contract under test:
 *   eink_fb_set_px(fb, col, row, is_white)
 *     → fb[row*25 + col/8] bit (7 - col%8) = is_white
 *   White (is_white=1) → bit set to 1 (SSD1681 white).
 *   Black (is_white=0) → bit cleared to 0 (SSD1681 black).
 *   MSB-first: pixel at col=0 is bit 7, col=7 is bit 0, col=8 is next byte bit 7.
 *   Total framebuffer: EINK_FB_SIZE = 5000 bytes (200×200 / 8).
 */
#define TEST_HOST
#include "test_framework.h"
#include "../main/periph/eink/eink.h"
#include <string.h>

/* Helper: read back a single bit from the packed framebuffer */
static int fb_bit(const uint8_t *fb, int col, int row)
{
    int byte_idx = row * (EINK_WIDTH / 8) + col / 8;
    int bit_idx  = 7 - (col % 8);
    return (fb[byte_idx] >> bit_idx) & 1;
}

/* Helper: paint every pixel via eink_fb_set_px and check */
static void paint_all(uint8_t *fb, int is_white)
{
    memset(fb, is_white ? 0x00 : 0xFF, EINK_FB_SIZE);   /* pre-fill with opposite */
    for (int row = 0; row < EINK_HEIGHT; row++) {
        for (int col = 0; col < EINK_WIDTH; col++) {
            eink_fb_set_px(fb, col, row, is_white);
        }
    }
}

void test_eink_pack(void)
{
    TEST_SUITE("eink_fb_set_px");

    static uint8_t fb[EINK_FB_SIZE];

    /* ── Case 1: EINK_FB_SIZE sanity ─────────────────────────── */
    TEST_ASSERT(EINK_FB_SIZE == 5000, "EINK_FB_SIZE must be 5000 bytes");

    /* ── Case 2: all-white → s_fb all 0xFF ───────────────────── */
    paint_all(fb, 1);
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0xFF) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "all-white: fb must be 0xFF throughout");
    }

    /* ── Case 3: all-black → s_fb all 0x00 ───────────────────── */
    paint_all(fb, 0);
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0x00) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "all-black: fb must be 0x00 throughout");
    }

    /* ── Case 4: single black pixel at (0,0), all others white ── */
    /* Start all-white via paint_all, then set (0,0) black. */
    paint_all(fb, 1);
    eink_fb_set_px(fb, 0, 0, 0);
    /* Col 0 = bit 7 of byte 0. Black → bit 7 cleared.
     * Expected: fb[0] = 0b01111111 = 0x7F */
    TEST_ASSERT_EQ(fb[0], 0x7F, "single black (0,0): fb[0] must be 0x7F");
    TEST_ASSERT_EQ(fb[1], 0xFF, "single black (0,0): fb[1] must be 0xFF");
    TEST_ASSERT_EQ(fb[25], 0xFF, "single black (0,0): fb[25] (row1,byte0) must be 0xFF");

    /* ── Case 5: single black pixel at (7,0) — LSB of byte 0 ─── */
    paint_all(fb, 1);
    eink_fb_set_px(fb, 7, 0, 0);
    /* Col 7 = bit (7-7) = bit 0 of byte 0 → cleared. */
    TEST_ASSERT_EQ(fb[0], 0xFE, "single black (7,0): fb[0] must be 0xFE");

    /* ── Case 6: single black pixel at (8,0) — MSB of byte 1 ─── */
    paint_all(fb, 1);
    eink_fb_set_px(fb, 8, 0, 0);
    TEST_ASSERT_EQ(fb[0], 0xFF, "single black (8,0): fb[0] unaffected");
    TEST_ASSERT_EQ(fb[1], 0x7F, "single black (8,0): fb[1] must be 0x7F");

    /* ── Case 7: single black pixel at (0,1) — start of row 1 ── */
    paint_all(fb, 1);
    eink_fb_set_px(fb, 0, 1, 0);
    /* Row 1, byte 0 = fb[25]. Col 0 = bit 7. */
    TEST_ASSERT_EQ(fb[0],  0xFF, "single black (0,1): fb[0] (row0) unaffected");
    TEST_ASSERT_EQ(fb[25], 0x7F, "single black (0,1): fb[25] (row1,byte0) must be 0x7F");

    /* ── Case 8: single white pixel at (0,0) on black background ── */
    paint_all(fb, 0);
    eink_fb_set_px(fb, 0, 0, 1);
    TEST_ASSERT_EQ(fb[0], 0x80, "single white (0,0) on black: fb[0] must be 0x80");

    /* ── Case 9: checkerboard (even col = black) → all bytes 0x55 ── */
    /* bit7=0(black), bit6=1(white), bit5=0, bit4=1, ... = 0x55 */
    memset(fb, 0x00, EINK_FB_SIZE);
    for (int row = 0; row < EINK_HEIGHT; row++) {
        for (int col = 0; col < EINK_WIDTH; col++) {
            eink_fb_set_px(fb, col, row, (col % 2 != 0) ? 1 : 0);
        }
    }
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0x55) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "checkerboard (even=black): all bytes must be 0x55");
    }

    /* ── Case 10: inverse checkerboard (even col = white) → 0xAA ── */
    memset(fb, 0x00, EINK_FB_SIZE);
    for (int row = 0; row < EINK_HEIGHT; row++) {
        for (int col = 0; col < EINK_WIDTH; col++) {
            eink_fb_set_px(fb, col, row, (col % 2 == 0) ? 1 : 0);
        }
    }
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0xAA) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "checkerboard (even=white): all bytes must be 0xAA");
    }

    /* ── Case 11: fb_bit helper self-check ─────────────────────── */
    paint_all(fb, 1);
    eink_fb_set_px(fb, 0, 0, 0);   /* (0,0) = black */
    TEST_ASSERT_EQ(fb_bit(fb, 0, 0), 0, "fb_bit(0,0) black must read 0");
    TEST_ASSERT_EQ(fb_bit(fb, 1, 0), 1, "fb_bit(1,0) white must read 1");
    TEST_ASSERT_EQ(fb_bit(fb, 0, 1), 1, "fb_bit(0,1) white must read 1");
}
