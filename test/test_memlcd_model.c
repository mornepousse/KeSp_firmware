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
#include "../main/comm/chest/chest_view.h"
#include "chest_test_vectors.h"
#include <string.h>
#include <stdlib.h>

#define V1  CHEST_TV_V1
#define V9  CHEST_TV_V9
#define V15 CHEST_TV_V15
#define V16 CHEST_TV_V16
#define L1  CHEST_TV_L1
#define C1  CHEST_TV_C1

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
    /* Full width under the icon column since 2026-09-26: 6 characters per
     * line (Montserrat 14 on 68 px — "LAYER 2" is ~56 px), 2 lines, cut at
     * the last space that fits so a word is not split when it can be avoided. */
    char l[MEMLCD_NOM_LIGNES][MEMLCD_NOM_BUF];
    TEST_ASSERT_EQ(MEMLCD_NOM_LIGNES, 2, "two name lines");
    TEST_ASSERT_EQ(MEMLCD_NOM_COLS, 6, "six characters per line");
    TEST_ASSERT_EQ(memlcd_couper_nom("DVORAK", l), 1, "6 letters → 1 line");
    TEST_ASSERT(strcmp(l[0], "DVORAK") == 0 && l[1][0] == '\0', "DVORAK, 2nd line empty");
    TEST_ASSERT_EQ(memlcd_couper_nom("NAV", l), 1, "3 letters → 1 line");
    TEST_ASSERT(strcmp(l[0], "NAV") == 0, "NAV");
    TEST_ASSERT_EQ(memlcd_couper_nom("", l), 1, "empty → 1 empty line (never 0)");
    TEST_ASSERT(l[0][0] == '\0', "empty line");
    TEST_ASSERT_EQ(memlcd_couper_nom(NULL, l), 1, "NULL → like empty, no crash");
    TEST_ASSERT_EQ(memlcd_couper_nom("LAYER 2", l), 2, "7 characters → 2 lines");
    TEST_ASSERT(strcmp(l[0], "LAYER") == 0 && strcmp(l[1], "2") == 0, "cut at the space: LAYER / 2");
    TEST_ASSERT_EQ(memlcd_couper_nom("GAMING", l), 1, "exactly 6 → 1 line");
    TEST_ASSERT_EQ(memlcd_couper_nom("SYMBOLS", l), 2, "7 letters, no space → hard cut");
    TEST_ASSERT(strcmp(l[0], "SYMBOL") == 0 && strcmp(l[1], "S") == 0, "SYMBOL / S");
    TEST_ASSERT_EQ(memlcd_couper_nom("NUM PAD", l), 2, "space at 3");
    TEST_ASSERT(strcmp(l[0], "NUM") == 0 && strcmp(l[1], "PAD") == 0, "NUM / PAD");
    TEST_ASSERT_EQ(memlcd_couper_nom("ABCDEFGHIJKL", l), 2, "12 letters → 2 full lines");
    TEST_ASSERT(strcmp(l[1], "GHIJKL") == 0, "2nd line full, no …");
    TEST_ASSERT_EQ(memlcd_couper_nom("ABCDEFGHIJKLMN", l), 2, "14 letters → truncated");
    TEST_ASSERT(strcmp(l[1], "GHIJK\xE2\x80\xA6") == 0, "2nd line = 5 letters + … (UTF-8)");
    TEST_ASSERT_EQ(memlcd_couper_nom("MEDIA KEYS 2", l), 2, "space cut, the rest fits exactly");
    TEST_ASSERT(strcmp(l[0], "MEDIA") == 0 && strcmp(l[1], "KEYS 2") == 0, "MEDIA / KEYS 2");
    TEST_ASSERT_EQ(memlcd_couper_nom("MEDIA KEYS ABC", l), 2, "space cut, then overflow");
    TEST_ASSERT(strcmp(l[1], "KEYS\xE2\x80\xA6") == 0, "the ellipsis never follows a space: KEYS…");
}

/* Displayed layer: the STABLE one (base, TO, Layer Lock) = the engine's
 * last_layer, never a held MO/LT/LM. In RF the left does not hear the right
 * (the dongle runs the engine), so a MO on the right was invisible and a mix
 * of thumbs left the screen on the wrong layer; a status screen at 1 s cannot
 * follow a momentary layer anyway. Mae, 2026-09-26: "if we can't be reactive,
 * we don't handle MOs". */
static void test_couche_affichee(void)
{
    TEST_ASSERT_EQ(memlcd_couche_affichee(0, 0), 0, "base layer");
    TEST_ASSERT_EQ(memlcd_couche_affichee(1, 0), 0, "MO(1) held from base → still base");
    TEST_ASSERT_EQ(memlcd_couche_affichee(3, 3), 3, "TO(3) / layer lock → 3");
    TEST_ASSERT_EQ(memlcd_couche_affichee(1, 3), 3, "MO(1) held over a locked 3 → 3");
}

/* Status lines, UNSCII 8 = 8 characters on 68 px. Line 1: what capitalises
 * (host Caps Lock, Caps Word). Line 2: armed one-shots — modifiers as letters
 * C S A G (left and right merged), then the one-shot layer. Empty when
 * nothing is armed: the screen says nothing when there is nothing to say. */
