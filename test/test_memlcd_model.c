/* Sharp memory-LCD screen of the halves — pure logic.
 *
 * Three things a bug would make visible on screen without ever crashing:
 *  - rev8: the panel reads LSB-first, the ESP32 transmits MSB-first. A wrong
 *    inversion = ignored commands, silent screen, no error anywhere.
 *  - the left screen's layout (memlcd_cave.h, "cave", 2026-09-30): every
 *    line inside the panel, nothing overlapping, the chest's label WHOLE,
 *    no name line that reads like a code, nothing under the 12 px floor.
 *  - the model diff: we only redraw if a displayed field has changed —
 *    every redraw is a transaction on the bus shared with the radio.
 * Spec: docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md */
#include "test_framework.h"
#include "../main/display/memlcd/memlcd_model.h"
#include "../main/display/memlcd/memlcd_cave.h"
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

/* ── The cave view (memlcd_cave.h) ────────────────────────────────────
 * The test's OWN fit check — written apart from memlcd_cave_tient on
 * purpose: a view is held to it whatever the layout believed. Every line
 * inside the content box and inside its label box (2 px of air), no two
 * lines' ink extents overlapping, every pictogram inside the box and clear
 * of every line; no font under the 12 px floor, except UNSCII 8 for the
 * label of a prompt (the last resort that keeps it whole). */
static int ink_x0(const memlcd_cave_ligne_t *l)
{
    int w = memlcd_text_width(l->font, l->text);
    return l->left ? l->x : l->x + (l->w - w) / 2;
}
static bool croise(int a0, int a1, int b0, int b1) { return a0 < b1 && b0 < a1; }
static void assert_vue_tient(const memlcd_cave_vue_t *v, const char *quoi)
{
    TEST_ASSERT(v->n <= MEMLCD_CAVE_LIGNES, quoi);
    struct { int x0, y0, x1, y1; } p[8];
    int np = 0;
    if (v->logo)    { p[np].x0 = v->logo_x; p[np].y0 = v->logo_y; p[np].x1 = v->logo_x + v->logo; p[np].y1 = v->logo_y + v->logo; np++; }
    if (v->cadenas) { p[np].x0 = v->cadenas_x; p[np].y0 = v->cadenas_y; p[np].x1 = v->cadenas_x + 24; p[np].y1 = v->cadenas_y + 28; np++; }
    if (v->goutte)  { p[np].x0 = v->goutte_x; p[np].y0 = v->goutte_y; p[np].x1 = v->goutte_x + 13; p[np].y1 = v->goutte_y + 16; np++; }
    if (v->lien)    { p[np].x0 = v->lien_x; p[np].y0 = v->lien_y; p[np].x1 = v->lien_x + 16; p[np].y1 = v->lien_y + 12; np++; }
    if (v->rule)    { p[np].x0 = 8; p[np].y0 = v->rule_y - 1; p[np].x1 = 61; p[np].y1 = v->rule_y + 2; np++; }
    if (v->eau)     { p[np].x0 = v->eau_x; p[np].y0 = v->eau_y; p[np].x1 = v->eau_x + 44; p[np].y1 = v->eau_y + 14 + 5; np++; }
    for (int i = 0; i < np; i++) {
        TEST_ASSERT(p[i].x0 >= 0 && p[i].x1 <= MEMLCD_W && p[i].y0 >= v->top && p[i].y1 <= v->bottom, quoi);
        for (int j = 0; j < i; j++)
            if (croise(p[i].x0, p[i].x1, p[j].x0, p[j].x1) && croise(p[i].y0, p[i].y1, p[j].y0, p[j].y1))
                TEST_ASSERT(0, "two pictograms overlap");
    }
    for (uint8_t i = 0; i < v->n; i++) {
        const memlcd_cave_ligne_t *l = &v->l[i];
        int w = memlcd_text_width(l->font, l->text), h = memlcd_font_pas(l->font);
        bool label = v->kind == MEMLCD_CV_PROMPT && i >= v->txt_i && i < v->txt_i + v->txt_n;
        TEST_ASSERT(l->font <= MEMLCD_F_M32 || (l->font == MEMLCD_F_U8 && label), "no font under the 12 px floor");
        TEST_ASSERT(w + 2 <= l->w && l->x + l->w <= MEMLCD_W, quoi);
        TEST_ASSERT(l->y >= v->top && l->y + h <= v->bottom, quoi);
        int x0 = ink_x0(l);
        for (uint8_t j = 0; j < i; j++) {
            const memlcd_cave_ligne_t *k = &v->l[j];
            int kx0 = ink_x0(k), kw = memlcd_text_width(k->font, k->text);
            if (w && kw && croise(x0, x0 + w, kx0, kx0 + kw) && croise(l->y, l->y + h, k->y, k->y + memlcd_font_pas(k->font)))
                TEST_ASSERT(0, "two lines overlap");
        }
        for (int j = 0; j < np; j++)
            if (w && croise(x0, x0 + w, p[j].x0, p[j].x1) && croise(l->y, l->y + h, p[j].y0, p[j].y1))
                TEST_ASSERT(0, "a line overlaps a pictogram");
    }
    /* A continuation mark (prompt label): left-aligned text after it, the
     * mark inside the box, clear of every line's ink and every pictogram. */
    for (uint8_t i = 0; i < v->n; i++) {
        const memlcd_cave_ligne_t *l = &v->l[i];
        if (!l->marque) continue;
        TEST_ASSERT(l->left, "a marked line is left-aligned after its mark");
        int mx0 = l->x - MEMLCD_CAVE_MARQUE_PAS, my0 = l->y + memlcd_cave_marque_dy(l->font);
        int mx1 = mx0 + MEMLCD_CAVE_MARQUE_W, my1 = my0 + MEMLCD_CAVE_MARQUE_H;
        TEST_ASSERT(mx0 >= 0 && my0 >= v->top && my1 <= v->bottom, "the mark inside the content box");
        for (uint8_t j = 0; j < v->n; j++) {
            const memlcd_cave_ligne_t *k = &v->l[j];
            int kx0 = ink_x0(k), kw = memlcd_text_width(k->font, k->text);
            if (kw && croise(mx0, mx1, kx0, kx0 + kw) && croise(my0, my1, k->y, k->y + memlcd_font_pas(k->font)))
                TEST_ASSERT(0, "a mark overlaps a line");
        }
        for (int j = 0; j < np; j++)
            if (croise(mx0, mx1, p[j].x0, p[j].x1) && croise(my0, my1, p[j].y0, p[j].y1))
                TEST_ASSERT(0, "a mark overlaps a pictogram");
    }
}

