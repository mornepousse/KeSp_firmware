#pragma once
/* The left half's screen, "cave" (spec docs/superpowers/specs/
 * 2026-09-29-left-screen-redesign.md, plan 2026-09-30-left-screen-cave).
 *
 * Two halves in one header:
 *  - the VIEW, pure (no LVGL): memlcd_cave_vue() turns the model into a list
 *    of positioned text lines and pictograms. Every layout decision lives
 *    here and is tested on host (test/test_memlcd_model.c,
 *    test/test_memlcd_safe_wrap.c) with the width oracle of memlcd_model.h;
 *  - the ENGINE, LVGL (memlcd_cave.c): builds the objects once and draws a
 *    view. tools/memlcd_sim compiles memlcd_cave.c itself, so the mockups
 *    ARE the firmware's drawing.
 *
 * Dark: pale ink on black paper (Mae's choice, 2026-09-29). One family,
 * Montserrat, from 32 down to the 12 px floor; UNSCII 8 only as the last
 * resort that keeps a pathological chest label whole on a prompt.
 *
 * Security rules (hard, spec "Security rules for text"):
 *  - a prompt shows the CHEST's label (coffre_label), never the browser's
 *    copy (coffre_nom), WHOLE, nothing overlapping it — the op title, the
 *    divider and the rock shrink first, UNSCII 8 last;
 *  - a name line never holds a digit without a letter (memlcd_safe_wrap.h);
 *  - no code without coffre_code_visible; priority prompt > code > browser. */
#include "memlcd_model.h"
#include "memlcd_safe_wrap.h"

/* ── Geometry ───────────────────────────────────────────────────────── */
#define MEMLCD_CAVE_ROCK_TOP_H   12      /* rock bitmaps (memlcd_assets_cave.c) */
#define MEMLCD_CAVE_ROCK_BOT_H   9
/* Content box: normal rock, or "thin" rock (both bitmaps slid half off the
 * screen, still a rocky edge) on the full-screen views and when a view needs
 * the room. Text never enters the rock. */
#define MEMLCD_CAVE_TOP          14
#define MEMLCD_CAVE_BOTTOM       150
#define MEMLCD_CAVE_TOP_THIN     10
#define MEMLCD_CAVE_BOTTOM_THIN  154
#define MEMLCD_CAVE_ROCK_TOP_THIN_Y  (-4)
#define MEMLCD_CAVE_ROCK_BOT_THIN_Y  (MEMLCD_H - MEMLCD_CAVE_ROCK_BOT_H + 5)

#define MEMLCD_CAVE_LOGO_S       28      /* corner logo — the smallest that stays recognisable */
#define MEMLCD_CAVE_LOGO_L       56      /* sleep */
#define MEMLCD_CAVE_CADENAS_W    24
#define MEMLCD_CAVE_CADENAS_H    28
#define MEMLCD_CAVE_LIEN_W       16      /* TRRS link pictogram ⇆ */
#define MEMLCD_CAVE_LIEN_H       12
#define MEMLCD_CAVE_GOUTTE_W     13      /* water-drop gauge (G3) */
#define MEMLCD_CAVE_GOUTTE_H     16
#define MEMLCD_CAVE_GOUTTE_STRIDE 2      /* ALPHA_1BIT: ceil(13 / 8) bytes a row */
#define MEMLCD_CAVE_EAU_W        44      /* TOTP countdown water bar */
#define MEMLCD_CAVE_EAU_H        14
#define MEMLCD_CAVE_OMBRE_H      4       /* its dither shadow, under it */

#define MEMLCD_CAVE_GAP          2
#define MEMLCD_CAVE_LABEL_X      2       /* a name/label line: box 64 px, 62 px of text */
#define MEMLCD_CAVE_LABEL_W      (MEMLCD_W - 4)
#define MEMLCD_CAVE_FLOOR        MEMLCD_F_M12
#define MEMLCD_CAVE_LIGNES       14

typedef enum {
    MEMLCD_CV_NORMAL, MEMLCD_CV_BROWSE, MEMLCD_CV_PROMPT, MEMLCD_CV_CODE, MEMLCD_CV_SLEEP
} memlcd_cave_kind_t;

typedef struct {
    uint8_t font;                  /* memlcd_font_t */
    uint8_t x, y, w;               /* the label's box: x..x+w, text inside w - 2 */
    uint8_t left;                  /* 1 = left-aligned, else centred */
    char    text[MEMLCD_VC_TXT];
} memlcd_cave_ligne_t;

typedef struct {
    uint8_t kind;                  /* memlcd_cave_kind_t */
    uint8_t rock_thin;
    uint8_t top, bottom;           /* the content box: [top, bottom) */
    uint8_t logo, logo_x, logo_y;  /* 0, MEMLCD_CAVE_LOGO_S or MEMLCD_CAVE_LOGO_L */
    uint8_t cadenas, cadenas_x, cadenas_y;
    uint8_t goutte, goutte_x, goutte_y, goutte_pct, goutte_low;
    uint8_t lien, lien_x, lien_y;
    uint8_t rule, rule_y;          /* prompt: the wavy divider, ink on rule_y - 1 .. rule_y + 1 */
    uint8_t eau, eau_x, eau_y, eau_pct;   /* code: the countdown bar + its shadow */
    uint8_t n;                     /* lines used */
    uint8_t txt_i, txt_n;          /* the lines holding the label (prompt) or the name (code, browse) */
    uint8_t tient;                 /* 1 = memlcd_cave_tient() held for the chosen layout */
    uint8_t coupe_brute;           /* prompt: the label took the last resort (plain 8-character
                                    * cut) — no safe split exists; the label stays WHOLE */
    memlcd_cave_ligne_t l[MEMLCD_CAVE_LIGNES];
} memlcd_cave_vue_t;