static void test_ligne_etat(void)
{
    char l1[MEMLCD_ETAT_BUF], l2[MEMLCD_ETAT_BUF];
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE };
    memlcd_ligne_etat(&m, l1, l2);
    TEST_ASSERT(l1[0] == '\0' && l2[0] == '\0', "nothing armed → both lines empty");

    m.caps_lock = 1;
    memlcd_ligne_etat(&m, l1, l2);
    TEST_ASSERT(strcmp(l1, "CAPS") == 0, "Caps Lock");
    m.caps_word = 1;
    memlcd_ligne_etat(&m, l1, l2);
    TEST_ASSERT(strcmp(l1, "CAPS CW") == 0, "Caps Lock + Caps Word");
    m.caps_lock = 0;
    memlcd_ligne_etat(&m, l1, l2);
    TEST_ASSERT(strcmp(l1, "CW") == 0, "Caps Word alone");

    memlcd_model_t o = { .osl = MEMLCD_OSL_AUCUNE, .osm = 0x02 };   /* LShift */
    memlcd_ligne_etat(&o, l1, l2);
    TEST_ASSERT(strcmp(l2, "S") == 0, "one-shot Shift");
    o.osm = 0x20 | 0x01;                                             /* RShift + LCtrl */
    memlcd_ligne_etat(&o, l1, l2);
    TEST_ASSERT(strcmp(l2, "CS") == 0, "right Shift merged, fixed order C S A G");
    o.osm = 0xFF;
    memlcd_ligne_etat(&o, l1, l2);
    TEST_ASSERT(strcmp(l2, "CSAG") == 0, "all four");
    o.osl = 3;
    memlcd_ligne_etat(&o, l1, l2);
    TEST_ASSERT(strcmp(l2, "CSAG L3") == 0, "mods + one-shot layer, 7 characters");
    TEST_ASSERT(strlen(l2) <= 8, "fits the 8 UNSCII columns");
    o.osm = 0;
    memlcd_ligne_etat(&o, l1, l2);
    TEST_ASSERT(strcmp(l2, "L3") == 0, "one-shot layer alone, no leading space");
}

/* Chest status under the logo (plan Task 8, "SD c'est confusant", Mae
 * 2026-09-29): a PADLOCK pictogram = chest present; ".." beside it while not
 * READY, "?" on a protocol version mismatch; the mode in plain words
 * (chest_mode_label: DISK/PGP/OTP/FIDO/TOTP, lower case in flight, ERR);
 * the SD card only when MISSING ("NO" / "CARD"). Nothing without a chest. */