/* Concatenation of the label/name lines — what the eye reads across them. */
static void vue_texte(const memlcd_cave_vue_t *v, char *out, size_t n)
{
    out[0] = '\0';
    for (uint8_t i = v->txt_i; i < v->txt_i + v->txt_n && i < v->n; i++)
        strncat(out, v->l[i].text, n - strlen(out) - 1);
}
static int vue_cherche(const memlcd_cave_vue_t *v, const char *s)
{
    for (uint8_t i = 0; i < v->n; i++) if (strcmp(v->l[i].text, s) == 0) return i;
    return -1;
}
static bool vue_contient(const memlcd_cave_vue_t *v, const char *s)
{
    for (uint8_t i = 0; i < v->n; i++) if (strstr(v->l[i].text, s)) return true;
    return false;
}

/* The layer name is the normal screen's hero (plan 2026-09-30, cave): the
 * WIDEST Montserrat of the ladder (32 down to the 12 px floor) that holds
 * the whole name on one line; past the floor, two BALANCED lines
 * ("NAVIG" / "ATION", never "NAVIGATIO" / "N"), never a font under 12 px.
 * (2 lines of 6 UNSCII-width characters with "…" until 2026-09-30.) */
static void test_nom_couche(void)
{
    memlcd_cave_vue_t v;
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE, .batt_local_dv = 40, .is_left = 1 };
    strcpy(m.nom, "BASE");
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_NORMAL, "no chest view: the normal screen");
    assert_vue_tient(&v, "BASE fits");
    int i = vue_cherche(&v, "BASE");
    TEST_ASSERT(i >= 0, "BASE on screen, whole, on one line");
    /* the widest that fits 66 px: M24 is 67 px (kerned), M20 55 */
    TEST_ASSERT(i >= 0 && v.l[i].font == MEMLCD_F_M20, "BASE in Montserrat 20, the widest that fits");

    strcpy(m.nom, "NAVIGATION");
    memlcd_cave_vue(&m, 80, &v);
    assert_vue_tient(&v, "NAVIGATION fits");
    int a = vue_cherche(&v, "NAVIG"), b = vue_cherche(&v, "ATION");
    TEST_ASSERT(a >= 0 && b == a + 1, "NAVIGATION: NAVIG / ATION, balanced");
    TEST_ASSERT(a >= 0 && v.l[a].font == MEMLCD_F_M12 && v.l[b].font == MEMLCD_F_M12, "... at the 12 px floor, never under");
    TEST_ASSERT(vue_cherche(&v, "N") < 0, "never a stranded letter");

    strcpy(m.nom, "NAV");
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(vue_cherche(&v, "NAV") >= 0, "a short name, whole");
    m.nom[0] = '\0';
    memlcd_cave_vue(&m, 80, &v);
    assert_vue_tient(&v, "empty name: still a valid screen");
    strcpy(m.nom, "WWWWWWWWWWWWWWW");                 /* 15, the buffer's max */
    memlcd_cave_vue(&m, 80, &v);
    assert_vue_tient(&v, "15 W: every line fits");
    char t[64]; t[0] = '\0';
    for (uint8_t k = 0; k < v.n; k++) if (v.l[k].text[0] == 'W') strcat(t, v.l[k].text);
    TEST_ASSERT(strcmp(t, "WWWWWWWWWWWWWWW") == 0, "15 W: the whole name across its lines");
}