/* ── The water drop (G3, settled 2026-09-30) ─────────────────────────
 * 13 x 16: a tip tapering linearly into a round bulb of radius 6 (half
 * widths below, from tools/memlcd_sim's round-4 formula), filled from the
 * bottom — the top wet row staggered 0/1 px column by column for a wavy
 * surface — outlined 1 px, 2 px when the battery is LOW. */
static const uint8_t memlcd_goutte_demi[MEMLCD_CAVE_GOUTTE_H] = {
    0, 1, 1, 2, 3, 3, 4, 5, 5, 6, 6, 6, 5, 4, 3, 0,
};

/* Wet rows for a percentage: 0 % -> 0, 100 % -> 16, rounded. */
static inline uint8_t memlcd_goutte_rangs(uint8_t pct)
{
    unsigned r = ((unsigned)(pct > 100 ? 100 : pct) * MEMLCD_CAVE_GOUTTE_H + 50) / 100;
    return (uint8_t)r;
}

/* 1 = ink at (x, y) of the drop. */
static inline bool memlcd_goutte_px(uint8_t pct, bool low, int x, int y)
{
    if (x < 0 || y < 0 || x >= MEMLCD_CAVE_GOUTTE_W || y >= MEMLCD_CAVE_GOUTTE_H) return false;
    const int cx = MEMLCD_CAVE_GOUTTE_W / 2;
    int hw = memlcd_goutte_demi[y];
    if (hw <= 0) return y == 0 && x == cx;         /* the tip */
    int xl = cx - hw, xr = cx + hw;
    if (x < xl || x > xr) return false;
    int bord = low ? 2 : 1;
    if (x - xl < bord || xr - x < bord) return true;   /* outline */
    int seuil = MEMLCD_CAVE_GOUTTE_H - memlcd_goutte_rangs(pct) + (x % 2);
    return y >= seuil;                             /* water, wavy surface */
}