static void test_etat_coffre(void)
{
    memlcd_etat_coffre_t e;
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE };
    memlcd_etat_coffre(&m, &e);
    TEST_ASSERT(!e.cadenas && !e.ligne[0][0] && !e.ligne[1][0] && !e.ligne[2][0], "absent: nothing at all");

    m.coffre = MEMLCD_COFFRE_PRESENT;
    memlcd_etat_coffre(&m, &e);
    TEST_ASSERT(e.cadenas, "present, booting: the padlock");
    TEST_ASSERT(strcmp(e.ligne[0], "..") == 0 && !e.ligne[1][0] && !e.ligne[2][0], "booting: '..' and nothing else (no NO CARD before READY)");

    m.coffre = MEMLCD_COFFRE_PRESENT | MEMLCD_COFFRE_BADVER;
    memlcd_etat_coffre(&m, &e);
    TEST_ASSERT(e.cadenas && strcmp(e.ligne[0], "?") == 0 && !e.ligne[1][0], "unknown protocol version: padlock + ?");

    /* Ready, card in, PGP mounted: the padlock and the mode, NO SD line. */
    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_USB;
    m.coffre_mode_state = CHEST_MODE_ARRIVED; m.coffre_mode_active = CHEST_MODE_PGP; m.coffre_mode_wanted = CHEST_MODE_PGP;
    memlcd_etat_coffre(&m, &e);
    TEST_ASSERT(e.cadenas && strcmp(e.ligne[0], "PGP") == 0, "ready: padlock + PGP");
    TEST_ASSERT(!e.ligne[1][0] && !e.ligne[2][0], "card present: nothing about the card (no more 'SD')");
    for (int i = 0; i < 3; i++) TEST_ASSERT(strstr(e.ligne[i], "SD") == NULL, "the word SD is gone");

    /* Card missing: NO / CARD under the mode. */
    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY;
    m.coffre_mode_state = CHEST_MODE_PENDING; m.coffre_mode_active = CHEST_MODE_IN_FLIGHT; m.coffre_mode_wanted = CHEST_MODE_OATH;
    memlcd_etat_coffre(&m, &e);
    TEST_ASSERT(strcmp(e.ligne[0], "totp") == 0, "pending: the wanted mode, lower case, in plain words");
    TEST_ASSERT(strcmp(e.ligne[1], "NO") == 0 && strcmp(e.ligne[2], "CARD") == 0, "card missing: NO / CARD");

    /* No mode yet, card missing: NO CARD moves up, no blank line. */
    m.coffre_mode_state = CHEST_MODE_ARRIVED; m.coffre_mode_active = CHEST_MODE_NONE; m.coffre_mode_wanted = CHEST_MODE_NONE;
    memlcd_etat_coffre(&m, &e);
    TEST_ASSERT(strcmp(e.ligne[0], "NO") == 0 && strcmp(e.ligne[1], "CARD") == 0 && !e.ligne[2][0], "no mode: NO CARD packed upwards");

    m.coffre_mode_state = CHEST_MODE_FAULT;
    memlcd_etat_coffre(&m, &e);
    TEST_ASSERT(strcmp(e.ligne[0], "ERR") == 0, "persisting refusal: ERR");

    /* Every line fits the 35 px zone left of the icon column in UNSCII 8. */
    static const uint8_t modes[] = { CHEST_MODE_STORAGE, CHEST_MODE_PGP, CHEST_MODE_OTP, CHEST_MODE_FIDO, CHEST_MODE_OATH };
    for (size_t i = 0; i < sizeof modes; i++) {
        m.coffre_mode_state = CHEST_MODE_ARRIVED; m.coffre_mode_active = modes[i]; m.coffre_mode_wanted = modes[i];
        memlcd_etat_coffre(&m, &e);
        for (int k = 0; k < 3; k++)
            TEST_ASSERT(memlcd_text_width(MEMLCD_F_U8, e.ligne[k]) <= MEMLCD_ETAT_COFFRE_W, "status line fits the 35 px zone");
    }
    /* The padlock and three UNSCII lines stay between the logo and the
     * separator. UNSCII 8 (line_height 9, base_line 0) inks rows y+1..y+8
     * of a label at y; the 34 px logo's ink ends at y 35. */
    TEST_ASSERT(MEMLCD_CADENAS_Y > 35, "the padlock starts under the logo's ink");
    TEST_ASSERT(MEMLCD_CADENAS_Y + MEMLCD_CADENAS_H <= MEMLCD_ETAT_COFFRE_Y0 + 1, "the first line's ink starts under the padlock");
    TEST_ASSERT(MEMLCD_ETAT_COFFRE_Y0 + 2 * MEMLCD_ETAT_COFFRE_PAS + 9 <= MEMLCD_Y_SEP, "the third line's ink ends above the separator");
    TEST_ASSERT(MEMLCD_CADENAS_W >= 24 && MEMLCD_CADENAS_H >= 28, "a BIG padlock (Mae: 24 x 28 or more)");
    TEST_ASSERT(MEMLCD_CADENAS_X + MEMLCD_CADENAS_W <= MEMLCD_ETAT_COFFRE_W, "the padlock fits the zone's width");
}

/* 34 characters, CHEST_LABEL_MAX — the alphabet (26) plus "ABCDEFGH" (8). */
#define LABEL34 "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGH"
/* A realistic 34-character label: lower case, digits, punctuation. */
#define LABEL34N "github.com:alice.martin@work-2fa01"
/* 34 of the widest upper-case letter. */
#define LABEL34W "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"

/* The width oracle itself: sums of the generated LVGL advances, UNSCII 8 px
 * per character. Values checked by hand against lv_font_montserrat_*.c
 * (adv_w 252 for 'W' in M14 -> (252 + 8) >> 4 = 16 px). */
static void test_text_width(void)
{
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_U8, "ABCDEFGH"), 64, "UNSCII: 8 px a character");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M14, "W"), 16, "M14 W: adv 252/16 rounded");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M14, "I"), 4, "M14 I: adv 69/16 rounded");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M14, "WWWW"), 64, "per-glyph rounding, summed");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M24, "TOTP"), 65, "M24 TOTP");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M28, "418"), 47, "M28 418");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M14, ""), 0, "empty");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M14, NULL), 0, "NULL");
    TEST_ASSERT_EQ(memlcd_font_pas(MEMLCD_F_U8), 11, "UNSCII pitch");
    TEST_ASSERT_EQ(memlcd_font_pas(MEMLCD_F_M14), 16, "M14 line");
    TEST_ASSERT_EQ(memlcd_font_pas(MEMLCD_F_M24), 27, "M24 line");
    TEST_ASSERT_EQ(memlcd_font_pas(MEMLCD_F_M28), 30, "M28 line");
    /* The widest printable M14 character must fit the budget on its own:
     * otherwise the pixel cutter could not make progress. */
    for (int c = 0x20; c <= 0x7E; c++) {
        char s[2] = { (char)c, 0 };
        TEST_ASSERT(memlcd_text_width(MEMLCD_F_M14, s) <= MEMLCD_W_BUDGET, "one M14 glyph fits a line");
    }
}

/* memlcd_couper_px: hard cut by PIXEL width (untrusted text, no word
 * awareness) — every line <= MEMLCD_W_BUDGET, the concatenation is the
 * text, and the return value says how many lines the WHOLE text needs even
 * when fewer were written. */