/* The chest's status on the normal screen (plan Task 8, "SD c'est
 * confusant", Mae 2026-09-29; cave layout 2026-09-30): a PADLOCK = chest
 * present, beside the corner logo; under the battery row ".." while not
 * READY, "?" on a protocol version mismatch, the mode in plain words
 * (chest_mode_label: DISK/PGP/OTP/FIDO/TOTP, lower case in flight, ERR),
 * "NO CARD" only when the SD card is MISSING. Nothing without a chest. */
static void test_etat_coffre(void)
{
    memlcd_cave_vue_t v;
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE, .batt_local_dv = 40, .nom = "BASE" };
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(!v.cadenas && !vue_contient(&v, "..") && !vue_contient(&v, "CARD"), "absent: nothing at all");

    m.coffre = MEMLCD_COFFRE_PRESENT;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(v.cadenas, "present, booting: the padlock");
    TEST_ASSERT(vue_cherche(&v, "..") >= 0 && !vue_contient(&v, "CARD"), "booting: '..' and no NO CARD before READY");
    assert_vue_tient(&v, "booting fits");

    m.coffre = MEMLCD_COFFRE_PRESENT | MEMLCD_COFFRE_BADVER;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(v.cadenas && vue_cherche(&v, "?") >= 0, "unknown protocol version: padlock + ?");

    /* Ready, card in, PGP mounted: the padlock and the mode, no card line. */
    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_USB;
    m.coffre_mode_state = CHEST_MODE_ARRIVED; m.coffre_mode_active = CHEST_MODE_PGP; m.coffre_mode_wanted = CHEST_MODE_PGP;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(v.cadenas && vue_cherche(&v, "PGP") >= 0, "ready: padlock + PGP");
    TEST_ASSERT(!vue_contient(&v, "CARD") && !vue_contient(&v, "SD"), "card present: nothing about the card");

    /* Card missing: NO CARD under the mode. */
    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY;
    m.coffre_mode_state = CHEST_MODE_PENDING; m.coffre_mode_active = CHEST_MODE_IN_FLIGHT; m.coffre_mode_wanted = CHEST_MODE_OATH;
    memlcd_cave_vue(&m, 80, &v);
    int a = vue_cherche(&v, "totp"), b = vue_cherche(&v, "NO CARD");
    TEST_ASSERT(a >= 0, "pending: the wanted mode, lower case, in plain words");
    TEST_ASSERT(b >= 0 && v.l[b].y > v.l[a].y, "card missing: NO CARD under the mode");
    assert_vue_tient(&v, "mode + NO CARD fits");

    m.coffre_mode_state = CHEST_MODE_ARRIVED; m.coffre_mode_active = CHEST_MODE_NONE; m.coffre_mode_wanted = CHEST_MODE_NONE;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(vue_cherche(&v, "NO CARD") >= 0, "no mode: NO CARD alone");

    m.coffre_mode_state = CHEST_MODE_FAULT;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(vue_cherche(&v, "ERR") >= 0, "persisting refusal: ERR");

    /* Every mode word fits, with every flag armed and the dongle seen. */
    static const uint8_t modes[] = { CHEST_MODE_STORAGE, CHEST_MODE_PGP, CHEST_MODE_OTP, CHEST_MODE_FIDO, CHEST_MODE_OATH };
    m.caps_lock = 1; m.caps_word = 1; m.osm = 0xFF; m.osl = 3; m.dongle_vu = 1; m.lien_5v = 1;
    strcpy(m.nom, "NAVIGATION");
    for (size_t i = 0; i < sizeof modes; i++) {
        m.coffre_mode_state = CHEST_MODE_ARRIVED; m.coffre_mode_active = modes[i]; m.coffre_mode_wanted = modes[i];
        memlcd_cave_vue(&m, 80, &v);
        assert_vue_tient(&v, "every mode + NO CARD + every flag fits");
        TEST_ASSERT(vue_cherche(&v, "NO CARD") >= 0 && vue_contient(&v, "CAPS") && vue_contient(&v, "L3"), "nothing dropped");
    }
    TEST_ASSERT(MEMLCD_CAVE_CADENAS_W >= 24 && MEMLCD_CAVE_CADENAS_H >= 28, "a BIG padlock (Mae: 24 x 28 or more)");
}

/* The top status of the normal screen: route in words ("USB" / "RADIO"),
 * "SEEN" on its own line when the dongle acks, the water drop with the
 * percentage beside it (not volts), "+" charging, "FULL" charged, "?" when
 * unknown, the TRRS link pictogram in place of the charge marker, the drop
 * outlined 2 px when the battery is LOW. */
