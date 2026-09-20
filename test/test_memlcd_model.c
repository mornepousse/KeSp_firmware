/* Sharp memory-LCD screen of the halves — pure logic.
 *
 * Three things a bug would make visible on screen without ever crashing:
 *  - rev8: the panel reads LSB-first, the ESP32 transmits MSB-first. A wrong
 *    inversion = ignored commands, silent screen, no error anywhere.
 *  - the layer name cut at 68 px: 4 characters per line, 3 lines,
 *    then "…" — the user preferred readable lines over rotated text.
 *  - the model diff: we only redraw if a displayed field has changed —
 *    every redraw is a transaction on the bus shared with the radio.
 * Spec: docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md */
#include "test_framework.h"
#include "../main/display/memlcd/memlcd_model.h"
#include <string.h>

static void test_rev8(void)
{
    TEST_ASSERT_EQ(memlcd_rev8(0x01), 0x80, "bit0 → bit7");
    TEST_ASSERT_EQ(memlcd_rev8(0x80), 0x01, "bit7 → bit0");
    TEST_ASSERT_EQ(memlcd_rev8(0xA5), 0xA5, "0xA5 is a binary palindrome");
    TEST_ASSERT_EQ(memlcd_rev8(0x0F), 0xF0, "nibbles swapped bit by bit");
    TEST_ASSERT_EQ(memlcd_rev8(0x86), 0x61, "0x86 (1000 0110) → 0x61 (0110 0001)");
    for (unsigned b = 0; b < 256; b++)
        TEST_ASSERT_EQ(memlcd_rev8(memlcd_rev8((uint8_t)b)), (uint8_t)b, "involution");
}

static void test_couper_nom(void)
{
    char l[MEMLCD_NOM_LIGNES][MEMLCD_NOM_BUF];
    TEST_ASSERT_EQ(memlcd_couper_nom("DVORAK", l), 2, "6 letters → 2 lines");
    TEST_ASSERT(strcmp(l[0], "DVOR") == 0 && strcmp(l[1], "AK") == 0, "DVOR / AK");
    TEST_ASSERT(l[2][0] == '\0', "3rd line empty");
    TEST_ASSERT_EQ(memlcd_couper_nom("NAV", l), 1, "3 letters → 1 line");
    TEST_ASSERT(strcmp(l[0], "NAV") == 0, "NAV");
    TEST_ASSERT_EQ(memlcd_couper_nom("", l), 1, "empty → 1 empty line (never 0)");
    TEST_ASSERT(l[0][0] == '\0', "empty line");
    TEST_ASSERT_EQ(memlcd_couper_nom(NULL, l), 1, "NULL → like empty, no crash");
    TEST_ASSERT_EQ(memlcd_couper_nom("ABCDEFGHIJKL", l), 3, "12 letters → 3 full lines");
    TEST_ASSERT(strcmp(l[2], "IJKL") == 0, "3rd line full, no …");
    TEST_ASSERT_EQ(memlcd_couper_nom("ABCDEFGHIJKLMNOP", l), 3, "16 letters → 3 lines, truncated");
    TEST_ASSERT(strcmp(l[2], "IJK\xE2\x80\xA6") == 0, "3rd line = 3 letters + … (UTF-8)");
}

static void test_model_diff(void)
{
    memlcd_model_t a = { .route_rf = 1, .dongle_vu = 1, .batt_local_dv = 40,
                         .couche = 1, .nom = "DVORAK", .is_left = 1 };
    memlcd_model_t b = a;
    TEST_ASSERT(!memlcd_model_diff(&a, &b), "identical → no redraw");
    b.batt_local_dv = 39;  TEST_ASSERT(memlcd_model_diff(&a, &b), "local voltage changes → redraw");
    b = a; b.batt_local_chg = 2; TEST_ASSERT(memlcd_model_diff(&a, &b), "charge state changes → redraw");
    b = a; b.couche = 2;   TEST_ASSERT(memlcd_model_diff(&a, &b), "layer changes → redraw");
    b = a; strcpy(b.nom, "NAV"); TEST_ASSERT(memlcd_model_diff(&a, &b), "name changes → redraw");
    b = a; b.dongle_vu = 0; TEST_ASSERT(memlcd_model_diff(&a, &b), "dongle lost → redraw");
    b = a; b.is_left = 0;  TEST_ASSERT(!memlcd_model_diff(&a, &b), "is_left is not a displayed field that moves");
}