static void test_couper_px(void)
{
    memlcd_vc_line_t l[8];
    TEST_ASSERT_EQ(memlcd_couper_px(MEMLCD_F_U8, "", 4, l), 1, "empty -> 1 empty line, never 0");
    TEST_ASSERT(l[0].text[0] == '\0', "empty line");
    TEST_ASSERT_EQ(memlcd_couper_px(MEMLCD_F_U8, NULL, 4, l), 1, "NULL -> like empty");
    TEST_ASSERT_EQ(memlcd_couper_px(MEMLCD_F_U8, "ABCDEFGH", 4, l), 1, "8 UNSCII characters = 64 px, one line");
    TEST_ASSERT_EQ(memlcd_couper_px(MEMLCD_F_U8, "ABCDEFGHI", 4, l), 2, "9 -> 2 lines");
    TEST_ASSERT(strcmp(l[0].text, "ABCDEFGH") == 0 && strcmp(l[1].text, "I") == 0, "8 + 1");
    TEST_ASSERT_EQ(memlcd_couper_px(MEMLCD_F_U8, LABEL34, 5, l), 5, "34 chars, 5 UNSCII lines");
    TEST_ASSERT(strcmp(l[4].text, "GH") == 0, "the last 2");
    TEST_ASSERT_EQ(memlcd_couper_px(MEMLCD_F_U8, LABEL34, 3, l), 5, "only 3 written, but 5 NEEDED is returned");
    TEST_ASSERT_EQ(memlcd_couper_px(MEMLCD_F_M14, LABEL34W, 8, l), 9, "34 W in M14: 4 a line (64 px), 9 lines");
    TEST_ASSERT(strcmp(l[0].text, "WWWW") == 0, "4 W = 64 px <= 66, a 5th would be 80");
    for (int i = 0; i < 8; i++) TEST_ASSERT(l[i].font == MEMLCD_F_M14, "font set on every written line");

    /* The tilde variant: the cut is never silent, and the marked line still fits. */
    TEST_ASSERT_EQ(memlcd_couper_px_tilde(MEMLCD_F_M14, "OVH:PERSONAL-ACCOUNT-2", 2, l), 2, "long name, 2 lines");
    size_t n1 = strlen(l[1].text);
    TEST_ASSERT(n1 > 0 && l[1].text[n1 - 1] == '~', "the 2nd line ends in ~");
    TEST_ASSERT(memlcd_text_width(MEMLCD_F_M14, l[1].text) <= MEMLCD_W_BUDGET, "the ~ line fits");
    TEST_ASSERT_EQ(memlcd_couper_px_tilde(MEMLCD_F_M14, "WORK", 2, l), 1, "short: one line, no ~");
    TEST_ASSERT(strcmp(l[0].text, "WORK") == 0, "WORK untouched");
}

/* Bounded scan (review M-b, carried over from memlcd_couper_8): 34 bytes, no
 * NUL anywhere, in a heap block of exactly 34 — strnlen(s, CHEST_LABEL_MAX)
 * never looks past byte 33; a strlen would, and ASan (test_chest_sanitized)
 * catches it. */
static void test_couper_px_bounded_scan_no_terminator(void)
{
    char *buf = malloc(34);
    TEST_ASSERT(buf != NULL, "allocation for the bounded-scan probe");
    if (!buf) return;
    memset(buf, 'X', 34);
    memlcd_vc_line_t l[5];
    TEST_ASSERT_EQ(memlcd_couper_px(MEMLCD_F_U8, buf, 5, l), 5, "34 X's, unterminated: 5 UNSCII lines");
    TEST_ASSERT(strcmp(l[0].text, "XXXXXXXX") == 0 && strcmp(l[4].text, "XX") == 0, "34 X's split 8/8/8/8/2");
    free(buf);
}

/* Every line of a view within the width budget, every line inside its zone. */
static void assert_vue_tient(const memlcd_vue_coffre_t *v, const char *quoi)
{
    uint8_t zone = (v->kind == MEMLCD_VC_BROWSE) ? MEMLCD_ZONE_BAS_H : MEMLCD_H;
    TEST_ASSERT(v->n <= MEMLCD_VC_LIGNES, quoi);
    for (uint8_t i = 0; i < v->n; i++) {
        TEST_ASSERT(memlcd_text_width(v->l[i].font, v->l[i].text) <= MEMLCD_W_BUDGET, quoi);
        TEST_ASSERT(v->l[i].y + memlcd_font_pas(v->l[i].font) <= zone, quoi);
        if (i > 0 && v->l[i].y < v->l[i - 1].y + memlcd_font_pas(v->l[i - 1].font))
            TEST_ASSERT(0, "lines overlap");
    }
}

/* Concatenation of the label/name lines — what the eye reads across them. */
static void vue_texte(const memlcd_vue_coffre_t *v, char *out, size_t n)
{
    out[0] = '\0';
    for (uint8_t i = v->txt_i; i < v->txt_i + v->txt_n && i < v->n; i++)
        strncat(out, v->l[i].text, n - strlen(out) - 1);
}
static int vue_cherche(const memlcd_vue_coffre_t *v, const char *s)
{
    for (uint8_t i = 0; i < v->n; i++) if (strcmp(v->l[i].text, s) == 0) return i;
    return -1;
}

