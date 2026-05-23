/*
 * test_en_status.c — Host tests for:
 *   1. en_encode_status / en_decode_status (codec round-trip, wire byte layout)
 *   2. build_link_label() formatting (ASCII-only glyphs, clamp, sides)
 *   3. build_usb_label() formatting
 *
 * All tested functions are pure (no I/O, no FreeRTOS).
 */
#include "test_framework.h"
#include "../main/comm/espnow/espnow_msg.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ── build_link_label and build_usb_label are defined in eink_lvgl.c.
 * For host testing, reproduce them here as static helpers with the EXACT
 * same logic as the implementation. If the implementation changes, update
 * both. The test validates the specification, not just the current impl. */

static void build_link_label(char *buf, char side,
                              bool dongle_alive, bool link_up, uint8_t bars)
{
    const char dot = dongle_alive ? (link_up ? '*' : '-') : '?';
    bars = (bars > 4) ? 4 : bars;
    char bstr[7];
    bstr[0] = '[';
    for (int i = 0; i < 4; i++) bstr[1 + i] = (i < (int)bars) ? '|' : '.';
    bstr[5] = ']';
    bstr[6] = '\0';
    snprintf(buf, 16, "%c %c %s", side, dot, bstr);
}

static void build_usb_label(char *buf, bool dongle_alive, bool usb_active)
{
    const char dot = dongle_alive ? (usb_active ? '*' : '-') : '?';
    snprintf(buf, 10, "USB %c", dot);
}

void test_en_status(void)
{
    TEST_SUITE("en_status_codec");

    /* ── Codec: encode then decode round-trip ────────────────────── */
    {
        en_status_t orig = { .flags = 0x05, .sig_left = 3, .sig_right = 1 };
        uint8_t buf[8];
        uint8_t n = en_encode_status(buf, &orig);
        TEST_ASSERT_EQ(n, 4, "encode returns 4 bytes");
        TEST_ASSERT_EQ(buf[0], 0x04, "type byte is EN_INFO_STATUS");
        TEST_ASSERT_EQ(buf[1], 0x05, "flags byte");
        TEST_ASSERT_EQ(buf[2], 0x03, "sig_left byte");
        TEST_ASSERT_EQ(buf[3], 0x01, "sig_right byte");

        en_status_t dec;
        TEST_ASSERT(en_decode_status(buf, 4, &dec), "decode succeeds");
        TEST_ASSERT_EQ(dec.flags,     0x05, "decoded flags");
        TEST_ASSERT_EQ(dec.sig_left,  3,    "decoded sig_left");
        TEST_ASSERT_EQ(dec.sig_right, 1,    "decoded sig_right");
    }

    /* ── Codec: wrong type byte must reject ──────────────────────── */
    {
        uint8_t buf[4] = { 0x99, 0x05, 0x03, 0x01 };
        en_status_t dec;
        TEST_ASSERT(!en_decode_status(buf, 4, &dec), "wrong type rejected");
    }

    /* ── Codec: short buffer must reject ─────────────────────────── */
    {
        uint8_t buf[4] = { 0x04, 0x05, 0x03, 0x01 };
        en_status_t dec;
        TEST_ASSERT(!en_decode_status(buf, 3, &dec), "len=3 rejected (need 4)");
        TEST_ASSERT(!en_decode_status(buf, 0, &dec), "len=0 rejected");
    }

    /* ── Codec: all-zeros flags round-trip ───────────────────────── */
    {
        en_status_t orig = { .flags = 0x00, .sig_left = 0, .sig_right = 0 };
        uint8_t buf[8];
        en_encode_status(buf, &orig);
        en_status_t dec;
        TEST_ASSERT(en_decode_status(buf, 4, &dec), "all-zeros decode succeeds");
        TEST_ASSERT_EQ(dec.flags, 0x00, "all-zeros flags");
        TEST_ASSERT_EQ(dec.sig_left,  0, "all-zeros sig_left");
        TEST_ASSERT_EQ(dec.sig_right, 0, "all-zeros sig_right");
    }

    /* ── Codec: all bits set ─────────────────────────────────────── */
    {
        en_status_t orig = { .flags = 0x07, .sig_left = 4, .sig_right = 4 };
        uint8_t buf[8];
        en_encode_status(buf, &orig);
        en_status_t dec;
        TEST_ASSERT(en_decode_status(buf, 4, &dec), "max decode succeeds");
        TEST_ASSERT_EQ(dec.flags, 0x07, "all flags set");
        TEST_ASSERT_EQ(dec.sig_left,  4, "sig_left=4");
        TEST_ASSERT_EQ(dec.sig_right, 4, "sig_right=4");
    }

    TEST_SUITE("build_link_label");

    char buf[16];

    /* ── Link up, 4 bars ──────────────────────────────────────────── */
    build_link_label(buf, 'L', true, true, 4);
    TEST_ASSERT(strcmp(buf, "L * [||||]") == 0, "L up 4 bars");

    /* ── Link up, 0 bars ──────────────────────────────────────────── */
    build_link_label(buf, 'R', true, true, 0);
    TEST_ASSERT(strcmp(buf, "R * [....]") == 0, "R up 0 bars");

    /* ── Link up, 3 bars ──────────────────────────────────────────── */
    build_link_label(buf, 'L', true, true, 3);
    TEST_ASSERT(strcmp(buf, "L * [|||.]") == 0, "L up 3 bars");

    /* ── Link up, 2 bars ──────────────────────────────────────────── */
    build_link_label(buf, 'R', true, true, 2);
    TEST_ASSERT(strcmp(buf, "R * [||..]") == 0, "R up 2 bars");

    /* ── Link up, 1 bar ───────────────────────────────────────────── */
    build_link_label(buf, 'L', true, true, 1);
    TEST_ASSERT(strcmp(buf, "L * [|...]") == 0, "L up 1 bar");

    /* ── Link down ────────────────────────────────────────────────── */
    build_link_label(buf, 'L', true, false, 0);
    TEST_ASSERT(strcmp(buf, "L - [....]") == 0, "L link down");

    /* ── Dongle timeout: '?' override regardless of link_up ──────── */
    build_link_label(buf, 'R', false, true,  3);
    TEST_ASSERT(strcmp(buf, "R ? [|||.]") == 0, "R timeout up 3 bars to '?'");
    build_link_label(buf, 'L', false, false, 0);
    TEST_ASSERT(strcmp(buf, "L ? [....]") == 0, "L timeout down to '?'");

    /* ── Clamp bars > 4 → 4 ──────────────────────────────────────── */
    build_link_label(buf, 'L', true, true, 5);
    TEST_ASSERT(strcmp(buf, "L * [||||]") == 0, "bars=5 clamped to 4");
    build_link_label(buf, 'R', true, true, 255);
    TEST_ASSERT(strcmp(buf, "R * [||||]") == 0, "bars=255 clamped to 4");

    TEST_SUITE("build_usb_label");

    char ubuf[10];

    build_usb_label(ubuf, true, true);
    TEST_ASSERT(strcmp(ubuf, "USB *") == 0, "USB active");

    build_usb_label(ubuf, true, false);
    TEST_ASSERT(strcmp(ubuf, "USB -") == 0, "USB inactive");

    build_usb_label(ubuf, false, true);
    TEST_ASSERT(strcmp(ubuf, "USB ?") == 0, "USB timeout active but unknown");

    build_usb_label(ubuf, false, false);
    TEST_ASSERT(strcmp(ubuf, "USB ?") == 0, "USB timeout inactive but unknown");
}
