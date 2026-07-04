/*
 * test_en_status.c — Host tests for:
 *   1. en_encode_status / en_decode_status (codec round-trip, wire byte layout)
 *   2. en_build_link_label() — vrai code extrait de eink_lvgl.c via en_label.c
 *   3. en_build_usb_label()  — idem
 *
 * All tested functions are pure (no I/O, no FreeRTOS).
 */
#include "test_framework.h"
#include "../main/comm/espnow/espnow_msg.h"
#include "en_label.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

    TEST_SUITE("en_build_link_label");

    char lbuf[28];

    /* ── Dongle absent : affiche --/255 quelle que soit q255 ────── */
    en_build_link_label(lbuf, sizeof(lbuf), 'L', false, 200);
    TEST_ASSERT(strcmp(lbuf, LV_SYMBOL_WIFI " L --/255 --%") == 0,
                "dongle absent side L: --/255");

    en_build_link_label(lbuf, sizeof(lbuf), 'R', false, 0);
    TEST_ASSERT(strcmp(lbuf, LV_SYMBOL_WIFI " R --/255 --%") == 0,
                "dongle absent side R: --/255");

    /* ── Dongle présent, qualité maximale (255) ──────────────────── */
    en_build_link_label(lbuf, sizeof(lbuf), 'L', true, 255);
    TEST_ASSERT(strcmp(lbuf, LV_SYMBOL_WIFI " L 255/255 --%") == 0,
                "dongle alive side L q255=255");

    /* ── Dongle présent, qualité nulle (0) ───────────────────────── */
    en_build_link_label(lbuf, sizeof(lbuf), 'R', true, 0);
    TEST_ASSERT(strcmp(lbuf, LV_SYMBOL_WIFI " R 0/255 --%") == 0,
                "dongle alive side R q255=0");

    /* ── Valeurs intermédiaires ───────────────────────────────────── */
    en_build_link_label(lbuf, sizeof(lbuf), 'L', true, 128);
    TEST_ASSERT(strcmp(lbuf, LV_SYMBOL_WIFI " L 128/255 --%") == 0,
                "dongle alive side L q255=128");

    en_build_link_label(lbuf, sizeof(lbuf), 'R', true, 64);
    TEST_ASSERT(strcmp(lbuf, LV_SYMBOL_WIFI " R 64/255 --%") == 0,
                "dongle alive side R q255=64");

    en_build_link_label(lbuf, sizeof(lbuf), 'L', true, 1);
    TEST_ASSERT(strcmp(lbuf, LV_SYMBOL_WIFI " L 1/255 --%") == 0,
                "dongle alive side L q255=1 (low quality)");

    en_build_link_label(lbuf, sizeof(lbuf), 'R', true, 200);
    TEST_ASSERT(strcmp(lbuf, LV_SYMBOL_WIFI " R 200/255 --%") == 0,
                "dongle alive side R q255=200 (high quality)");

    /* ── Préfixe WiFi glyph présent (octets non-ASCII) ───────────── */
    TEST_ASSERT(memcmp(lbuf, "\xEF\x87\xAB", 3) == 0,
                "WiFi glyph prefix is 3-byte FontAwesome sequence");

    TEST_SUITE("en_build_usb_label");

    char ubuf[16];

    /* ── Dongle présent, USB actif ────────────────────────────────── */
    en_build_usb_label(ubuf, sizeof(ubuf), true, true);
    TEST_ASSERT(strcmp(ubuf, LV_SYMBOL_USB " on") == 0, "USB on");

    /* ── Dongle présent, USB inactif ─────────────────────────────── */
    en_build_usb_label(ubuf, sizeof(ubuf), true, false);
    TEST_ASSERT(strcmp(ubuf, LV_SYMBOL_USB " off") == 0, "USB off");

    /* ── Dongle absent : '?' quelle que soit la valeur usb_active ── */
    en_build_usb_label(ubuf, sizeof(ubuf), false, true);
    TEST_ASSERT(strcmp(ubuf, LV_SYMBOL_USB " ?") == 0,
                "dongle absent usb_active=true -> '?'");

    en_build_usb_label(ubuf, sizeof(ubuf), false, false);
    TEST_ASSERT(strcmp(ubuf, LV_SYMBOL_USB " ?") == 0,
                "dongle absent usb_active=false -> '?'");

    /* ── Préfixe USB glyph présent ───────────────────────────────── */
    TEST_ASSERT(memcmp(ubuf, "\xEF\x8a\x87", 3) == 0,
                "USB glyph prefix is 3-byte FontAwesome sequence");
}