/* The prompt takes the WHOLE screen (plan Task 8): op in M24 when it fits,
 * the chest's label in M14 cut by pixel width, falling back to UNSCII 8
 * rather than ever cutting it (C1 of Task 5 stands); N CPT; OK ?. */
static void test_vue_prompt(void)
{
    memlcd_vue_coffre_t v;
    char txt[64];
    memlcd_model_t m = {0};
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_NONE, "nothing pending, no code, not browsing: the normal screen");

    m.coffre_op = 7; strcpy(m.coffre_label, "GITHUB"); m.coffre_op_count = 1;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_PROMPT, "prompt");
    assert_vue_tient(&v, "TOTP/GITHUB fits");
    TEST_ASSERT(strcmp(v.l[0].text, "TOTP") == 0 && v.l[0].font == MEMLCD_F_M24, "the op, alone, in M24 (65 px)");
    TEST_ASSERT(v.rule_y > v.l[0].y && v.rule_y < v.l[1].y, "a rule between the op and the label");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "GITHUB") == 0 && v.l[v.txt_i].font == MEMLCD_F_M14, "the label in M14");
    TEST_ASSERT(strcmp(v.l[v.n - 1].text, "OK ?") == 0 && v.l[v.n - 1].font == MEMLCD_F_M14, "last line OK ? in M14");
    TEST_ASSERT(vue_cherche(&v, "1 CPT") < 0, "op_count 1: no N CPT");

    /* An op label too wide for M24 falls back to M14 — RESET! is 84 px in M24. */
    m.coffre_op = 10;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT(strcmp(v.l[0].text, "RESET!") == 0 && v.l[0].font == MEMLCD_F_M14, "RESET! > 66 px in M24: M14");
    /* Every op label, whichever font it gets, fits. */
    static const uint16_t ops[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 42, 99, 100, 1042 };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        m.coffre_op = ops[i];
        memlcd_vue_coffre(&m, &v);
        assert_vue_tient(&v, "every op label fits");
    }

    /* A 34-character realistic label: M14, whole. */
    m.coffre_op = 7; m.coffre_op_count = 1; strcpy(m.coffre_label, LABEL34N);
    memlcd_vue_coffre(&m, &v);
    assert_vue_tient(&v, "34-char normal label fits");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, LABEL34N) == 0, "34 normal characters: the whole label");
    TEST_ASSERT(v.l[v.txt_i].font == MEMLCD_F_M14, "... in M14");

    /* 34 W: 9 M14 lines do not fit the height -> UNSCII 8, whole, no ~. */
    strcpy(m.coffre_label, LABEL34W);
    memlcd_vue_coffre(&m, &v);
    assert_vue_tient(&v, "34 W fits");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, LABEL34W) == 0, "34 W: the whole label, never cut");
    TEST_ASSERT(v.l[v.txt_i].font == MEMLCD_F_U8 && v.txt_n == 5, "... in UNSCII 8, 5 lines");
    TEST_ASSERT(strchr(txt, '~') == NULL, "never a ~ on a prompt");

    /* The same with N CPT: still whole, N CPT and OK ? still there. */
    m.coffre_op = 10; m.coffre_op_count = 12;
    memlcd_vue_coffre(&m, &v);
    assert_vue_tient(&v, "34 W + 12 CPT fits");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, LABEL34W) == 0, "34 W with N CPT: whole");
    TEST_ASSERT(vue_cherche(&v, "12 CPT") > 0 && strcmp(v.l[v.n - 1].text, "OK ?") == 0, "12 CPT then OK ?");
    strcpy(m.coffre_label, LABEL34);
    memlcd_vue_coffre(&m, &v);
    assert_vue_tient(&v, "alphabet34 + 12 CPT fits");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, LABEL34) == 0, "the alphabet label with N CPT: whole");

    /* Two 34-char labels differing only at the last character render differently (C1). */
    {
        memlcd_vue_coffre_t a, b;
        char la[35], lb[35];
        strcpy(la, LABEL34); la[33] = '1';
        strcpy(lb, LABEL34); lb[33] = '2';
        strcpy(m.coffre_label, la); memlcd_vue_coffre(&m, &a);
        strcpy(m.coffre_label, lb); memlcd_vue_coffre(&m, &b);
        char ta[64], tb[64];
        vue_texte(&a, ta, sizeof ta); vue_texte(&b, tb, sizeof tb);
        TEST_ASSERT(strcmp(ta, la) == 0 && strcmp(tb, lb) == 0, "C1: both whole, so they differ");
    }

    /* op_count boundary: 2 is already more than one. */
    strcpy(m.coffre_label, "GITHUB"); m.coffre_op_count = 2;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT(vue_cherche(&v, "2 CPT") > 0, "op_count 2: N CPT");
    m.coffre_op_count = 0;
    memlcd_vue_coffre(&m, &v);
    for (uint8_t i = 0; i < v.n; i++) TEST_ASSERT(strstr(v.l[i].text, "CPT") == NULL, "op_count 0: no CPT");

    /* The prompt shows coffre_label, NEVER coffre_nom (the browser's copy). */
    memset(&m, 0, sizeof m);
    m.coffre_op = 7; strcpy(m.coffre_label, "GITHUB"); m.coffre_op_count = 1;
    strcpy(m.coffre_nom, "OVH:PRO"); m.coffre_browsing = 1; m.coffre_total = 3;
    memlcd_vue_coffre(&m, &v);
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "GITHUB") == 0, "the chest's label, not the cursor's");
    for (uint8_t i = 0; i < v.n; i++) TEST_ASSERT(strstr(v.l[i].text, "OVH") == NULL, "the browser's name nowhere on the prompt");

    /* Priority: prompt > code. The code's digits never leak onto a prompt. */
    m.coffre_code_visible = 1; strcpy(m.coffre_code, "418902"); m.coffre_code_secs = 12;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_PROMPT, "prompt beats a visible code");
    for (uint8_t i = 0; i < v.n; i++) {
        TEST_ASSERT(strstr(v.l[i].text, "418") == NULL && strstr(v.l[i].text, "902") == NULL, "no digit of the code on the prompt");
    }
}