static void test_statut_haut(void)
{
    memlcd_cave_vue_t v;
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE, .batt_local_dv = 40, .nom = "BASE" };
    memlcd_cave_vue(&m, 87, &v);
    TEST_ASSERT(vue_cherche(&v, "USB") >= 0 && vue_cherche(&v, "RADIO") < 0 && vue_cherche(&v, "SEEN") < 0, "USB, no SEEN");
    TEST_ASSERT(v.logo == MEMLCD_CAVE_LOGO_S, "the 28 px corner logo");
    int p = vue_cherche(&v, "87%");
    TEST_ASSERT(p >= 0 && v.goutte && v.goutte_pct == 87 && !v.goutte_low, "the drop at 87 %, the percentage beside it");
    TEST_ASSERT(p >= 0 && v.l[p].left && v.l[p].x >= v.goutte_x + MEMLCD_CAVE_GOUTTE_W, "... to the right of the drop");
    TEST_ASSERT(!vue_contient(&v, "V"), "no volts anywhere");
    assert_vue_tient(&v, "USB top fits");

    m.route_rf = 1; m.dongle_vu = 1; m.batt_niveau = 1;
    memlcd_cave_vue(&m, 12, &v);
    int r = vue_cherche(&v, "RADIO"), s = vue_cherche(&v, "SEEN");
    TEST_ASSERT(r >= 0 && s >= 0 && v.l[s].y > v.l[r].y, "RADIO, then SEEN on its own line");
    TEST_ASSERT(v.goutte_low && v.goutte_pct == 12, "LOW: the thick outline");

    m.batt_local_chg = 1;
    memlcd_cave_vue(&m, 50, &v);
    TEST_ASSERT(vue_cherche(&v, "50%+") >= 0, "charging: +");
    m.batt_local_chg = 2;
    memlcd_cave_vue(&m, 100, &v);
    TEST_ASSERT(vue_cherche(&v, "FULL") >= 0, "charged: FULL, a word");
    m.lien_5v = 1;
    memlcd_cave_vue(&m, 100, &v);
    TEST_ASSERT(v.lien && vue_cherche(&v, "100%") >= 0, "TRRS 5 V: the link pictogram in place of the charge marker");
    assert_vue_tient(&v, "100% + link fits");
    m.lien_5v = 0; m.batt_local_dv = 0xFF;
    memlcd_cave_vue(&m, 100, &v);
    TEST_ASSERT(vue_cherche(&v, "?") >= 0 && v.goutte_pct == 0, "unknown voltage: ?, an empty drop, never 0 V");
    m.batt_local_dv = 40;
    memlcd_cave_vue(&m, 0xFF, &v);
    TEST_ASSERT(vue_cherche(&v, "?") >= 0 && v.goutte_pct == 0 && vue_cherche(&v, "255%") < 0,
                "unknown DISPLAYED percentage (batt_sense_pct 0xFF): ?, never 255%");
}

/* 34 characters, CHEST_LABEL_MAX — the alphabet (26) plus "ABCDEFGH" (8). */
#define LABEL34 "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGH"
/* A realistic 34-character label: lower case, digits, punctuation. */
#define LABEL34N "github.com:alice.martin@work-2fa01"
/* 34 of the widest upper-case letter. */
#define LABEL34W "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"

/* The width oracle: the generated LVGL advances WITH the class kerning,
 * rounded glyph by glyph; UNSCII 8 px a character. Values checked against
 * lv_font_montserrat_*.c (adv_w 252 for 'W' in M14 -> 16 px) and, for every
 * printable pair, against lv_txt_get_width itself (tools/memlcd_sim's
 * check_widths gate). */
static void test_text_width(void)
{
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_U8, "ABCDEFGH"), 64, "UNSCII: 8 px a character");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M14, "W"), 16, "M14 W: adv 252/16 rounded");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M14, "I"), 4, "M14 I: adv 69/16 rounded");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M24, "TOTP"), 65, "M24 TOTP");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M28, "418"), 46, "M28 418 (4 and 1 kern)");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M24, "BASE"), 67, "M24 BASE: kerning makes it one pixel too wide for 66");
    TEST_ASSERT(memlcd_text_width(MEMLCD_F_M24, "AV") < 2 * memlcd_text_width(MEMLCD_F_M24, "A") + 1
                && memlcd_text_width(MEMLCD_F_M24, "AV") < memlcd_text_width(MEMLCD_F_M24, "A") + memlcd_text_width(MEMLCD_F_M24, "V"),
                "AV kerns tighter than A + V");
    TEST_ASSERT_EQ(memlcd_text_width_n(MEMLCD_F_M24, "AVX", 1), memlcd_text_width(MEMLCD_F_M24, "A"),
                   "a prefix is measured as a line of its own: no partner past its end");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M14, ""), 0, "empty");
    TEST_ASSERT_EQ(memlcd_text_width(MEMLCD_F_M14, NULL), 0, "NULL");
    TEST_ASSERT_EQ(memlcd_font_pas(MEMLCD_F_U8), 11, "UNSCII pitch");
    TEST_ASSERT_EQ(memlcd_font_pas(MEMLCD_F_M12), 15, "M12 line");
    TEST_ASSERT_EQ(memlcd_font_pas(MEMLCD_F_M14), 16, "M14 line");
    TEST_ASSERT_EQ(memlcd_font_pas(MEMLCD_F_M24), 27, "M24 line");
    TEST_ASSERT_EQ(memlcd_font_pas(MEMLCD_F_M32), 35, "M32 line");
    /* The widest printable character at the floor fits a label line on its
     * own: otherwise the wrap could not make progress. */
    for (int c = 0x20; c <= 0x7E; c++) {
        char s[2] = { (char)c, 0 };
        TEST_ASSERT(memlcd_text_width(MEMLCD_F_M12, s) <= MEMLCD_CAVE_LABEL_W - 2, "one M12 glyph fits a label line");
    }
}

