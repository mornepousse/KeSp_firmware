#include "test_framework.h"
#include "../main/comm/espnow/espnow_msg.h"
#include <string.h>

void test_espnow_msg(void)
{
    TEST_SUITE("espnow_msg");

    /* ── Struct size verification ────────────────────────────── */
    TEST_ASSERT_EQ(sizeof(en_battery_t), 3, "en_battery_t is 3 bytes (packed)");
    TEST_ASSERT_EQ(sizeof(en_layer_t),  17, "en_layer_t is 17 bytes (1+16, packed)");
    TEST_ASSERT_EQ(sizeof(en_state_t),   2, "en_state_t is 2 bytes (packed)");

    /* ── EN_INFO_BATTERY round-trip ─────────────────────────── */
    {
        en_battery_t b_in  = { .batt_dV = 74, .soc_pct = 85, .charging = 1 };
        en_battery_t b_out = { 0 };
        uint8_t buf[1 + sizeof(en_battery_t)];

        uint8_t n = en_encode_battery(buf, &b_in);
        TEST_ASSERT_EQ(n, 4, "en_encode_battery returns 4 bytes (1+3)");
        TEST_ASSERT_EQ(buf[0], EN_INFO_BATTERY, "type byte is EN_INFO_BATTERY");

        TEST_ASSERT(en_decode_battery(buf, n, &b_out), "en_decode_battery succeeds");
        TEST_ASSERT_EQ(b_out.batt_dV,  74, "batt_dV round-trips");
        TEST_ASSERT_EQ(b_out.soc_pct,  85, "soc_pct round-trips");
        TEST_ASSERT_EQ(b_out.charging,  1, "charging round-trips");

        /* Wrong type byte must fail decode */
        buf[0] = EN_INFO_LAYER;
        TEST_ASSERT(!en_decode_battery(buf, n, &b_out), "wrong type byte fails decode");

        /* Truncated buffer must fail decode */
        buf[0] = EN_INFO_BATTERY;
        TEST_ASSERT(!en_decode_battery(buf, 2, &b_out), "truncated buffer fails decode");
    }

    /* ── EN_INFO_LAYER round-trip ────────────────────────────── */
    {
        en_layer_t l_in = { .layer_idx = 3 };
        strncpy(l_in.name, "Gaming", 16);
        en_layer_t l_out = { 0 };
        uint8_t buf[1 + sizeof(en_layer_t)];

        uint8_t n = en_encode_layer(buf, &l_in);
        TEST_ASSERT_EQ(n, 18, "en_encode_layer returns 18 bytes (1+17)");
        TEST_ASSERT_EQ(buf[0], EN_INFO_LAYER, "type byte is EN_INFO_LAYER");

        TEST_ASSERT(en_decode_layer(buf, n, &l_out), "en_decode_layer succeeds");
        TEST_ASSERT_EQ(l_out.layer_idx, 3, "layer_idx round-trips");
        TEST_ASSERT(memcmp(l_out.name, "Gaming", 6) == 0, "name round-trips");

        /* 16-char name (no null terminator) */
        en_layer_t l16 = { .layer_idx = 0 };
        memset(l16.name, 'A', 16);   /* no null terminator */
        uint8_t buf16[1 + sizeof(en_layer_t)];
        en_encode_layer(buf16, &l16);
        en_layer_t l16_out = { 0 };
        TEST_ASSERT(en_decode_layer(buf16, sizeof(buf16), &l16_out), "16-char name decodes");
        TEST_ASSERT(memcmp(l16_out.name, l16.name, 16) == 0, "16-char name matches exactly");
    }

    /* ── EN_INFO_STATE round-trip ────────────────────────────── */
    {
        en_state_t s_in  = { .modifiers = 0x02, .flags = 0x05 };   /* left shift + caps_word + usb_active */
        en_state_t s_out = { 0 };
        uint8_t buf[1 + sizeof(en_state_t)];

        uint8_t n = en_encode_state(buf, &s_in);
        TEST_ASSERT_EQ(n, 3, "en_encode_state returns 3 bytes (1+2)");
        TEST_ASSERT_EQ(buf[0], EN_INFO_STATE, "type byte is EN_INFO_STATE");

        TEST_ASSERT(en_decode_state(buf, n, &s_out), "en_decode_state succeeds");
        TEST_ASSERT_EQ(s_out.modifiers, 0x02, "modifiers round-trips");
        TEST_ASSERT_EQ(s_out.flags,     0x05, "flags round-trips");
    }

    /* ── Type ID uniqueness ────────────────────────────────────── */
    TEST_ASSERT(EN_INFO_BATTERY != EN_INFO_LAYER,  "BATTERY != LAYER");
    TEST_ASSERT(EN_INFO_BATTERY != EN_INFO_STATE,  "BATTERY != STATE");
    TEST_ASSERT(EN_INFO_LAYER   != EN_INFO_STATE,  "LAYER != STATE");

    /* ── Zero / unknown struct values ────────────────────────── */
    {
        en_battery_t b_zero = { .batt_dV = 0, .soc_pct = 0, .charging = 0 };
        uint8_t buf[4];
        en_encode_battery(buf, &b_zero);
        en_battery_t b_out = { 0xFF, 0xFF, 0xFF };
        en_decode_battery(buf, sizeof(buf), &b_out);
        TEST_ASSERT_EQ(b_out.batt_dV, 0, "batt_dV zero encodes/decodes as 0 (unknown stub value)");
        TEST_ASSERT_EQ(b_out.soc_pct, 0, "soc_pct zero encodes/decodes as 0");
    }
}