/* The code takes the WHOLE screen: the name in M14 (<= 2 lines, ~ if cut),
 * 6 digits as two M28 lines of 3, 8 digits as two M24 lines of 4, a
 * countdown bar and "NN s" in M24 — only while coffre_code_visible. */
static void test_vue_code(void)
{
    memlcd_vue_coffre_t v;
    char txt[64];
    memlcd_model_t m = {0};
    m.coffre_code_visible = 1; strcpy(m.coffre_nom, "WORK"); strcpy(m.coffre_code, "418902"); m.coffre_code_secs = 12;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_CODE, "code");
    assert_vue_tient(&v, "6-digit code fits");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "WORK") == 0 && v.l[v.txt_i].font == MEMLCD_F_M14, "the name in M14");
    int a = vue_cherche(&v, "418"), b = vue_cherche(&v, "902");
    TEST_ASSERT(a >= 0 && b == a + 1, "6 digits: 418 then 902");
    TEST_ASSERT(a >= 0 && v.l[a].font == MEMLCD_F_M28 && v.l[b].font == MEMLCD_F_M28, "... in M28");
    int s = vue_cherche(&v, "12 s");
    TEST_ASSERT(s >= 0 && v.l[s].font == MEMLCD_F_M24, "12 s in M24");
    TEST_ASSERT_EQ(v.bar_pct, 40, "12 s of 30: 40 %");
    TEST_ASSERT(v.bar_y > v.l[b].y && v.bar_y + MEMLCD_BAR_H <= v.l[s].y, "the bar between the code and the seconds");

    /* Every digit, 3 in M28 / 4 in M24, fits. */
    for (char d = '0'; d <= '9'; d++) {
        char c6[7], c8[9];
        memset(c6, d, 6); c6[6] = 0; memset(c8, d, 8); c8[8] = 0;
        strcpy(m.coffre_code, c6); memlcd_vue_coffre(&m, &v); assert_vue_tient(&v, "6 x digit fits");
        strcpy(m.coffre_code, c8); memlcd_vue_coffre(&m, &v); assert_vue_tient(&v, "8 x digit fits");
    }

    strcpy(m.coffre_code, "12345678"); m.coffre_code_secs = 30;
    memlcd_vue_coffre(&m, &v);
    a = vue_cherche(&v, "1234"); b = vue_cherche(&v, "5678");
    TEST_ASSERT(a >= 0 && b == a + 1 && v.l[a].font == MEMLCD_F_M24 && v.l[b].font == MEMLCD_F_M24, "8 digits: two M24 lines of 4");
    TEST_ASSERT_EQ(v.bar_pct, 100, "30 s: full bar");
    TEST_ASSERT(vue_cherche(&v, "30 s") >= 0, "30 s");
    m.coffre_code_secs = 45;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.bar_pct, 100, "never above 100 %");

    /* A long name: 2 M14 lines, cut with ~ (the browser's copy, not a security text). */
    strcpy(m.coffre_nom, LABEL34);
    memlcd_vue_coffre(&m, &v);
    assert_vue_tient(&v, "long name fits");
    TEST_ASSERT(v.txt_n == 2, "the name on 2 lines");
    size_t n = strlen(v.l[v.txt_i + 1].text);
    TEST_ASSERT(n && v.l[v.txt_i + 1].text[n - 1] == '~', "cut with ~");

    /* No code without coffre_code_visible, whatever coffre_code holds. */
    memset(&m, 0, sizeof m);
    strcpy(m.coffre_code, "123456"); m.coffre_code_secs = 12; strcpy(m.coffre_nom, "WORK");
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_NONE, "code_visible false: never the code");
    m.coffre_browsing = 1; m.coffre_total = 1;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_BROWSE, "browsing, code not visible: the browser");
    for (uint8_t i = 0; i < v.n; i++) TEST_ASSERT(strstr(v.l[i].text, "123") == NULL, "no digit of an invisible code");

    /* Priority code > browse. */
    m.coffre_code_visible = 1;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_CODE, "code beats the browser");
}

/* The browser stays in the bottom zone (67 px): i/total, the name in M14 on
 * <= 2 lines (UNSCII 8 with ~ beyond), NO TIME when TIME_VALID is clear. */