/* The prompt takes the WHOLE screen: the op title (at most 20 px), a wavy
 * divider, the CHEST's label WHOLE — the op shrinks first, UNSCII 8 last,
 * never a cut — "N ACCTS" when more than one account, "PRESS" at the bottom. */
static void test_vue_prompt(void)
{
    memlcd_cave_vue_t v;
    char txt[64];
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE, .nom = "BASE", .batt_local_dv = 40 };
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_NORMAL, "nothing pending, no code, not browsing: the normal screen");

    m.coffre_op = 7; strcpy(m.coffre_label, "GITHUB"); m.coffre_op_count = 1;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_PROMPT, "prompt");
    assert_vue_tient(&v, "TOTP/GITHUB fits");
    TEST_ASSERT(strcmp(v.l[0].text, "TOTP") == 0 && v.l[0].font == MEMLCD_F_M20, "the op alone, at its 20 px ceiling");
    TEST_ASSERT(v.rule && v.rule_y > v.l[0].y && v.rule_y < v.l[v.txt_i].y, "a divider between the op and the label");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "GITHUB") == 0 && v.l[v.txt_i].font == MEMLCD_F_M14, "the label, one line in M14");
    TEST_ASSERT(strcmp(v.l[v.n - 1].text, "PRESS") == 0, "last line PRESS");
    TEST_ASSERT(!v.logo && !v.goutte && !vue_contient(&v, "USB") && !vue_contient(&v, "BASE"), "full screen: nothing of the normal screen");
    TEST_ASSERT(!vue_contient(&v, "ACCTS"), "op_count 1: no N ACCTS");

    /* RESET! is 70 px at 20: the op falls to 18 on its own. */
    m.coffre_op = 10;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(strcmp(v.l[0].text, "RESET!") == 0 && v.l[0].font == MEMLCD_F_M18, "RESET! in M18");
    static const uint16_t ops[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 42, 99, 100, 1042 };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        m.coffre_op = ops[i];
        memlcd_cave_vue(&m, 80, &v);
        assert_vue_tient(&v, "every op label fits");
    }

    /* The 34-character realistic label: whole, safe-wrapped at the 12 px floor. */
    m.coffre_op = 7; m.coffre_op_count = 1; strcpy(m.coffre_label, LABEL34N);
    memlcd_cave_vue(&m, 80, &v);
    assert_vue_tient(&v, "34-char normal label fits");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, LABEL34N) == 0, "34 normal characters: the whole label");
    TEST_ASSERT(v.l[v.txt_i].font == MEMLCD_F_M12, "... at the 12 px floor, never under");

    /* 34 W: 9 lines at 12 px cannot fit — UNSCII 8, whole, no ~. */
    strcpy(m.coffre_label, LABEL34W);
    memlcd_cave_vue(&m, 80, &v);
    assert_vue_tient(&v, "34 W fits");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, LABEL34W) == 0, "34 W: the whole label, never cut");
    TEST_ASSERT(v.l[v.txt_i].font == MEMLCD_F_U8 && v.txt_n == 5, "... in UNSCII 8, 5 lines: the last resort");
    for (uint8_t i = v.txt_i; i < v.txt_i + v.txt_n; i++)
        TEST_ASSERT(!!v.l[i].marque == (i > v.txt_i), "... every line after the first carries the continuation mark");
    TEST_ASSERT(strchr(txt, '~') == NULL, "never a ~ on a prompt");

    /* The same with N ACCTS: still whole, N ACCTS and PRESS still there. */
    m.coffre_op = 10; m.coffre_op_count = 12;
    memlcd_cave_vue(&m, 80, &v);
    assert_vue_tient(&v, "34 W + 12 ACCTS fits");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, LABEL34W) == 0, "34 W with N ACCTS: whole");
    TEST_ASSERT(vue_cherche(&v, "12 ACCTS") > 0 && strcmp(v.l[v.n - 1].text, "PRESS") == 0, "12 ACCTS then PRESS");
    strcpy(m.coffre_label, LABEL34);
    memlcd_cave_vue(&m, 80, &v);
    assert_vue_tient(&v, "alphabet34 + 12 ACCTS fits");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, LABEL34) == 0, "the alphabet label with N ACCTS: whole");

    /* Two 34-char labels differing only at the last character render differently (C1). */
    {
        memlcd_cave_vue_t a, b;
        char la[35], lb[35];
        strcpy(la, LABEL34); la[33] = '1';
        strcpy(lb, LABEL34); lb[33] = '2';
        strcpy(m.coffre_label, la); memlcd_cave_vue(&m, 80, &a);
        strcpy(m.coffre_label, lb); memlcd_cave_vue(&m, 80, &b);
        char ta[64], tb[64];
        vue_texte(&a, ta, sizeof ta); vue_texte(&b, tb, sizeof tb);
        TEST_ASSERT(strcmp(ta, la) == 0 && strcmp(tb, lb) == 0, "C1: both whole, so they differ");
    }

    /* op_count boundary: 2 is already more than one. */
    strcpy(m.coffre_label, "GITHUB"); m.coffre_op_count = 2;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(vue_cherche(&v, "2 ACCTS") > 0, "op_count 2: N ACCTS");
    m.coffre_op_count = 0;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(!vue_contient(&v, "ACCTS"), "op_count 0: no ACCTS");

    /* The prompt shows coffre_label, NEVER coffre_nom (the browser's copy). */
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    m.coffre_op = 7; strcpy(m.coffre_label, "GITHUB"); m.coffre_op_count = 1;
    strcpy(m.coffre_nom, "OVH:PRO"); m.coffre_browsing = 1; m.coffre_total = 3;
    memlcd_cave_vue(&m, 80, &v);
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "GITHUB") == 0, "the chest's label, not the cursor's");
    TEST_ASSERT(!vue_contient(&v, "OVH"), "the browser's name nowhere on the prompt");

    /* Priority: prompt > code. The code's digits never leak onto a prompt. */
    m.coffre_code_visible = 1; strcpy(m.coffre_code, "418902"); m.coffre_code_secs = 12;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_PROMPT, "prompt beats a visible code");
    TEST_ASSERT(!vue_contient(&v, "418") && !vue_contient(&v, "902") && !v.eau, "no digit, no countdown of the code on the prompt");

    /* The bench incident's labels, on a prompt: no line of digits without a letter. */
    strcpy(m.coffre_label, "TEST:RFC6238");
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(vue_cherche(&v, "TEST:") == (int)v.txt_i && vue_cherche(&v, "RFC6238") == (int)v.txt_i + 1, "TEST: / RFC6238");
    strcpy(m.coffre_label, "BANQUE:4021");
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(vue_cherche(&v, "BANQU") == (int)v.txt_i && vue_cherche(&v, "E:4021") == (int)v.txt_i + 1, "BANQU / E:4021");
}

