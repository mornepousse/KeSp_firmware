/* The left screen's text rules (plan 2026-09-30-left-screen-cave, spec
 * docs/superpowers/specs/2026-09-29-left-screen-redesign.md) — pure logic.
 *
 *  - safe wrap (memlcd_safe_wrap.h): a name broken over lines never leaves
 *    a line with digits and no letter — bench incident 2026-09-29, Mae read
 *    "TEST:RFC6238" pixel-cut to "TEST:RFC" / "6238" as a truncated code;
 *  - the water-drop gauge (memlcd_cave.h): percent -> wet rows, outline 2 px
 *    when LOW;
 *  - the layout budget (memlcd_cave.h): the 34-character label whole inside
 *    the prompt's band at >= 12 px, and no name line that reads like a code
 *    on any view, across a fuzz of issuer:account strings. */
#include "test_framework.h"
#include "../main/display/memlcd/memlcd_model.h"
#include "../main/display/memlcd/memlcd_safe_wrap.h"
#include "../main/display/memlcd/memlcd_cave.h"
#include <string.h>
#include <stdlib.h>

#define BUDGET (MEMLCD_CAVE_LABEL_W - 2)   /* 62 px: a label line */

static int wrap(const char *s, char out[][MEMLCD_SW_LINE_BUF])
{
    return memlcd_safe_wrap(s, MEMLCD_F_M12, BUDGET, out, MEMLCD_SW_MAX_LINES);
}

/* Characters of s other than spaces, in order — what a reader gets back
 * across the lines (a space break's leading space is trimmed for display). */
static void sans_espaces(const char *s, char *out)
{
    size_t k = 0;
    for (; *s; s++) if (*s != ' ') out[k++] = *s;
    out[k] = '\0';
}

static bool a_lettre(const char *s) { for (; *s; s++) if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) return true; return false; }
static bool a_chiffre(const char *s) { for (; *s; s++) if (*s >= '0' && *s <= '9') return true; return false; }
/* The test's own oracle for "reads like a code" (not the unit's): a digit
 * and no letter. */
static bool ligne_de_code(const char *s) { return a_chiffre(s) && !a_lettre(s); }