static void test_vue_browse(void)
{
    memlcd_vue_coffre_t v;
    char txt[64];
    memlcd_model_t m = {0};
    m.coffre_browsing = 1; m.coffre_pos = 2; m.coffre_total = 12; strcpy(m.coffre_nom, "OVH:PRO");
    m.coffre = CHEST_STATE_TIME;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_BROWSE, "browse");
    assert_vue_tient(&v, "browse fits 67 px");
    TEST_ASSERT(strcmp(v.l[0].text, "3/12") == 0 && v.l[0].font == MEMLCD_F_M14, "3/12 in M14");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "OVH:PRO") == 0 && v.l[v.txt_i].font == MEMLCD_F_M14, "the name, whole, in M14");
    TEST_ASSERT(vue_cherche(&v, "NO TIME") < 0, "time set: no NO TIME");

    m.coffre = 0;
    memlcd_vue_coffre(&m, &v);
    assert_vue_tient(&v, "browse + NO TIME fits 67 px");
    int t = vue_cherche(&v, "NO TIME");
    TEST_ASSERT(t >= 0 && v.l[t].font == MEMLCD_F_M14, "TIME clear: NO TIME in M14");

    /* A name too long for 2 M14 lines: UNSCII 8, ~ marks the cut, still inside 67 px. */
    strcpy(m.coffre_nom, LABEL34);
    memlcd_vue_coffre(&m, &v);
    assert_vue_tient(&v, "long name + NO TIME fits 67 px");
    TEST_ASSERT(v.l[v.txt_i].font == MEMLCD_F_U8, "fallback UNSCII 8");
    TEST_ASSERT(vue_cherche(&v, "NO TIME") >= 0, "NO TIME still shown");
    m.coffre = CHEST_STATE_TIME;
    memlcd_vue_coffre(&m, &v);
    assert_vue_tient(&v, "long name fits 67 px");
    TEST_ASSERT(v.txt_n == 4, "without NO TIME: 4 UNSCII lines");
    size_t n = strlen(v.l[v.txt_i + 3].text);
    TEST_ASSERT(n && v.l[v.txt_i + 3].text[n - 1] == '~', "cut with ~");
}

/* "Bytes -> pixels, pinned end to end" (plan Task 5, kept through Task 8):
 * the chest's own raw register/DMA bytes, through chest_proto_parse ->
 * chest_view_build -> the memlcd model (memlcd_model_set_coffre) ->
 * memlcd_vue_coffre, asserting the RENDERED lines. */