/* The code takes the WHOLE screen: the name (<= 2 lines, ~ if cut — the
 * browser's copy, not a security text), the code as two halves in the
 * widest font that fits (3 + 3 in M32, 4 + 4 in M24), the countdown as a
 * water bar, "NN s" — only while coffre_code_visible. */
static void test_vue_code(void)
{
    memlcd_cave_vue_t v;
    char txt[64];
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE, .nom = "BASE" };
    m.coffre_code_visible = 1; strcpy(m.coffre_nom, "WORK"); strcpy(m.coffre_code, "418902"); m.coffre_code_secs = 12;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_CODE, "code");
    assert_vue_tient(&v, "6-digit code fits");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "WORK") == 0 && v.l[v.txt_i].font == MEMLCD_F_M14, "the name in M14");
    int a = vue_cherche(&v, "418"), b = vue_cherche(&v, "902");
    TEST_ASSERT(a >= 0 && b == a + 1, "6 digits: 418 then 902");
    TEST_ASSERT(a >= 0 && v.l[a].font == MEMLCD_F_M32 && v.l[b].font == MEMLCD_F_M32, "... in M32: bigger than any name");
    int s = vue_cherche(&v, "12 s");
    TEST_ASSERT(s >= 0 && v.l[s].font == MEMLCD_F_M14, "12 s");
    TEST_ASSERT_EQ(v.eau_pct, 40, "12 s of 30: the water at 40 %");
    TEST_ASSERT(v.eau && v.eau_y > v.l[b].y && v.eau_y + MEMLCD_CAVE_EAU_H <= v.l[s].y, "the water between the code and the seconds");

    for (char d = '0'; d <= '9'; d++) {
        char c6[7], c8[9];
        memset(c6, d, 6); c6[6] = 0; memset(c8, d, 8); c8[8] = 0;
        strcpy(m.coffre_code, c6); memlcd_cave_vue(&m, 80, &v); assert_vue_tient(&v, "6 x digit fits");
        strcpy(m.coffre_code, c8); memlcd_cave_vue(&m, 80, &v); assert_vue_tient(&v, "8 x digit fits");
    }

    strcpy(m.coffre_code, "12345678"); m.coffre_code_secs = 30;
    memlcd_cave_vue(&m, 80, &v);
    a = vue_cherche(&v, "1234"); b = vue_cherche(&v, "5678");
    TEST_ASSERT(a >= 0 && b == a + 1 && v.l[a].font == MEMLCD_F_M24 && v.l[b].font == MEMLCD_F_M24, "8 digits: two M24 lines of 4");
    TEST_ASSERT_EQ(v.eau_pct, 100, "30 s: full");
    TEST_ASSERT(vue_cherche(&v, "30 s") >= 0, "30 s");
    m.coffre_code_secs = 45;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.eau_pct, 100, "never above 100 %");

    /* A long name: 2 lines, cut with ~, and a code still whole below it. */
    strcpy(m.coffre_nom, LABEL34); strcpy(m.coffre_code, "418902");
    memlcd_cave_vue(&m, 80, &v);
    assert_vue_tient(&v, "long name fits");
    TEST_ASSERT(v.txt_n == 2, "the name on 2 lines");
    size_t n = strlen(v.l[v.txt_i + 1].text);
    TEST_ASSERT(n && v.l[v.txt_i + 1].text[n - 1] == '~', "cut with ~");
    TEST_ASSERT(vue_cherche(&v, "418") >= 0 && vue_cherche(&v, "902") >= 0, "the code whole under a long name");

    /* No code without coffre_code_visible, whatever coffre_code holds. */
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    strcpy(m.coffre_code, "123456"); m.coffre_code_secs = 12; strcpy(m.coffre_nom, "WORK");
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(v.kind == MEMLCD_CV_NORMAL && !vue_contient(&v, "123") && !v.eau, "code_visible false: never the code");
    m.coffre_browsing = 1; m.coffre_total = 1; m.coffre = CHEST_STATE_TIME;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_BROWSE, "browsing, code not visible: the browser");
    TEST_ASSERT(!vue_contient(&v, "123") && !v.eau, "no digit of an invisible code");

    m.coffre_code_visible = 1;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_CODE, "code beats the browser");
}