/* The drop as an LV_IMG_CF_ALPHA_1BIT bitmap (MSB = leftmost pixel). */
static inline void memlcd_goutte_bitmap(uint8_t pct, bool low,
                                        uint8_t out[MEMLCD_CAVE_GOUTTE_H * MEMLCD_CAVE_GOUTTE_STRIDE])
{
    memset(out, 0, MEMLCD_CAVE_GOUTTE_H * MEMLCD_CAVE_GOUTTE_STRIDE);
    for (int y = 0; y < MEMLCD_CAVE_GOUTTE_H; y++)
        for (int x = 0; x < MEMLCD_CAVE_GOUTTE_W; x++)
            if (memlcd_goutte_px(pct, low, x, y))
                out[y * MEMLCD_CAVE_GOUTTE_STRIDE + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

/* ── Building blocks ─────────────────────────────────────────────────── */
static inline memlcd_cave_ligne_t *memlcd_cave_add(memlcd_cave_vue_t *v, uint8_t font, uint8_t x, uint8_t y,
                                                   uint8_t w, bool left, const char *s)
{
    if (v->n >= MEMLCD_CAVE_LIGNES) { v->tient = 0; return NULL; }
    memlcd_cave_ligne_t *l = &v->l[v->n++];
    l->font = font; l->x = x; l->y = y; l->w = w; l->left = left ? 1 : 0;
    snprintf(l->text, sizeof l->text, "%s", s ? s : "");
    return l;
}
static inline void memlcd_cave_row(memlcd_cave_vue_t *v, uint8_t font, uint8_t y, const char *s)
{
    (void)memlcd_cave_add(v, font, 0, y, MEMLCD_W, false, s);
}

/* The Montserrat ladder, widest first. */
static const uint8_t memlcd_cave_ladder[] = {
    MEMLCD_F_M32, MEMLCD_F_M28, MEMLCD_F_M24, MEMLCD_F_M20,
    MEMLCD_F_M18, MEMLCD_F_M16, MEMLCD_F_M14, MEMLCD_F_M12,
};
#define MEMLCD_CAVE_LADDER_N ((int)sizeof memlcd_cave_ladder)
#define MEMLCD_CAVE_RANG(f)  (MEMLCD_F_M32 - (f))   /* the ladder index of a Montserrat size */

#define MEMLCD_CAVE_LABEL_BUDGET (MEMLCD_CAVE_LABEL_W - 2)
static inline bool memlcd_cave_lignes_ok(uint8_t font, uint16_t budget,
                                         char lines[][MEMLCD_SW_LINE_BUF], int n)
{
    for (int i = 0; i < n; i++) if (memlcd_text_width(font, lines[i]) > budget) return false;
    return true;
}
/* Ends the kept lines with '~' (a cut is never silent): the last line is
 * shortened until "line~" fits — measured whole, the '~' kerns with what
 * precedes it — and a line that would be left with digits but no letter is
 * dropped instead. Returns the lines kept (>= 1: at worst a lone "~"). */
static inline int memlcd_cave_tilde(uint8_t f, uint16_t budget, char lines[][MEMLCD_SW_LINE_BUF], int n)
{
    while (n > 0) {
        char *t = lines[n - 1];
        size_t k = strlen(t);
        if (k > MEMLCD_SW_LINE_BUF - 2) k = MEMLCD_SW_LINE_BUF - 2;
        for (; k > 0; k--) {
            t[k] = '~'; t[k + 1] = '\0';
            if (memlcd_text_width(f, t) <= budget) break;
        }
        if (k > 0 && !memlcd_sw_line_is_unsafe(t, k)) return n;
        n--;                                           /* nothing safe left on it: drop it */
    }
    snprintf(lines[0], MEMLCD_SW_LINE_BUF, "~");
    return 1;
}

/* Hero text (layer name, op title): the WIDEST ladder font, from `plafond`
 * down, holding the whole string on one line within MEMLCD_W_BUDGET and
 * max_h. Below that, at the floor: two balanced lines ("NAVIG" / "ATION",
 * never "NAVIGATIO" / "N"), else the safe wrap. A hero is never a name or a
 * code, but the safe wrap costs nothing. Returns the height used, 0 when
 * even the floor does not fit max_h (nothing added). */
static inline uint8_t memlcd_cave_hero_c(memlcd_cave_vue_t *v, uint8_t y, uint8_t plafond, uint8_t max_h,
                                         const char *s, bool coupe)
{
    for (int i = MEMLCD_CAVE_RANG(plafond); i < MEMLCD_CAVE_LADDER_N; i++) {
        uint8_t f = memlcd_cave_ladder[i];
        if (memlcd_text_width(f, s) <= MEMLCD_W_BUDGET && memlcd_font_pas(f) <= max_h) {
            memlcd_cave_row(v, f, y, s);
            return memlcd_font_pas(f);
        }
    }
    const uint8_t f = MEMLCD_CAVE_FLOOR, lh = memlcd_font_pas(f);
    size_t len = strnlen(s, MEMLCD_VC_TXT - 1);
    size_t best = 0;
    uint16_t best_diff = 0xFFFF;
    for (size_t i = 1; i < len; i++) {
        uint16_t w1 = memlcd_text_width_n(f, s, i), w2 = memlcd_text_width_n(f, s + i, len - i);
        if (w1 > MEMLCD_W_BUDGET || w2 > MEMLCD_W_BUDGET) continue;
        if (memlcd_sw_line_is_unsafe(s, i) || memlcd_sw_line_is_unsafe(s + i, len - i)) continue;
        uint16_t d = (uint16_t)(w1 > w2 ? w1 - w2 : w2 - w1);
        if (d < best_diff) { best_diff = d; best = i; }
    }
    if (best) {
        if (2 * lh > max_h) return 0;
        char a[MEMLCD_VC_TXT];
        snprintf(a, sizeof a, "%.*s", (int)best, s);
        memlcd_cave_row(v, f, y, a);
        snprintf(a, sizeof a, "%s", s + best);
        memlcd_cave_row(v, f, (uint8_t)(y + lh), a);
        return (uint8_t)(2 * lh);
    }
    char lines[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
    int n = memlcd_safe_wrap(s, f, MEMLCD_W_BUDGET, lines, MEMLCD_SW_MAX_LINES);
    bool large = !memlcd_cave_lignes_ok(f, MEMLCD_W_BUDGET, lines, n);
    if (n * lh > max_h || large) {
        /* A layer name is not a security text: in the last arrangement it
         * may be cut, with a '~', rather than push the screen off the panel. */
        if (!coupe || max_h < lh) return 0;
        int garde = n;
        for (int i = 0; i < n; i++) if (memlcd_text_width(f, lines[i]) > MEMLCD_W_BUDGET) { garde = i + 1; break; }
        n = garde < max_h / lh ? garde : max_h / lh;
        n = memlcd_cave_tilde(f, MEMLCD_W_BUDGET, lines, n);
    }
    for (int i = 0; i < n; i++) memlcd_cave_row(v, f, (uint8_t)(y + i * lh), lines[i]);
    return (uint8_t)(n * lh);
}
static inline uint8_t memlcd_cave_hero(memlcd_cave_vue_t *v, uint8_t y, uint8_t plafond, uint8_t max_h,
                                       const char *s)
{
    return memlcd_cave_hero_c(v, y, plafond, max_h, s, false);
}

/* A name or a label, in the label box (x 2, 64 wide, 62 px of text): the
 * whole string on ONE line at 14 then 12 px if it fits, else safe-wrapped
 * at the 12 px floor. Writes nothing; returns the lines NEEDED, their font,
 * and in *ok whether every line is within the 62 px — false when the safe
 * wrap had to rejoin two lines (a run of digits and punctuation too long
 * to split without leaving a line of digits alone: "AWS:123456789012"). */
static inline int memlcd_cave_label_lignes(const char *s, uint8_t *font,
                                           char lines[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF], bool *ok)
{
    static const uint8_t ladder[] = { MEMLCD_F_M14, MEMLCD_F_M12 };
    for (size_t i = 0; i < sizeof ladder; i++) {
        if (memlcd_text_width(ladder[i], s) <= MEMLCD_CAVE_LABEL_BUDGET) {
            *font = ladder[i];
            snprintf(lines[0], MEMLCD_SW_LINE_BUF, "%s", s ? s : "");
            *ok = true;
            return 1;
        }
    }
    *font = MEMLCD_CAVE_FLOOR;
    int n = memlcd_safe_wrap(s, MEMLCD_CAVE_FLOOR, MEMLCD_CAVE_LABEL_BUDGET, lines, MEMLCD_SW_MAX_LINES);
    *ok = memlcd_cave_lignes_ok(MEMLCD_CAVE_FLOOR, MEMLCD_CAVE_LABEL_BUDGET, lines, n);
    return n;
}

/* A NAME (the browser's copy of an account, never a prompt's label) in at
 * most nmax lines: when it needs more — or a line would not fit its width —
 * the last kept line ends with '~' so a cut is never silent, and a line
 * the cut would leave with digits but no letter is dropped rather than
 * shown ("…4021~" never appears). Returns the lines added. */
static inline uint8_t memlcd_cave_nom(memlcd_cave_vue_t *v, uint8_t y, uint8_t nmax, const char *s)
{
    char lines[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
    uint8_t f;
    bool ok;
    int n = memlcd_cave_label_lignes(s, &f, lines, &ok);
    if (nmax == 0) return 0;
    int garde = n;                                     /* lines kept whole */
    for (int i = 0; i < n; i++)
        if (memlcd_text_width(f, lines[i]) > MEMLCD_CAVE_LABEL_BUDGET) { garde = i; break; }
    bool coupe = garde < n || n > nmax;
    if (coupe) {
        n = garde < n ? garde + 1 : n;                 /* the over-wide line is cut, not dropped */
        if (n > nmax) n = nmax;
        n = memlcd_cave_tilde(f, MEMLCD_CAVE_LABEL_BUDGET, lines, n);
    }
    v->txt_i = v->n; v->txt_n = 0;
    const uint8_t lh = memlcd_font_pas(f);
    for (int i = 0; i < n; i++) {
        if (memlcd_cave_add(v, f, MEMLCD_CAVE_LABEL_X, (uint8_t)(y + i * lh), MEMLCD_CAVE_LABEL_W, false, lines[i]))
            v->txt_n++;
    }
    return (uint8_t)n;
}

/* ── Fit check ───────────────────────────────────────────────────────
 * Every line within its box's width (box - 2 px for kerning), every line
 * and pictogram inside the content box, no two of them overlapping. The
 * layout uses it to pick the roomiest arrangement that holds; the tests
 * re-check the views with their own assertions. */
typedef struct { int x0, y0, x1, y1; } memlcd_cave_boite_t;
static inline bool memlcd_cave_chevauche(memlcd_cave_boite_t a, memlcd_cave_boite_t b)
{
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}
/* The ink extent of a line: its text width, centred or left in its box. */
static inline memlcd_cave_boite_t memlcd_cave_boite_ligne(const memlcd_cave_ligne_t *l)
{
    int w = memlcd_text_width(l->font, l->text);
    int x0 = l->left ? l->x : l->x + (l->w - w) / 2;
    memlcd_cave_boite_t b = { x0, l->y, x0 + w, l->y + memlcd_font_pas(l->font) };
    return b;
}
static inline int memlcd_cave_boites(const memlcd_cave_vue_t *v, memlcd_cave_boite_t *b)
{
    int n = 0;
    if (v->logo)    { b[n].x0 = v->logo_x; b[n].y0 = v->logo_y; b[n].x1 = v->logo_x + v->logo; b[n].y1 = v->logo_y + v->logo; n++; }
    if (v->cadenas) { b[n].x0 = v->cadenas_x; b[n].y0 = v->cadenas_y; b[n].x1 = v->cadenas_x + MEMLCD_CAVE_CADENAS_W; b[n].y1 = v->cadenas_y + MEMLCD_CAVE_CADENAS_H; n++; }
    if (v->goutte)  { b[n].x0 = v->goutte_x; b[n].y0 = v->goutte_y; b[n].x1 = v->goutte_x + MEMLCD_CAVE_GOUTTE_W; b[n].y1 = v->goutte_y + MEMLCD_CAVE_GOUTTE_H; n++; }
    if (v->lien)    { b[n].x0 = v->lien_x; b[n].y0 = v->lien_y; b[n].x1 = v->lien_x + MEMLCD_CAVE_LIEN_W; b[n].y1 = v->lien_y + MEMLCD_CAVE_LIEN_H; n++; }
    if (v->rule)    { b[n].x0 = 8; b[n].y0 = v->rule_y - 1; b[n].x1 = 61; b[n].y1 = v->rule_y + 2; n++; }
    if (v->eau)     { b[n].x0 = v->eau_x; b[n].y0 = v->eau_y; b[n].x1 = v->eau_x + MEMLCD_CAVE_EAU_W; b[n].y1 = v->eau_y + MEMLCD_CAVE_EAU_H + 1 + MEMLCD_CAVE_OMBRE_H; n++; }
    return n;
}
static inline bool memlcd_cave_tient(const memlcd_cave_vue_t *v)
{
    memlcd_cave_boite_t b[MEMLCD_CAVE_LIGNES + 8];
    int n = memlcd_cave_boites(v, b);
    for (uint8_t i = 0; i < v->n; i++) {
        const memlcd_cave_ligne_t *l = &v->l[i];
        if (memlcd_text_width(l->font, l->text) + 2 > l->w) return false;
        if (l->x + l->w > MEMLCD_W) return false;
        b[n++] = memlcd_cave_boite_ligne(l);
    }
    for (int i = 0; i < n; i++) {
        if (b[i].y0 < v->top || b[i].y1 > v->bottom || b[i].x0 < 0 || b[i].x1 > MEMLCD_W) return false;
        for (int j = 0; j < i; j++) if (memlcd_cave_chevauche(b[i], b[j])) return false;
    }
    return true;
}

/* ── The views ───────────────────────────────────────────────────────── */
static inline void memlcd_cave_debut(memlcd_cave_vue_t *v, uint8_t kind, bool thin)
{
    memset(v, 0, sizeof *v);
    v->kind = kind;
    v->rock_thin = thin ? 1 : 0;
    v->top = thin ? MEMLCD_CAVE_TOP_THIN : MEMLCD_CAVE_TOP;
    v->bottom = thin ? MEMLCD_CAVE_BOTTOM_THIN : MEMLCD_CAVE_BOTTOM;
    v->tient = 1;
}

/* The battery reading beside the drop: the percentage ("87%"), "+" while
 * charging, "FULL" once charged (words, not the old "#"), "?" when the
 * voltage is unknown. The TRRS link pictogram takes the charge marker's
 * place: the cable already says the half is fed. */
static inline void memlcd_cave_batt_texte(const memlcd_model_t *m, uint8_t pct, char out[MEMLCD_ETAT_BUF])
{
    uint8_t chg = m->lien_5v ? 0 : m->batt_local_chg;
    if (m->batt_local_dv == 0xFF) snprintf(out, MEMLCD_ETAT_BUF, "?");
    else if (chg == 2)            snprintf(out, MEMLCD_ETAT_BUF, "FULL");
    else                          snprintf(out, MEMLCD_ETAT_BUF, "%u%%%s", pct > 100 ? 100u : (unsigned)pct, chg == 1 ? "+" : "");
}

/* The chest's status rows on the normal screen: ".." while not READY, "?"
 * on a protocol mismatch, the mode word once READY (chest_mode_label: DISK
 * PGP OTP FIDO TOTP, lower case in flight, ERR), "NO CARD" only when the SD
 * card is missing. Returns the rows (0..2). */
static inline int memlcd_cave_coffre_lignes(const memlcd_model_t *m, char out[2][CHEST_MODE_LABEL_BUF + 4])
{
    out[0][0] = out[1][0] = '\0';
    if (!(m->coffre & MEMLCD_COFFRE_PRESENT)) return 0;
    if (m->coffre & MEMLCD_COFFRE_BADVER) { strcpy(out[0], "?"); return 1; }
    if (!(m->coffre & CHEST_STATE_READY)) { strcpy(out[0], ".."); return 1; }
    int n = 0;
    chest_mode_label((chest_mode_state_t)m->coffre_mode_state, m->coffre_mode_active, m->coffre_mode_wanted, out[0]);
    if (out[0][0]) n = 1;
    if (!(m->coffre & CHEST_STATE_SD)) strcpy(out[n++], "NO CARD");
    return n;
}

/* Normal screen and browser (browse = the OATH browser in the lower part,
 * the top status kept): logo + padlock, route, SEEN, drop + reading, then
 * the chest's rows and the layer name with its flags (normal) or the layer
 * name capped at 16 px, "i/N", the name and NO TIME (browse). `etage`
 * picks the room: 0 normal rock + logo, 1 thin rock + logo, 2 thin rock,
 * no logo row. */
static inline void memlcd_cave_statut(const memlcd_model_t *m, uint8_t pct, bool browse, int etage,
                                      memlcd_cave_vue_t *v)
{
    memlcd_cave_debut(v, browse ? MEMLCD_CV_BROWSE : MEMLCD_CV_NORMAL, etage > 0);
    const uint8_t lh = memlcd_font_pas(MEMLCD_F_M12);
    uint8_t y = v->top;
    if (etage < 2) {
        v->logo = MEMLCD_CAVE_LOGO_S; v->logo_x = 2; v->logo_y = y;
        if (m->coffre & MEMLCD_COFFRE_PRESENT) {
            v->cadenas = 1; v->cadenas_x = MEMLCD_W - 2 - MEMLCD_CAVE_CADENAS_W; v->cadenas_y = y;
        }
        y = (uint8_t)(y + MEMLCD_CAVE_LOGO_S + 1);
    }
    memlcd_cave_row(v, MEMLCD_F_M12, y, m->route_rf ? "RADIO" : "USB");
    y = (uint8_t)(y + lh);
    if (m->dongle_vu) { memlcd_cave_row(v, MEMLCD_F_M12, y, "SEEN"); y = (uint8_t)(y + lh); }

    /* drop + reading: one row as tall as the drop */
    v->goutte = 1; v->goutte_x = 2; v->goutte_y = y;
    v->goutte_pct = m->batt_local_dv == 0xFF ? 0 : (pct > 100 ? 100 : pct);
    v->goutte_low = m->batt_niveau ? 1 : 0;
    char bt[MEMLCD_ETAT_BUF];
    memlcd_cave_batt_texte(m, pct, bt);
    const uint8_t tx = 2 + MEMLCD_CAVE_GOUTTE_W + MEMLCD_CAVE_GAP;
    (void)memlcd_cave_add(v, MEMLCD_F_M12, tx, y, (uint8_t)(MEMLCD_W - tx), true, bt);
    if (m->lien_5v) {
        v->lien = 1;
        v->lien_x = (uint8_t)(tx + memlcd_text_width(MEMLCD_F_M12, bt) + MEMLCD_CAVE_GAP);
        v->lien_y = (uint8_t)(y + (MEMLCD_CAVE_GOUTTE_H - MEMLCD_CAVE_LIEN_H) / 2);
    }
    y = (uint8_t)(y + MEMLCD_CAVE_GOUTTE_H + MEMLCD_CAVE_GAP);

    if (!browse) {
        char c[2][CHEST_MODE_LABEL_BUF + 4];
        int nc = memlcd_cave_coffre_lignes(m, c);
        for (int i = 0; i < nc; i++) { memlcd_cave_row(v, MEMLCD_F_M12, y, c[i]); y = (uint8_t)(y + lh); }

        /* flags: one line when "CAPS CW C S L2" fits, else two */
        char e1[MEMLCD_ETAT_BUF], e2[MEMLCD_ETAT_BUF], j[2 * MEMLCD_ETAT_BUF];
        memlcd_ligne_etat(m, e1, e2);
        snprintf(j, sizeof j, "%s%s%s", e1, (e1[0] && e2[0]) ? " " : "", e2);
        int nf = !j[0] ? 0 : (memlcd_text_width(MEMLCD_F_M12, j) <= MEMLCD_W_BUDGET ? 1 : 2);
        uint8_t reserve = (uint8_t)(nf ? nf * lh + MEMLCD_CAVE_GAP : 0);
        uint8_t max_h = (uint8_t)(v->bottom > y + reserve ? v->bottom - y - reserve : 0);
        uint8_t h = memlcd_cave_hero_c(v, y, MEMLCD_F_M32, max_h, m->nom, etage == 2);
        if (!h) { v->tient = 0; return; }
        y = (uint8_t)(y + h + MEMLCD_CAVE_GAP);
        if (nf == 1) memlcd_cave_row(v, MEMLCD_F_M12, y, j);
        else if (nf == 2) { memlcd_cave_row(v, MEMLCD_F_M12, y, e1); memlcd_cave_row(v, MEMLCD_F_M12, (uint8_t)(y + lh), e2); }
    } else {
        bool no_time = !(m->coffre & CHEST_STATE_TIME);
        /* layer name capped at 16 px: the name being browsed is the point */
        uint8_t reserve = (uint8_t)(lh + lh + (no_time ? lh + 3 : 2));
        uint8_t max_h = (uint8_t)(v->bottom > y + reserve ? v->bottom - y - reserve : 0);
        uint8_t h = memlcd_cave_hero_c(v, y, MEMLCD_F_M16, max_h, m->nom, etage == 2);
        if (!h) { v->tient = 0; return; }
        y = (uint8_t)(y + h + MEMLCD_CAVE_GAP);
        char pos[MEMLCD_ETAT_BUF];
        snprintf(pos, sizeof pos, "%u/%u", (unsigned)m->coffre_pos + 1, (unsigned)m->coffre_total);
        memlcd_cave_row(v, MEMLCD_F_M12, y, pos);
        y = (uint8_t)(y + lh);
        uint8_t bas = (uint8_t)(v->bottom - (no_time ? lh + MEMLCD_CAVE_GAP : MEMLCD_CAVE_GAP));
        char lines[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
        uint8_t f;
        bool ok;
        int need = memlcd_cave_label_lignes(m->coffre_nom, &f, lines, &ok);
        uint8_t room = (uint8_t)(bas > y ? (bas - y) / memlcd_font_pas(f) : 0);
        if (need > room && etage < 2) { v->tient = 0; return; }   /* try a roomier arrangement first */
        uint8_t nn = memlcd_cave_nom(v, y, room, m->coffre_nom);
        y = (uint8_t)(y + nn * memlcd_font_pas(f));
        if (no_time) {
            uint8_t ny = (uint8_t)(y + 3);
            if (ny > bas) ny = bas;
            memlcd_cave_row(v, MEMLCD_F_M12, ny, "NO TIME");
        }
    }
    if (v->tient) v->tient = memlcd_cave_tient(v) ? 1 : 0;
}

/* PROMPT, full screen: the op title (at most 20 px), a wavy divider, the
 * CHEST's label WHOLE, "N ACCTS" when more than one account is targeted,
 * "PRESS" at the bottom. Room for the label is made by shrinking the op
 * title rung by rung down to 12 px. Past that — only wide glyphs (34 'W'
 * need 9 lines at 12 px) or a digit run too long to split safely — UNSCII
 * 8, a bitmap font whose every glyph keeps its ink, safe-wrapped over the
 * full width, and as the very last resort a plain 8-character cut: the
 * label is NEVER cut short. */
static inline bool memlcd_cave_prompt_essai(const memlcd_model_t *m, memlcd_cave_vue_t *v, const char *op,
                                            uint8_t op_font, char lines[][MEMLCD_SW_LINE_BUF], int n,
                                            uint8_t lf, bool pleine_largeur)
{
    const bool cpt = m->coffre_op_count > 1;
    const uint8_t lh12 = memlcd_font_pas(MEMLCD_F_M12);
    memlcd_cave_debut(v, MEMLCD_CV_PROMPT, true);
    uint8_t y = (uint8_t)(v->top + MEMLCD_CAVE_GAP);
    uint8_t oh = memlcd_cave_hero(v, y, op_font, 40, op);
    y = (uint8_t)(y + oh + 3);
    v->rule = 1; v->rule_y = y;
    y = (uint8_t)(y + 4);
    uint8_t reserve = (uint8_t)((cpt ? lh12 + 3 : 0) + lh12 + 3);
    uint8_t band = (uint8_t)(v->bottom > y + reserve ? v->bottom - y - reserve : 0);
    const uint8_t pas = memlcd_font_pas(lf);
    if (n * pas > band) return false;
    v->txt_i = v->n; v->txt_n = 0;
    for (int i = 0; i < n; i++) {
        if (memlcd_cave_add(v, lf, pleine_largeur ? 0 : MEMLCD_CAVE_LABEL_X, (uint8_t)(y + i * pas),
                            pleine_largeur ? MEMLCD_W : MEMLCD_CAVE_LABEL_W, false, lines[i]))
            v->txt_n++;
    }
    y = (uint8_t)(y + n * pas + MEMLCD_CAVE_GAP);
    if (cpt) {
        char c[MEMLCD_ETAT_BUF + 4];
        snprintf(c, sizeof c, "%u ACCTS", (unsigned)m->coffre_op_count);
        memlcd_cave_row(v, MEMLCD_F_M12, y, c);
        y = (uint8_t)(y + lh12 + 3);
    }
    uint8_t ancre = (uint8_t)(v->bottom - lh12 - MEMLCD_CAVE_GAP);
    memlcd_cave_row(v, MEMLCD_F_M12, (uint8_t)(y + 4 > ancre ? y + 4 : ancre), "PRESS");
    v->tient = (v->tient && memlcd_cave_tient(v)) ? 1 : 0;
    return v->tient;
}

static inline void memlcd_cave_prompt(const memlcd_model_t *m, memlcd_cave_vue_t *v)
{
    char op[CHEST_LABEL_BUF];
    chest_op_label(m->coffre_op, op);
    char lines[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
    uint8_t f;
    bool ok;
    int n = memlcd_cave_label_lignes(m->coffre_label, &f, lines, &ok);
    if (ok)
        for (int rang = MEMLCD_CAVE_RANG(MEMLCD_F_M20); rang < MEMLCD_CAVE_LADDER_N; rang++)
            if (memlcd_cave_prompt_essai(m, v, op, memlcd_cave_ladder[rang], lines, n, f, false)) return;
    /* UNSCII 8, safe-wrapped over the full width */
    n = memlcd_safe_wrap(m->coffre_label, MEMLCD_F_U8, MEMLCD_W_BUDGET, lines, MEMLCD_SW_MAX_LINES);
    if (memlcd_cave_lignes_ok(MEMLCD_F_U8, MEMLCD_W_BUDGET, lines, n)
        && memlcd_cave_prompt_essai(m, v, op, MEMLCD_F_M12, lines, n, MEMLCD_F_U8, true)) return;
    /* the very last resort: 8 characters a line, 5 lines for the chest's 34 */
    size_t len = m->coffre_label[0] ? strnlen(m->coffre_label, CHEST_LABEL_MAX) : 0;
    n = 0;
    do {
        size_t p = (size_t)n * 8;
        snprintf(lines[n], MEMLCD_SW_LINE_BUF, "%.*s", (int)(len - p < 8 ? len - p : 8), m->coffre_label + p);
        n++;
    } while ((size_t)n * 8 < len);
    (void)memlcd_cave_prompt_essai(m, v, op, MEMLCD_F_M12, lines, n, MEMLCD_F_U8, true);
    v->coupe_brute = 1;
}
/* The last resort always fits: op at 12 px, divider, 5 UNSCII lines, N ACCTS, PRESS. */
_Static_assert(CHEST_LABEL_MAX <= 5 * 8, "the chest's label must fit 5 UNSCII lines of 8");
_Static_assert(MEMLCD_CAVE_TOP_THIN + MEMLCD_CAVE_GAP + MEMLCD_FW_M12_LINE_H + 3 + 4 + 5 * 11 + MEMLCD_CAVE_GAP
               + (MEMLCD_FW_M12_LINE_H + 3) + MEMLCD_FW_M12_LINE_H + MEMLCD_CAVE_GAP <= MEMLCD_CAVE_BOTTOM_THIN,
               "the UNSCII last resort + N ACCTS + PRESS must always fit the prompt");

/* CODE, full screen: the account name (the browser's copy: <= 2 lines, '~'
 * if cut), the code as two halves (3 + 3 or 4 + 4) in the widest font that
 * fits, the countdown as a draining water bar, "NN s" — the seconds rounded
 * UP, never above 99. Only while coffre_code_visible (the caller checks). */
static inline void memlcd_cave_code(const memlcd_model_t *m, memlcd_cave_vue_t *v)
{
    memlcd_cave_debut(v, MEMLCD_CV_CODE, true);
    uint8_t y = (uint8_t)(v->top + MEMLCD_CAVE_GAP);
    uint8_t nn = memlcd_cave_nom(v, y, 2, m->coffre_nom);
    y = (uint8_t)(y + (nn ? nn * memlcd_font_pas(v->l[v->txt_i].font) : 0) + MEMLCD_CAVE_GAP);
    size_t len = strnlen(m->coffre_code, sizeof m->coffre_code);
    uint8_t half = len == 8 ? 4 : 3;
    char h1[5], h2[5];
    snprintf(h1, sizeof h1, "%.*s", (int)half, m->coffre_code);
    snprintf(h2, sizeof h2, "%.4s", m->coffre_code + (len >= half ? half : len));
    uint8_t f = MEMLCD_F_M12;
    for (int i = 0; i < MEMLCD_CAVE_LADDER_N; i++) {
        f = memlcd_cave_ladder[i];
        if (memlcd_text_width(f, h1) <= MEMLCD_W_BUDGET && memlcd_text_width(f, h2) <= MEMLCD_W_BUDGET) break;
    }
    const uint8_t pas = memlcd_font_pas(f);
    memlcd_cave_row(v, f, y, h1);
    memlcd_cave_row(v, f, (uint8_t)(y + pas), h2);
    y = (uint8_t)(y + 2 * pas + MEMLCD_CAVE_GAP);
    unsigned s = m->coffre_code_secs;
    v->eau = 1; v->eau_x = (MEMLCD_W - MEMLCD_CAVE_EAU_W) / 2; v->eau_y = y;
    v->eau_pct = (uint8_t)(s >= MEMLCD_CODE_FENETRE_S ? 100 : s * 100u / MEMLCD_CODE_FENETRE_S);
    y = (uint8_t)(y + MEMLCD_CAVE_EAU_H + 1 + MEMLCD_CAVE_OMBRE_H + 1);
    char t[MEMLCD_ETAT_BUF];
    snprintf(t, sizeof t, "%u s", s > 99 ? 99u : s);
    memlcd_cave_row(v, MEMLCD_F_M14, y, t);
    v->tient = (v->tient && memlcd_cave_tient(v)) ? 1 : 0;
}

/* SLEEP: the frozen image says it sleeps — the large logo and "zZ", nothing
 * that could pretend to be live. */
static inline void memlcd_cave_veille(memlcd_cave_vue_t *v)
{
    memlcd_cave_debut(v, MEMLCD_CV_SLEEP, false);
    v->logo = MEMLCD_CAVE_LOGO_L;
    v->logo_x = (MEMLCD_W - MEMLCD_CAVE_LOGO_L) / 2;
    v->logo_y = (uint8_t)((MEMLCD_CAVE_TOP + MEMLCD_CAVE_BOTTOM) / 2 - MEMLCD_CAVE_LOGO_L / 2);
    (void)memlcd_cave_add(v, MEMLCD_F_M14, (uint8_t)(v->logo_x + MEMLCD_CAVE_LOGO_L - 22), (uint8_t)(v->logo_y - 17), 22, false, "zZ");
    v->tient = memlcd_cave_tient(v) ? 1 : 0;
}

/* The whole screen: sleep, then prompt > code > browser > normal. */
static inline void memlcd_cave_vue(const memlcd_model_t *m, uint8_t batt_pct, memlcd_cave_vue_t *v)
{
    if (m->veille)                   { memlcd_cave_veille(v); return; }
    if (m->coffre_op)                { memlcd_cave_prompt(m, v); return; }
    if (m->coffre_code_visible)      { memlcd_cave_code(m, v); return; }
    bool browse = m->coffre_browsing != 0;
    for (int etage = browse ? 1 : 0; etage <= 2; etage++) {
        memlcd_cave_statut(m, batt_pct, browse, etage, v);
        if (v->tient) return;
    }
}

/* ── The engine (memlcd_cave.c, LVGL) ────────────────────────────────── */
struct _lv_obj_t;
void memlcd_cave_build(struct _lv_obj_t *scr);
void memlcd_cave_draw(const memlcd_model_t *m, uint8_t batt_pct);