static void test_vue_coffre_end_to_end(void)
{
    chest_status_t st;
    chest_view_t cv;
    memlcd_model_t m;
    memlcd_vue_coffre_t v;
    char txt[64];

    /* V16: RESET pending, label "12 COMPTES", 0x0F = 12. */
    TEST_ASSERT_EQ(chest_proto_parse(V16, 64, &st), CHEST_BLOCK_OK, "V16 parses");
    chest_view_build(&cv, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, NULL, 0);
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    memlcd_model_set_coffre(&m, &cv);
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_PROMPT, "V16: prompt");
    assert_vue_tient(&v, "V16 fits");
    TEST_ASSERT(strcmp(v.l[0].text, "RESET!") == 0, "V16: op 10 = RESET!");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "12 COMPTES") == 0, "V16: the label, whole");
    TEST_ASSERT(vue_cherche(&v, "12 CPT") > 0, "V16: 12 CPT from op_count, not clamped");

    /* V1: the chest's label GITHUB even with the cursor on OVH:PRO. */
    chest_list_t l;
    TEST_ASSERT(chest_list_decode(L1, sizeof L1, &l), "L1 decodes");
    chest_oath_t o;
    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    chest_oath_nav(&o, +2);
    TEST_ASSERT_EQ(chest_proto_parse(V1, 64, &st), CHEST_BLOCK_OK, "V1 parses");
    chest_view_build(&cv, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    memlcd_model_set_coffre(&m, &cv);
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_PROMPT, "V1: prompt");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "GITHUB") == 0, "V1: the CHEST's label, not the cursor's OVH:PRO");

    /* V9: nothing to show whatever the cursor. */
    TEST_ASSERT_EQ(chest_proto_parse(V9, 64, &st), CHEST_BLOCK_OK, "V9 parses");
    chest_view_build(&cv, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    memlcd_model_set_coffre(&m, &cv);
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_NONE, "V9: nothing");
    TEST_ASSERT_EQ(v.n, 0, "V9: no line");

    /* V15: browsing, TIME clear -> NO TIME (its own op cleared, as the transport would). */
    TEST_ASSERT_EQ(chest_proto_parse(V15, 64, &st), CHEST_BLOCK_OK, "V15 parses");
    chest_view_build(&cv, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    memlcd_model_set_coffre(&m, &cv);
    m.coffre_op = 0;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_BROWSE, "V15: browsing");
    TEST_ASSERT(cv.browsing && cv.pos == 2 && cv.total > 2, "V15: the cursor moved by 2 on L1's page");
    {
        char pos[16];
        snprintf(pos, sizeof pos, "3/%u", (unsigned)cv.total);
        TEST_ASSERT(strcmp(v.l[0].text, pos) == 0, "V15: 3/total, 1-based");
    }
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "OVH:PRO") == 0, "V15: the cursor's name, whole, across its lines");
    TEST_ASSERT(vue_cherche(&v, "NO TIME") >= 0, "V15: NO TIME, bit 3 clear");

    /* A code after on_code, gone after its deadline. */
    chest_code_t c;
    TEST_ASSERT(chest_code_decode(C1, sizeof C1, &c), "C1 decodes");
    chest_list_t page; memset(&page, 0, sizeof page);
    page.total = 1; page.count = 1; page.e[0].index = 5; strcpy(page.e[0].name, "WORK");
    chest_oath_t o2; chest_oath_reset(&o2);
    chest_oath_on_list(&o2, &page);
    chest_oath_code_requested(&o2, 5);
    chest_oath_on_code(&o2, &c, 1000);
    chest_view_build(&cv, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o2, 1000);
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    memlcd_model_set_coffre(&m, &cv);
    m.coffre_op = 0;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_VC_CODE, "code shown");
    TEST_ASSERT(vue_cherche(&v, "418") >= 0 && vue_cherche(&v, "902") >= 0 && vue_cherche(&v, "12 s") >= 0, "418 / 902 / 12 s");
    TEST_ASSERT_EQ(v.bar_pct, 40, "bar at 40 %");
    chest_view_build(&cv, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o2, 13000);
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    memlcd_model_set_coffre(&m, &cv);
    m.coffre_op = 0;
    memlcd_vue_coffre(&m, &v);
    TEST_ASSERT(v.kind != MEMLCD_VC_CODE, "the expired code is gone");
    for (uint8_t i = 0; i < v.n; i++) TEST_ASSERT(strstr(v.l[i].text, "418") == NULL, "no digit left on screen");
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
    /* TRRS link: the 5 V closing is displayed (bolt in the banner), so it
     * redraws. Without this, the handshake was invisible — the only witness
     * was the console, which you don't have while typing on battery. */
    b = a; b.lien_5v = 1;  TEST_ASSERT(memlcd_model_diff(&a, &b), "TRRS 5 V closes → redraw");
    b = a; b.caps_lock = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "Caps Lock → redraw");
    b = a; b.caps_word = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "Caps Word → redraw");
    b = a; b.osm = 0x02;   TEST_ASSERT(memlcd_model_diff(&a, &b), "one-shot mod armed → redraw");
    b = a; b.osl = 2;      TEST_ASSERT(memlcd_model_diff(&a, &b), "one-shot layer armed → redraw");
    b = a; b.veille = 1;   TEST_ASSERT(memlcd_model_diff(&a, &b), "going to sleep → redraw (zZ)");
    b = a; b.coffre = MEMLCD_COFFRE_PRESENT; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest appears → redraw");
    b = a; b.coffre_op = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest prompt → redraw");
    b = a; b.coffre_op_count = 2; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest op_count changes → redraw");
    b = a; strcpy(b.coffre_label, "GITHUB"); TEST_ASSERT(memlcd_model_diff(&a, &b), "chest label changes → redraw");
    b = a; b.coffre_mode_active = CHEST_MODE_PGP; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest active mode changes → redraw");
    b = a; b.coffre_mode_wanted = CHEST_MODE_OATH; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest wanted mode changes → redraw");
    b = a; b.coffre_mode_state = CHEST_MODE_FAULT; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest mode state changes → redraw");
    b = a; b.coffre_browsing = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest browsing starts → redraw");
    b = a; b.coffre_pos = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest cursor moves → redraw");
    b = a; b.coffre_total = 12; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest page total changes → redraw");
    b = a; strcpy(b.coffre_nom, "OVH:PRO"); TEST_ASSERT(memlcd_model_diff(&a, &b), "chest cursor name changes → redraw");
    b = a; b.coffre_code_visible = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest code appears → redraw");
    b = a; strcpy(b.coffre_code, "123456"); TEST_ASSERT(memlcd_model_diff(&a, &b), "chest code text changes → redraw");
    /* The countdown changes every second while a code is visible (at most
     * 30 s, chest_oath's own window bound): this makes the model diff
     * return true on most 1 s ticks during that window, same cadence the
     * status task/LVGL already poll at (STATUS_DISP_PERIODE_MS /
     * LVGL_REFR_MS, cadence.h, both 1000 ms on the memlcd halves) — no new,
     * faster periodic wait is introduced, only more REAL redraws within an
     * already-scheduled 1 s tick, so the tickless-sleep rule (no wait below
     * CADENCE_REPOS_MIN_MS) is unaffected. */
    b = a; b.coffre_code_secs = 5; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest code countdown changes → redraw");
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
    test_couche_affichee();
    test_ligne_etat();
    test_etat_coffre();
    test_text_width();
    test_couper_px();
    test_couper_px_bounded_scan_no_terminator();
    test_vue_prompt();
    test_vue_code();
    test_vue_browse();
    test_vue_coffre_end_to_end();
    test_model_diff();
    test_fb_to_panel();
    test_batt_affichee();
}