/* The browser keeps the top status (logo, route, drop) and the layer name
 * (capped at 16 px), then "i/N", the name — whole when it fits, else cut
 * with a ~ — and NO TIME when TIME_VALID is clear. */
static void test_vue_browse(void)
{
    memlcd_cave_vue_t v;
    char txt[64];
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE, .nom = "BASE", .batt_local_dv = 40 };
    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_TIME;
    m.coffre_browsing = 1; m.coffre_pos = 2; m.coffre_total = 12; strcpy(m.coffre_nom, "OVH:PRO");
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_BROWSE, "browse");
    assert_vue_tient(&v, "browse fits");
    TEST_ASSERT(vue_cherche(&v, "3/12") >= 0, "3/12, 1-based");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "OVH:PRO") == 0, "the name, whole");
    TEST_ASSERT(vue_cherche(&v, "USB") >= 0 && v.goutte, "the top status stays");
    int h = vue_cherche(&v, "BASE");
    TEST_ASSERT(h >= 0 && v.l[h].font <= MEMLCD_F_M16, "the layer name, capped at 16 px");
    TEST_ASSERT(!vue_contient(&v, "NO TIME"), "time set: no NO TIME");

    m.coffre &= (uint8_t)~CHEST_STATE_TIME;
    memlcd_cave_vue(&m, 80, &v);
    assert_vue_tient(&v, "browse + NO TIME fits");
    TEST_ASSERT(vue_cherche(&v, "NO TIME") >= 0, "TIME clear: NO TIME");

    /* A name too long for the room: cut with ~, still inside, NO TIME kept. */
    strcpy(m.coffre_nom, LABEL34);
    memlcd_cave_vue(&m, 80, &v);
    assert_vue_tient(&v, "long name + NO TIME fits");
    TEST_ASSERT(vue_cherche(&v, "NO TIME") >= 0, "NO TIME still shown");
    const char *last = v.l[v.txt_i + v.txt_n - 1].text;
    TEST_ASSERT(v.txt_n >= 1 && last[strlen(last) - 1] == '~', "cut with ~");
    TEST_ASSERT(v.l[v.txt_i].font >= MEMLCD_F_M12 && v.l[v.txt_i].font <= MEMLCD_F_M32, "never under the floor");
}

/* "Bytes -> pixels, pinned end to end" (plan Task 5, kept through Task 8
 * and the cave): the chest's own raw register/DMA bytes, through
 * chest_proto_parse -> chest_view_build -> the memlcd model
 * (memlcd_model_set_coffre) -> memlcd_cave_vue, asserting the drawn lines. */