/* The spec's proof cases, at the 12 px floor in a 62 px label line. */
static void test_sw_cas_du_banc(void)
{
    char l[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
    TEST_ASSERT_EQ(wrap("TEST:RFC6238", l), 2, "TEST:RFC6238 needs two lines at 12 px");
    TEST_ASSERT(strcmp(l[0], "TEST:") == 0 && strcmp(l[1], "RFC6238") == 0, "TEST: / RFC6238 — never TEST:RFC / 6238");

    TEST_ASSERT_EQ(wrap("BANQUE:4021", l), 2, "BANQUE:4021 needs two lines");
    TEST_ASSERT(strcmp(l[0], "BANQU") == 0 && strcmp(l[1], "E:4021") == 0,
                "BANQU / E:4021: the ':' break would strand the digits, it moves back into the letters");

    TEST_ASSERT_EQ(wrap("4021:BANQUE", l), 2, "digits first: two lines");
    TEST_ASSERT(a_lettre(l[0]) && a_lettre(l[1]), "4021:BANQUE: the break moves forward, both lines hold a letter");

    /* A digit run with letters on both sides: split INSIDE the run so both
     * halves keep a letter (a greedy wrap would strand the digits). */
    TEST_ASSERT_EQ(wrap("AB:123456789:CD", l), 2, "AB:123456789:CD: two lines");
    TEST_ASSERT(a_lettre(l[0]) && a_lettre(l[1]), "... both holding a letter");

    TEST_ASSERT_EQ(wrap("GITHUB", l), 1, "a short name, one line");
    TEST_ASSERT(strcmp(l[0], "GITHUB") == 0, "GITHUB");
    TEST_ASSERT_EQ(wrap("OVH:PERSO", l), 2, "OVH:PERSO: 74 px");
    TEST_ASSERT(strcmp(l[0], "OVH:") == 0 && strcmp(l[1], "PERSO") == 0, "broken at the colon first");
    TEST_ASSERT_EQ(wrap("NIPHAR_CHEST", l), 2, "NIPHAR_CHEST: two lines");
    TEST_ASSERT(strcmp(l[0], "NIPHAR_") == 0 && strcmp(l[1], "CHEST") == 0, "broken at the underscore");

    TEST_ASSERT_EQ(wrap("", l), 1, "empty: one empty line, never zero");
    TEST_ASSERT(l[0][0] == '\0', "empty line");
    TEST_ASSERT_EQ(wrap(NULL, l), 1, "NULL: like empty, no crash");

    /* A label made of digits only is shown whole on one line (it IS the
     * label): the rule only forbids MAKING such a line out of a name. */
    TEST_ASSERT_EQ(wrap("123456", l), 1, "123456: one line, whole");
}

/* The 34-character label of the design brief: whole, every line within
 * 62 px, the break priority visible (':' '.' '@' '-'). */
static void test_sw_label34(void)
{
    char l[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
    const char *s = "GITHUB.COM:ALICE.MARTIN@WORK-2FA01";
    int n = wrap(s, l);
    char joint[64] = "";
    for (int i = 0; i < n; i++) {
        TEST_ASSERT(memlcd_text_width(MEMLCD_F_M12, l[i]) <= BUDGET, "every line within 62 px");
        TEST_ASSERT(!ligne_de_code(l[i]), "no line of digits without a letter");
        strcat(joint, l[i]);
    }
    TEST_ASSERT(strcmp(joint, s) == 0, "the lines ARE the label");
    TEST_ASSERT(n == 6 && strcmp(l[0], "GITHUB.") == 0 && strcmp(l[1], "COM:") == 0 && strcmp(l[5], "2FA01") == 0,
                "GITHUB. / COM: / ALICE. / MARTIN@ / WORK- / 2FA01");
}

/* Deterministic fuzz of issuer:account strings (letters, digits, the break
 * characters): no line ever holds digits without a letter unless the whole
 * string does, and the lines give back every character in order. */
static uint32_t s_x = 0x2026u;
static uint32_t rnd(void) { s_x = s_x * 1103515245u + 12345u; return s_x >> 16; }
static void fuzz_nom(char *s)
{
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static const char digit[] = "0123456789";
    static const char sep[] = ":.@-_ ";
    int len = 1 + (int)(rnd() % CHEST_LABEL_MAX), k = 0;
    while (k < len) {
        unsigned kind = rnd() % 10;
        int run = 1 + (int)(rnd() % 6);
        for (int j = 0; j < run && k < len; j++)
            s[k++] = kind < 4 ? alpha[rnd() % 52] : kind < 8 ? digit[rnd() % 10] : sep[rnd() % 6];
    }
    s[k] = '\0';
}
static void test_sw_fuzz(void)
{
    char s[CHEST_LABEL_MAX + 1], l[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
    char a[64], b[64], joint[64];
    int mauvais = 0, perdus = 0;
    for (int it = 0; it < 20000; it++) {
        fuzz_nom(s);
        int n = wrap(s, l);
        bool tout_chiffres = a_chiffre(s) && !a_lettre(s);
        joint[0] = '\0';
        for (int i = 0; i < n; i++) {
            if (!tout_chiffres && ligne_de_code(l[i])) mauvais++;
            strcat(joint, l[i]);
        }
        sans_espaces(s, a); sans_espaces(joint, b);
        if (strcmp(a, b) != 0) perdus++;
    }
    TEST_ASSERT_EQ(mauvais, 0, "fuzz: no name line of digits without a letter");
    TEST_ASSERT_EQ(perdus, 0, "fuzz: the lines give back every character, in order");
}

/* Bounded scan: a label buffer without a terminator in its first 34 bytes
 * is never read past them (the ASan binary turns an over-read red). */
static void test_sw_bounded_scan(void)
{
    char *buf = malloc(CHEST_LABEL_MAX);
    char l[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
    memset(buf, 'X', CHEST_LABEL_MAX);
    int n = wrap(buf, l);
    size_t total = 0;
    for (int i = 0; i < n; i++) total += strlen(l[i]);
    TEST_ASSERT_EQ(total, (size_t)CHEST_LABEL_MAX, "34 X's, unterminated: exactly 34 characters come back");
    free(buf);
}

/* The marked wrap of a prompt's label (memlcd_safe_wrap_marque, spec
 * "Continuation mark", Mae 2026-09-30): line 0 within the budget, every
 * later line within the budget minus the continuation mark's indent (the
 * view draws "↳" before it). A safe split is still preferred; when none
 * exists the label stays WHOLE and a continuation line may hold digits
 * alone — it is marked. The first line has no mark: it holds a letter
 * whenever ANY first line could. */
#define SUITE (BUDGET - MEMLCD_CAVE_MARQUE_PAS)   /* a continuation line: 54 px */
static int wrap_m(const char *s, uint8_t font, uint16_t budget, char out[][MEMLCD_SW_LINE_BUF])
{
    return memlcd_safe_wrap_marque(s, font, budget, MEMLCD_CAVE_MARQUE_PAS, out, MEMLCD_SW_MAX_LINES);
}
/* The test's own oracle: could a first line of `budget` px hold a letter?
 * (The label's prefix up to its first letter, leading spaces trimmed.) */
static bool tete_avec_lettre_possible(const char *s, uint8_t font, uint16_t budget)
{
    while (*s == ' ') s++;
    size_t k = 0;
    while (s[k] && !((s[k] >= 'A' && s[k] <= 'Z') || (s[k] >= 'a' && s[k] <= 'z'))) k++;
    return s[k] && memlcd_text_width_n(font, s, k + 1) <= budget;
}
/* Every line within its own budget, the lines ARE the text, and the first
 * line holds digits without a letter only when no first line could. */
static bool marque_ok(const char *s, uint8_t font, uint16_t budget, char l[][MEMLCD_SW_LINE_BUF], int n)
{
    char joint[64] = "", a[64], b[64];
    if (n < 1) return false;
    for (int i = 0; i < n; i++) {
        if (memlcd_text_width(font, l[i]) > (i ? budget - MEMLCD_CAVE_MARQUE_PAS : budget)) return false;
        strcat(joint, l[i]);
    }
    sans_espaces(s, a); sans_espaces(joint, b);
    if (strcmp(a, b) != 0) return false;
    bool tout_chiffres = a_chiffre(s) && !a_lettre(s);
    if (!tout_chiffres && ligne_de_code(l[0]) && tete_avec_lettre_possible(s, font, budget)) return false;
    return true;
}
static void test_sw_marque(void)
{
    char l[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
    TEST_ASSERT(MEMLCD_CAVE_MARQUE_PAS == MEMLCD_CAVE_MARQUE_W + MEMLCD_CAVE_MARQUE_GAP && SUITE == 54,
                "the mark takes 6 px + 2 px of gap: a continuation line has 54 px");

    /* The review's case (C1): no safe split exists; whole, marked. */
    int n = wrap_m("AWS:123456789012", MEMLCD_F_M12, BUDGET, l);
    TEST_ASSERT_EQ(n, 3, "AWS:123456789012: three lines at 12 px");
    TEST_ASSERT(strcmp(l[0], "AWS:") == 0 && strcmp(l[1], "1234567") == 0 && strcmp(l[2], "89012") == 0,
                "AWS: / (mark)1234567 / (mark)89012 — the issuer alone, the id on marked lines");
    TEST_ASSERT(marque_ok("AWS:123456789012", MEMLCD_F_M12, BUDGET, l, n), "... within budgets, whole");

    /* A safe split is still preferred: the spec's proof cases are unchanged. */
    n = wrap_m("TEST:RFC6238", MEMLCD_F_M12, BUDGET, l);
    TEST_ASSERT(n == 2 && strcmp(l[0], "TEST:") == 0 && strcmp(l[1], "RFC6238") == 0, "TEST: / RFC6238 (54 px: fits after the mark)");
    n = wrap_m("BANQUE:4021", MEMLCD_F_M12, BUDGET, l);
    TEST_ASSERT(n == 2 && strcmp(l[0], "BANQU") == 0 && strcmp(l[1], "E:4021") == 0, "BANQU / E:4021: still safe first");

    /* 34 characters, 30 of them digits. */
    const char *d34 = "AWS:123456789012345678901234567890";
    TEST_ASSERT_EQ(strlen(d34), (size_t)CHEST_LABEL_MAX, "the digit-heavy label is the chest's maximum");
    n = wrap_m(d34, MEMLCD_F_M12, BUDGET, l);
    TEST_ASSERT(n >= 2 && marque_ok(d34, MEMLCD_F_M12, BUDGET, l, n), "34 digit-heavy: whole, within budgets, first line safe");
    TEST_ASSERT(!ligne_de_code(l[0]), "... its first line holds the issuer");

    /* Digits FIRST, wider than a line: no first line can hold a letter. */
    const char *d34t = "123456789012345678901234567890:AWS";
    n = wrap_m(d34t, MEMLCD_F_M12, BUDGET, l);
    TEST_ASSERT(!tete_avec_lettre_possible(d34t, MEMLCD_F_M12, BUDGET), "no first line of 62 px can reach the A");
    TEST_ASSERT(n >= 2 && marque_ok(d34t, MEMLCD_F_M12, BUDGET, l, n), "digits first: whole, within budgets");

    /* 34 W: more than MEMLCD_SW_MAX_LINES lines at 12 px -> 0 (the caller
     * moves to UNSCII 8), and in UNSCII 8 over the full width: 5 lines. */
    const char *w34 = "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW";
    TEST_ASSERT_EQ(wrap_m(w34, MEMLCD_F_M12, BUDGET, l), 0, "34 W at 12 px: does not fit 10 lines");
    n = wrap_m(w34, MEMLCD_F_U8, MEMLCD_W_BUDGET, l);
    TEST_ASSERT(n == 5 && marque_ok(w34, MEMLCD_F_U8, MEMLCD_W_BUDGET, l, n), "34 W in UNSCII 8: 8 + 7 + 7 + 7 + 5");

    /* max_lines is a hard cap, whatever the cost-optimal wrap would take. */
    n = memlcd_safe_wrap_marque("GITHUB.COM:ALICE.MARTIN@WORK-2FA01", MEMLCD_F_M12, BUDGET, MEMLCD_CAVE_MARQUE_PAS, l, 6);
    TEST_ASSERT(n >= 1 && n <= 6 && marque_ok("GITHUB.COM:ALICE.MARTIN@WORK-2FA01", MEMLCD_F_M12, BUDGET, l, n),
                "the brief's label in at most 6 lines at 12 px");

    /* Fuzz, both fonts: whole, within budgets, the first line holds a
     * letter whenever it can; UNSCII 8 over the full width never fails. */
    char s[CHEST_LABEL_MAX + 1];
    int mauvais = 0, vides = 0;
    s_x = 0x1a2bu;
    for (int it = 0; it < 20000; it++) {
        fuzz_nom(s);
        n = wrap_m(s, MEMLCD_F_M12, BUDGET, l);
        if (n && !marque_ok(s, MEMLCD_F_M12, BUDGET, l, n)) mauvais++;
        /* capped at 5 lines — what the prompt's band always holds */
        n = memlcd_safe_wrap_marque(s, MEMLCD_F_U8, MEMLCD_W_BUDGET, MEMLCD_CAVE_MARQUE_PAS, l, 5);
        if (!n) vides++;
        else if (!marque_ok(s, MEMLCD_F_U8, MEMLCD_W_BUDGET, l, n) || n > 5) mauvais++;
    }
    TEST_ASSERT_EQ(mauvais, 0, "fuzz: marked wrap whole, within budgets, first line safe when it can be");
    TEST_ASSERT_EQ(vides, 0, "fuzz: UNSCII 8 over the full width always wraps in 5 lines");
}

/* The water drop: wet rows from the percentage, rounded — 0/10/30/60/100 %
 * give 0/2/5/10/16 of its 16 rows; the outline doubles when LOW. */
static void test_goutte(void)
{
    TEST_ASSERT_EQ(memlcd_goutte_rangs(0), 0, "0 %: dry");
    TEST_ASSERT_EQ(memlcd_goutte_rangs(10), 2, "10 %: 2 rows");
    TEST_ASSERT_EQ(memlcd_goutte_rangs(30), 5, "30 %: 5 rows");
    TEST_ASSERT_EQ(memlcd_goutte_rangs(60), 10, "60 %: 10 rows");
    TEST_ASSERT_EQ(memlcd_goutte_rangs(100), 16, "100 %: full");
    TEST_ASSERT_EQ(memlcd_goutte_rangs(250), 16, "never above full");
    for (int p = 0; p < 100; p++) TEST_ASSERT(memlcd_goutte_rangs((uint8_t)p) <= memlcd_goutte_rangs((uint8_t)(p + 1)), "monotonic");

    /* Row 12 (the bulb, half-width 5 around x 6): outline at x 1 and 11. */
    TEST_ASSERT(memlcd_goutte_px(0, false, 1, 12) && memlcd_goutte_px(0, false, 11, 12), "outline 1 px");
    TEST_ASSERT(!memlcd_goutte_px(0, false, 2, 12) && !memlcd_goutte_px(0, false, 6, 12), "0 %: empty inside");
    TEST_ASSERT(memlcd_goutte_px(0, true, 2, 12) && memlcd_goutte_px(0, true, 10, 12), "LOW: outline 2 px");
    TEST_ASSERT(!memlcd_goutte_px(0, true, 6, 12), "LOW, 0 %: still empty inside");
    TEST_ASSERT(memlcd_goutte_px(100, false, 6, 12) && memlcd_goutte_px(100, false, 6, 3), "100 %: water to the tip");
    TEST_ASSERT(memlcd_goutte_px(0, false, 6, 0), "the tip");
    TEST_ASSERT(!memlcd_goutte_px(100, false, 0, 12) && !memlcd_goutte_px(100, false, 12, 12), "nothing outside the drop");
    /* 50 %: row 8 from the bottom is the surface — staggered 0/1 px by column. */
    TEST_ASSERT(memlcd_goutte_px(50, false, 6, 8) != memlcd_goutte_px(50, false, 7, 8), "a wavy surface, not a flat cut");
    TEST_ASSERT(memlcd_goutte_px(50, false, 6, 14) && !memlcd_goutte_px(50, false, 6, 4), "wet under, dry over");

    /* The bitmap IS the pixel function (ALPHA_1BIT, MSB = leftmost). */
    uint8_t bm[MEMLCD_CAVE_GOUTTE_H * MEMLCD_CAVE_GOUTTE_STRIDE];
    static const uint8_t pcts[] = { 0, 10, 30, 60, 100 };
    for (size_t k = 0; k < sizeof pcts; k++)
        for (int low = 0; low < 2; low++) {
            memlcd_goutte_bitmap(pcts[k], low, bm);
            int bad = 0;
            for (int y = 0; y < MEMLCD_CAVE_GOUTTE_H; y++)
                for (int x = 0; x < 16; x++) {
                    bool bit = bm[y * 2 + (x >> 3)] & (0x80 >> (x & 7));
                    if (bit != memlcd_goutte_px(pcts[k], low, x, y)) bad++;
                }
            TEST_ASSERT_EQ(bad, 0, "bitmap == pixel function");
        }
}

/* The test's own minimal frame check (independent of memlcd_cave_tient):
 * every line inside the content box and inside its line box. */
static bool dans_le_cadre(const memlcd_cave_vue_t *v)
{
    for (uint8_t i = 0; i < v->n; i++) {
        const memlcd_cave_ligne_t *l = &v->l[i];
        if (l->y < v->top || l->y + memlcd_font_pas(l->font) > v->bottom) return false;
        if (memlcd_text_width(l->font, l->text) + 2 > l->w || l->x + l->w > MEMLCD_W) return false;
    }
    if (v->goutte && v->goutte_y + MEMLCD_CAVE_GOUTTE_H > v->bottom) return false;
    /* a continuation mark: inside the panel and the content box, clear of
     * every line's ink */
    for (uint8_t i = 0; i < v->n; i++) {
        const memlcd_cave_ligne_t *l = &v->l[i];
        if (!l->marque) continue;
        int mx0 = l->x - MEMLCD_CAVE_MARQUE_PAS, my0 = l->y + memlcd_cave_marque_dy(l->font);
        if (mx0 < 0 || my0 < v->top || my0 + MEMLCD_CAVE_MARQUE_H > v->bottom) return false;
        for (uint8_t j = 0; j < v->n; j++) {
            const memlcd_cave_ligne_t *k = &v->l[j];
            int kw = memlcd_text_width(k->font, k->text);
            int kx0 = k->left ? k->x : k->x + (k->w - kw) / 2;
            if (kw && mx0 < kx0 + kw && kx0 < mx0 + MEMLCD_CAVE_MARQUE_W
                && my0 < k->y + memlcd_font_pas(k->font) && k->y < my0 + MEMLCD_CAVE_MARQUE_H) return false;
        }
    }
    return true;
}

/* The prompt band: the 34-character label whole between the divider and
 * PRESS, at >= 12 px, with room made by the op title. */
static void test_budget_prompt(void)
{
    memlcd_cave_vue_t v;
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE };
    m.coffre_op = 7; m.coffre_op_count = 1;
    strcpy(m.coffre_label, "GITHUB.COM:ALICE.MARTIN@WORK-2FA01");
    memlcd_cave_vue(&m, 80, &v);
    int press = -1;
    for (uint8_t i = 0; i < v.n; i++) if (strcmp(v.l[i].text, "PRESS") == 0) press = i;
    TEST_ASSERT(press > 0, "PRESS present");
    char joint[64] = "";
    for (uint8_t i = v.txt_i; i < v.txt_i + v.txt_n; i++) {
        TEST_ASSERT(v.l[i].font >= MEMLCD_F_M12 && v.l[i].font <= MEMLCD_F_M32, "label at >= 12 px, Montserrat");
        TEST_ASSERT(v.l[i].y >= v.rule_y + 2, "under the divider");
        TEST_ASSERT(press > 0 && v.l[i].y + memlcd_font_pas(v.l[i].font) <= v.l[press].y, "above PRESS");
        TEST_ASSERT(memlcd_text_width(v.l[i].font, v.l[i].text) + 2 <= v.l[i].w, "within its line");
        strcat(joint, v.l[i].text);
    }
    TEST_ASSERT(strcmp(joint, m.coffre_label) == 0, "the 34-character label, whole");
    TEST_ASSERT(v.tient, "the layout says it holds");
}

/* Across a fuzz of names on every view that shows one — prompt (the
 * chest's label), code and browser (the cursor's copy) — no line holds
 * digits without a letter, and every view fits. */
static void test_budget_fuzz(void)
{
    memlcd_cave_vue_t v;
    char s[CHEST_LABEL_MAX + 1];
    int mauvais = 0, debordes = 0, incomplets = 0, recours = 0, mal_marques = 0;
    s_x = 0x4021u;
    for (int it = 0; it < 6000; it++) {
        fuzz_nom(s);
        bool tout_chiffres = a_chiffre(s) && !a_lettre(s);
        memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE, .batt_local_dv = 38, .nom = "BASE" };
        m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | ((it & 1) ? CHEST_STATE_TIME : 0);
        int vue = it % 3;
        if (vue == 0) { m.coffre_op = (uint16_t)(1 + rnd() % 10); m.coffre_op_count = (uint8_t)(rnd() % 13); strcpy(m.coffre_label, s); }
        else if (vue == 1) { m.coffre_code_visible = 1; strcpy(m.coffre_code, (it & 2) ? "418902" : "12345678"); m.coffre_code_secs = 17; strcpy(m.coffre_nom, s); }
        else { m.coffre_browsing = 1; m.coffre_total = 9; m.coffre_pos = 4; strcpy(m.coffre_nom, s); m.dongle_vu = it & 4 ? 1 : 0; }
        memlcd_cave_vue(&m, 55, &v);
        if (!v.tient || !dans_le_cadre(&v)) debordes++;
        /* The invariant: a line with digits and no letter carries the
         * continuation mark — only a label's FIRST line may go without, and
         * only when no first line could reach a letter (or the whole text
         * has none: it IS digits). Every line of every view is checked. */
        for (uint8_t i = 0; i < v.n; i++) {
            bool nom = i >= v.txt_i && i < v.txt_i + v.txt_n;
            if (!nom) { if (v.l[i].marque) mauvais++; continue; }
            if (!ligne_de_code(v.l[i].text) || v.l[i].marque) continue;
            if (tout_chiffres) continue;
            if (i == v.txt_i && !tete_avec_lettre_possible(s, v.l[i].font, (uint16_t)(v.l[i].w - 2))) { recours++; continue; }
            mauvais++;
        }
        /* The mark is the prompt's, and consistent there: every line after
         * the label's first carries it, the first never does; a browsed or
         * code name (cut with '~', never a digits line) never does. */
        for (uint8_t i = v.txt_i; i < v.txt_i + v.txt_n; i++) {
            bool attendu = vue == 0 && i > v.txt_i;
            if (!!v.l[i].marque != attendu) mal_marques++;
        }
        if (vue == 0) {
            char joint[64] = "", a[64], b[64];
            for (uint8_t i = v.txt_i; i < v.txt_i + v.txt_n; i++) strcat(joint, v.l[i].text);
            sans_espaces(s, a); sans_espaces(joint, b);
            if (strcmp(a, b) != 0) incomplets++;
        }
    }
    TEST_ASSERT_EQ(mauvais, 0, "fuzz: no name line of digits without a letter, on any view");
    TEST_ASSERT_EQ(debordes, 0, "fuzz: every view fits the panel");
    TEST_ASSERT_EQ(incomplets, 0, "fuzz: a prompt's label is always whole");
    TEST_ASSERT_EQ(mal_marques, 0, "fuzz: continuation lines of a prompt's label marked, nothing else");
    (void)recours;   /* labels STARTING with a digit run wider than a line: their first line cannot hold a letter */
}

/* The continuation mark on a prompt (review C1, 2026-09-30): a label with
 * NO safe split — an AWS account id — stays WHOLE in Montserrat 12, and
 * every line after the first carries the drawn mark; the digit-heavy
 * 34-character label too, with and without N ACCTS. Realistic labels never
 * show a line of digits at all. */
static void prompt_label(memlcd_model_t *m, memlcd_cave_vue_t *v, const char *label, char *joint)
{
    strcpy(m->coffre_label, label);
    memlcd_cave_vue(m, 80, v);
    joint[0] = '\0';
    for (uint8_t i = v->txt_i; i < v->txt_i + v->txt_n; i++) strcat(joint, v->l[i].text);
}
static void test_budget_marque(void)
{
    memlcd_cave_vue_t v;
    char joint[64];
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE };
    m.coffre_op = 7; m.coffre_op_count = 1;

    prompt_label(&m, &v, "AWS:123456789012", joint);
    TEST_ASSERT(strcmp(joint, "AWS:123456789012") == 0 && v.tient && dans_le_cadre(&v), "AWS:123456789012: whole, fits");
    TEST_ASSERT(v.txt_n == 3 && v.l[v.txt_i].font == MEMLCD_F_M12, "... three lines of Montserrat 12, no UNSCII");
    TEST_ASSERT(strcmp(v.l[v.txt_i].text, "AWS:") == 0 && !v.l[v.txt_i].marque, "AWS: first, unmarked");
    TEST_ASSERT(strcmp(v.l[v.txt_i + 1].text, "1234567") == 0 && v.l[v.txt_i + 1].marque, "(mark) 1234567");
    TEST_ASSERT(strcmp(v.l[v.txt_i + 2].text, "89012") == 0 && v.l[v.txt_i + 2].marque, "(mark) 89012");
    /* the mark sits before its text, on the text's row */
    const memlcd_cave_ligne_t *c = &v.l[v.txt_i + 1];
    TEST_ASSERT(c->left && c->x >= MEMLCD_CAVE_MARQUE_PAS && memlcd_cave_marque_dy(c->font) >= 0
                && memlcd_cave_marque_dy(c->font) + MEMLCD_CAVE_MARQUE_H <= memlcd_font_pas(c->font),
                "the mark: left of the text, within the line's height");

    const char *d34 = "AWS:123456789012345678901234567890";
    for (int cpt = 1; cpt <= 12; cpt += 11) {
        m.coffre_op_count = (uint8_t)cpt;
        prompt_label(&m, &v, d34, joint);
        TEST_ASSERT(strcmp(joint, d34) == 0 && v.tient && dans_le_cadre(&v), "34 digit-heavy: whole, fits");
        TEST_ASSERT(!v.l[v.txt_i].marque && !ligne_de_code(v.l[v.txt_i].text), "... first line AWS..., unmarked");
        for (uint8_t i = v.txt_i + 1; i < v.txt_i + v.txt_n; i++)
            TEST_ASSERT(v.l[i].marque, "... every digits line marked");
    }
    m.coffre_op_count = 1;

    /* The digit-first label: the one line of digits without a mark is the
     * first, where no letter could be reached. */
    const char *d34t = "123456789012345678901234567890:AWS";
    prompt_label(&m, &v, d34t, joint);
    TEST_ASSERT(strcmp(joint, d34t) == 0 && v.tient && dans_le_cadre(&v), "digits first: whole, fits");
    for (uint8_t i = v.txt_i + 1; i < v.txt_i + v.txt_n; i++) TEST_ASSERT(v.l[i].marque, "... continuation lines marked");

    static const char *const reels[] = {
        "GITHUB.COM:ALICE.MARTIN@WORK-2FA01", "github.com:alice.martin@work-2fa01", "TEST:RFC6238",
        "BANQUE:4021", "Google:alice@gmail.com", "OVH:PERSO", "NIPHAR_CHEST", "12 COMPTES",
        "Jetable:huit", "gitlab.com:harrael", "Proton:mae@proton.me",
    };
    for (size_t i = 0; i < sizeof reels / sizeof reels[0]; i++) {
        prompt_label(&m, &v, reels[i], joint);
        TEST_ASSERT(strcmp(joint, reels[i]) == 0 && v.tient, "a realistic label: whole, fits");
        TEST_ASSERT(v.l[v.txt_i].font != MEMLCD_F_U8, "... in Montserrat");
        for (uint8_t k = v.txt_i; k < v.txt_i + v.txt_n; k++) {
            TEST_ASSERT(!ligne_de_code(v.l[k].text), "... never a line of digits alone");
            TEST_ASSERT(!!v.l[k].marque == (k > v.txt_i), "... its continuation lines marked, its first not");
        }
    }
}

/* The normal screen under every combination of what it can show at once:
 * route, SEEN, link, the chest's two rows, every flag, short and long layer
 * names — nothing ever leaves the panel or overlaps. */
static void test_budget_normal(void)
{
    static const char *const noms[] = { "BASE", "NAVIGATION", "", "WWWWWWWWWWWWWWW", "LAYER 2", "sym" };
    memlcd_cave_vue_t v;
    int debordes = 0, total = 0;
    for (size_t k = 0; k < sizeof noms / sizeof noms[0]; k++)
        for (unsigned bits = 0; bits < 256; bits++) {
            memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE, .batt_local_dv = 37 };
            strcpy(m.nom, noms[k]);
            m.route_rf = bits & 1; m.dongle_vu = (bits >> 1) & 1; m.lien_5v = (bits >> 2) & 1;
            m.caps_lock = (bits >> 3) & 1; m.caps_word = (bits >> 4) & 1;
            m.osm = (bits >> 5) & 1 ? 0xFF : 0; m.osl = (bits >> 6) & 1 ? 7 : MEMLCD_OSL_AUCUNE;
            if ((bits >> 7) & 1) {
                m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY;   /* no SD: mode + NO CARD */
                m.coffre_mode_state = CHEST_MODE_PENDING; m.coffre_mode_wanted = CHEST_MODE_OATH;
            }
            memlcd_cave_vue(&m, 100, &v);
            total++;
            if (!v.tient || !dans_le_cadre(&v) || v.kind != MEMLCD_CV_NORMAL) debordes++;
        }
    TEST_ASSERT_EQ(debordes, 0, "every combination of the normal screen holds");
    TEST_ASSERT_EQ(total, 6 * 256, "all combinations tried");
}

void test_memlcd_safe_wrap(void)
{
    TEST_SUITE("Memory-LCD left screen: safe wrap, water drop, layout budget");
    test_sw_cas_du_banc();
    test_sw_label34();
    test_sw_fuzz();
    test_sw_bounded_scan();
    test_sw_marque();
    test_goutte();
    test_budget_prompt();
    test_budget_fuzz();
    test_budget_marque();
    test_budget_normal();
}