/* The panel is PHYSICALLY 68 lines of 160 pixels (Sharp catalog, doc
 * lemia 6844 p. 5: "LS011B7DH03 160 × 68", H = direction of data); it is
 * mounted upright. The portrait framebuffer (68 × 160, 9 bytes per row, bit 7 =
 * x = 0, 1 = ink) therefore transposes into 68 lines of 20 bytes, bit 7 = D1,
 * 1 = WHITE (app note doc 6845 p. 10: D(n) = L → black). */
static void test_fb_to_panel(void)
{
    static uint8_t fb[MEMLCD_H * MEMLCD_LINE_BYTES];
    static uint8_t panel[MEMLCD_PANEL_LINES * MEMLCD_PANEL_LINE_BYTES];
    TEST_ASSERT_EQ(MEMLCD_PANEL_LINES, 68, "68 grid lines");
    TEST_ASSERT_EQ(MEMLCD_PANEL_LINE_BYTES, 20, "160 pixels per line = 20 bytes");

    memset(fb, 0, sizeof fb);
    memlcd_fb_to_panel(fb, panel, false);
    bool blanc = true;
    for (size_t i = 0; i < sizeof panel; i++) if (panel[i] != 0xFF) blanc = false;
    TEST_ASSERT(blanc, "empty buffer → panel all white (1 = white for Sharp)");

    /* portrait pixel (x=0, y=0): top-left corner → line 0, column 159 (90° rotation) */
    fb[0] = 0x80;
    memlcd_fb_to_panel(fb, panel, false);
    TEST_ASSERT_EQ(panel[0 * MEMLCD_PANEL_LINE_BYTES + 19], 0xFE, "(0,0) → line 0, D160 (bit 0 of last byte) black");
    TEST_ASSERT_EQ(panel[1 * MEMLCD_PANEL_LINE_BYTES + 19], 0xFF, "line 1 is untouched");

    /* pixel (x=67, y=159): bottom-right corner → line 67, column 0 (D1 = bit 7 of byte 0) */
    memset(fb, 0, sizeof fb);
    fb[159 * MEMLCD_LINE_BYTES + 8] = 0x10;   /* x = 67 = byte 8, bit (7 - 3) */
    memlcd_fb_to_panel(fb, panel, false);
    TEST_ASSERT_EQ(panel[67 * MEMLCD_PANEL_LINE_BYTES + 0], 0x7F, "(67,159) → line 67, D1 black");

    /* 180° rotation: (0,0) → line 67, column 0 */
    memset(fb, 0, sizeof fb); fb[0] = 0x80;
    memlcd_fb_to_panel(fb, panel, true);
    TEST_ASSERT_EQ(panel[67 * MEMLCD_PANEL_LINE_BYTES + 0], 0x7F, "rot180: (0,0) → line 67, D1");
    TEST_ASSERT_EQ(panel[0 * MEMLCD_PANEL_LINE_BYTES + 19], 0xFF, "rot180: line 0 stays white");
}

/* The displayed voltage: the ADC oscillates between two neighboring dV values
 * (a panel rewrite each time), but a "±1 around the DISPLAYED value" hysteresis
 * froze 4.2 V for a whole night while the battery lost 0.1 V (bench 2026-09-15).
 * Rule: a value DIFFERENT from the displayed one is shown once it has HELD for
 * hold_ms straight — oscillation never holds, drift eventually holds. */
static void test_batt_affichee(void)
{
    memlcd_batt_aff_t b; memlcd_batt_aff_init(&b);
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 42, 0, 30000), 42, "first measurement: displayed right away");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 41, 10000, 30000), 42, "41 since 0 s: not yet");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 42, 20000, 30000), 42, "back to 42: the count for 41 restarts from zero");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 41, 30000, 30000), 42, "41 again, since 0 s");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 41, 50000, 30000), 42, "41 since 20 s: not yet");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 41, 60000, 30000), 41, "41 since 30 s: the drift is displayed");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 40, 60001, 30000), 41, "40: new candidate, restarts from zero");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 40, 90001, 30000), 40, "40 since 30 s: still follows");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 0xFF, 90002, 30000), 0xFF, "unknown: displayed without delay (not an oscillation)");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 39, 90003, 30000), 39, "return of a measurement after unknown: no delay");
}

void test_memlcd_model(void)
{
    TEST_SUITE("Memory-LCD screen: pure logic");
    test_rev8();
    test_couper_nom();
    test_model_diff();
    test_fb_to_panel();
    test_batt_affichee();
}