static void test_vue_coffre_end_to_end(void)
{
    chest_status_t st;
    chest_view_t cv;
    memlcd_model_t m;
    memlcd_cave_vue_t v;
    char txt[64];

    /* V16: RESET pending, label "12 COMPTES", 0x0F = 12. */
    TEST_ASSERT_EQ(chest_proto_parse(V16, 64, &st), CHEST_BLOCK_OK, "V16 parses");
    chest_view_build(&cv, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, NULL, 0);
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    memlcd_model_set_coffre(&m, &cv);
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_PROMPT, "V16: prompt");
    assert_vue_tient(&v, "V16 fits");
    TEST_ASSERT(strcmp(v.l[0].text, "RESET!") == 0, "V16: op 10 = RESET!");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "12 COMPTES") == 0, "V16: the label, whole");
    TEST_ASSERT(vue_cherche(&v, "12 ACCTS") > 0, "V16: 12 ACCTS from op_count, not clamped");

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
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_PROMPT, "V1: prompt");
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "GITHUB") == 0, "V1: the CHEST's label, not the cursor's OVH:PRO");

    /* V9: nothing to show whatever the cursor: the normal screen. */
    TEST_ASSERT_EQ(chest_proto_parse(V9, 64, &st), CHEST_BLOCK_OK, "V9 parses");
    chest_view_build(&cv, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    memlcd_model_set_coffre(&m, &cv);
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(v.kind == MEMLCD_CV_NORMAL || v.kind == MEMLCD_CV_BROWSE, "V9: no prompt, no code");
    TEST_ASSERT(!v.eau && !v.rule, "V9: no countdown, no divider");

    /* V15: browsing, TIME clear -> NO TIME (its own op cleared, as the transport would). */
    TEST_ASSERT_EQ(chest_proto_parse(V15, 64, &st), CHEST_BLOCK_OK, "V15 parses");
    chest_view_build(&cv, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    memlcd_model_set_coffre(&m, &cv);
    m.coffre_op = 0;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_BROWSE, "V15: browsing");
    TEST_ASSERT(cv.browsing && cv.pos == 2 && cv.total > 2, "V15: the cursor moved by 2 on L1's page");
    {
        char pos[16];
        snprintf(pos, sizeof pos, "3/%u", (unsigned)cv.total);
        TEST_ASSERT(vue_cherche(&v, pos) >= 0, "V15: 3/total, 1-based");
    }
    vue_texte(&v, txt, sizeof txt);
    TEST_ASSERT(strcmp(txt, "OVH:PRO") == 0, "V15: the cursor's name, whole");
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
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_CODE, "code shown");
    TEST_ASSERT(vue_cherche(&v, "418") >= 0 && vue_cherche(&v, "902") >= 0 && vue_cherche(&v, "12 s") >= 0, "418 / 902 / 12 s");
    TEST_ASSERT_EQ(v.eau_pct, 40, "water at 40 %");
    chest_view_build(&cv, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o2, 13000);
    memset(&m, 0, sizeof m); m.osl = MEMLCD_OSL_AUCUNE;
    memlcd_model_set_coffre(&m, &cv);
    m.coffre_op = 0;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT(v.kind != MEMLCD_CV_CODE, "the expired code is gone");
    TEST_ASSERT(!vue_contient(&v, "418") && !v.eau, "no digit left on screen");
}

/* Sleep: the frozen image says it sleeps — the large logo and zZ, nothing
 * that could pretend to be live (route, gauge, chest). */
static void test_vue_veille(void)
{
    memlcd_cave_vue_t v;
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE, .nom = "BASE", .batt_local_dv = 40, .route_rf = 1, .dongle_vu = 1, .lien_5v = 1 };
    m.coffre = MEMLCD_COFFRE_PRESENT; m.coffre_op = 7; strcpy(m.coffre_label, "GITHUB");
    m.veille = 1;
    memlcd_cave_vue(&m, 80, &v);
    TEST_ASSERT_EQ(v.kind, MEMLCD_CV_SLEEP, "asleep");
    TEST_ASSERT(v.logo == MEMLCD_CAVE_LOGO_L, "the large logo");
    TEST_ASSERT(vue_cherche(&v, "zZ") >= 0, "zZ");
    TEST_ASSERT(!v.goutte && !v.lien && !v.cadenas && !vue_contient(&v, "RADIO") && !vue_contient(&v, "GITHUB"), "nothing live on a frozen image");
    assert_vue_tient(&v, "sleep fits");
}

static void test_model_diff(void)
{
    memlcd_model_t a = { .route_rf = 1, .dongle_vu = 1, .batt_local_dv = 40,
                         .couche = 1, .nom = "DVORAK", .is_left = 1 };
    memlcd_model_t b = a;
    TEST_ASSERT(!memlcd_model_diff(&a, &b), "identical → no redraw");
    b.batt_local_dv = 39;  TEST_ASSERT(memlcd_model_diff(&a, &b), "local voltage changes → redraw");
    b = a; b.batt_local_chg = 2; TEST_ASSERT(memlcd_model_diff(&a, &b), "charge state changes → redraw");
    b = a; b.batt_pct = 85; TEST_ASSERT(memlcd_model_diff(&a, &b), "displayed percentage changes → redraw (the voltage alone no longer carries it)");
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
    test_couche_affichee();
    test_ligne_etat();
    test_text_width();
    test_nom_couche();
    test_etat_coffre();
    test_statut_haut();
    test_vue_prompt();
    test_vue_code();
    test_vue_browse();
    test_vue_veille();
    test_vue_coffre_end_to_end();
    test_model_diff();
    test_fb_to_panel();
    test_batt_affichee();
}
