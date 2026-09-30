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
 * resort that keeps a chest label of wide glyphs whole on a prompt (34 'W'
 * cannot fit the band in Montserrat 12).
 *
 * Security rules (hard, spec "Security rules for text"):
 *  - a prompt shows the CHEST's label (coffre_label), never the browser's
 *    copy (coffre_nom), WHOLE, nothing overlapping it — the op title, the
 *    divider and the rock shrink first, UNSCII 8 last;
 *  - a name line never reads as a code: a browsed or code-screen name never
 *    shows a line with a digit and no letter (memlcd_safe_wrap.h, cut with
 *    '~'); a prompt's label, never cut, draws the CONTINUATION MARK before
 *    every line that continues the previous one (spec "Continuation mark",
 *    Mae 2026-09-30) — a line of digits there reads as the rest of a name;
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
#define MEMLCD_CAVE_GOUTTE_W     20      /* water-drop gauge, the status band's (spec "Status icons") */
#define MEMLCD_CAVE_GOUTTE_H     26
#define MEMLCD_CAVE_GOUTTE_STRIDE 3      /* ALPHA_1BIT: ceil(20 / 8) bytes a row */
#define MEMLCD_CAVE_ICONE_W      20      /* route and caps icons: sized to balance the drop */
#define MEMLCD_CAVE_ICONE_H      20
#define MEMLCD_CAVE_ICONE_STRIDE 3
/* The status band: drop at x 2, the route icon 3 px after it, the TRRS ⇆
 * 3 px after that — 2 + 20 + 3 + 20 + 3 + 16 = 64 <= 66. */
#define MEMLCD_CAVE_ROUTE_X      (2 + MEMLCD_CAVE_GOUTTE_W + 3)
#define MEMLCD_CAVE_LIEN_X       (MEMLCD_CAVE_ROUTE_X + MEMLCD_CAVE_ICONE_W + 3)
_Static_assert(MEMLCD_CAVE_LIEN_X + MEMLCD_CAVE_LIEN_W <= MEMLCD_W - 2, "the status band fits the panel");
/* The percentage is shown only when LOW, inside the drop: <= 15 %, the
 * largest 5 % step whose digits fit the drop's dry part ("20" at 20 % is
 * 15 px wide where the dry part leaves 12 — test_goutte_chiffres). */
#define MEMLCD_CAVE_PCT_BAS      15
/* The ink rows of Montserrat 12's digits, "+" and "?" in their line: [3, 12)
 * (line 15, baseline 3, box 9 high at ofs_y 0), and inside their advance —
 * checked on the font by tools/memlcd_sim/check_glyph_ink.c. */
#define MEMLCD_CAVE_M12_ENCRE_HAUT 3
#define MEMLCD_CAVE_M12_ENCRE_BAS  12
/* The layer name: ONE modest size (Mae 2026-09-30, "big but not very
 * important — it can be small as long as it stays readable"). */
#define MEMLCD_CAVE_NOM_F        MEMLCD_F_M14
#define MEMLCD_CAVE_EAU_W        44      /* TOTP countdown water bar */
#define MEMLCD_CAVE_EAU_H        14
#define MEMLCD_CAVE_OMBRE_H      4       /* its dither shadow, under it */

#define MEMLCD_CAVE_GAP          2
/* The continuation mark "↳" (1-bit bitmap drawn by the engine: the built-in
 * fonts have no such glyph) before every line of a prompt's label after the
 * first, centred with its text as one group; its width + gap come off that
 * line's budget (a continuation line of Montserrat 12 has 54 px, UNSCII 8
 * 58 — "RFC6238" is 54). It must never read as a digit ("↳89012" is not
 * "489012"): its 1 px stem rises from the line's top, ABOVE the digits'
 * cap height, toward the line it continues, and turns into an open arrow
 * low on the line — nothing crosses the stem the way a 4's bar does;
 * 6 x 11. */
#define MEMLCD_CAVE_MARQUE_W     6
#define MEMLCD_CAVE_MARQUE_H     11
#define MEMLCD_CAVE_MARQUE_GAP   2
#define MEMLCD_CAVE_MARQUE_PAS   (MEMLCD_CAVE_MARQUE_W + MEMLCD_CAVE_MARQUE_GAP)
#define MEMLCD_CAVE_MARQUES      (MEMLCD_SW_MAX_LINES - 1)   /* at most one per label line but the first */
static const uint8_t memlcd_cave_marque_bits[MEMLCD_CAVE_MARQUE_H] = {   /* ALPHA_1BIT, MSB = leftmost */
    0x80,   /* X.....  */
    0x80,   /* X.....  */
    0x80,   /* X.....  */
    0x80,   /* X.....  */
    0x80,   /* X.....  */
    0x80,   /* X.....  */
    0x90,   /* X..X..  */
    0x88,   /* X...X.  */
    0xFC,   /* XXXXXX  */
    0x08,   /* ....X.  */
    0x10,   /* ...X..  */
};
/* The mark's top below its line's top: 0 — the stem starts at the line's
 * top (Montserrat 12's digits span rows 3..11, the arrow on row 8; UNSCII
 * 8's rows 0..7, the arrow on its baseline). */
static inline int memlcd_cave_marque_dy(uint8_t font) { (void)font; return 0; }
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
    uint8_t marque;                /* 1 = the continuation mark at (x - MARQUE_PAS, y + marque_dy):
                                    * left-aligned text after it (a prompt label's line > 0) */
    uint8_t dedans;                /* 1 = the low percentage / "?" INSIDE the drop (its dry part,
                                    * memlcd_goutte_texte_ok): not an overlap */
    char    text[MEMLCD_VC_TXT];
} memlcd_cave_ligne_t;

typedef struct {
    uint8_t kind;                  /* memlcd_cave_kind_t */
    uint8_t rock_thin;
    uint8_t top, bottom;           /* the content box: [top, bottom) */
    uint8_t logo, logo_x, logo_y;  /* 0, MEMLCD_CAVE_LOGO_S or MEMLCD_CAVE_LOGO_L */
    uint8_t cadenas, cadenas_x, cadenas_y;
    uint8_t goutte, goutte_x, goutte_y, goutte_pct, goutte_low, goutte_plus;
    uint8_t route, route_x, route_y;          /* memlcd_icone_t, 0 = none */
    uint8_t lien, lien_x, lien_y;
    uint8_t caps_lock, caps_word, caps_lock_x, caps_word_x, caps_y;
    uint8_t rule, rule_y;          /* prompt: the wavy divider, ink on rule_y - 1 .. rule_y + 1 */
    uint8_t eau, eau_x, eau_y, eau_pct;   /* code: the countdown bar + its shadow */
    uint8_t n;                     /* lines used */
    uint8_t txt_i, txt_n;          /* the lines holding the label (prompt) or the name (code, browse) */
    uint8_t tient;                 /* 1 = memlcd_cave_tient() held for the chosen layout */
    memlcd_cave_ligne_t l[MEMLCD_CAVE_LIGNES];
} memlcd_cave_vue_t;

/* ── The water drop (status band, settled 2026-09-30) ────────────────
 * 20 x 26: variant V2 of the icon round — the G3 drop's profile (a tip
 * tapering into a round bulb) resampled to 20 x 26, the left edge of each
 * row below (the drop is symmetric: right edge = 19 - left), 0xFF = no ink
 * on that row. Filled from the bottom — the top wet row staggered 0/1 px
 * column by column for a wavy surface — outlined 1 px, 2 px when the
 * battery is LOW; the outline is a closed contour (a pixel of the drop with
 * a 4-neighbour outside, within the outline's width). */
static const uint8_t memlcd_goutte_xl[MEMLCD_CAVE_GOUTTE_H] = {
    9, 8, 7, 7, 7, 6, 5, 4, 4, 4, 3, 2, 1, 1, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 0xFF,
};
typedef enum { MEMLCD_GC_DEHORS, MEMLCD_GC_BORD, MEMLCD_GC_EAU, MEMLCD_GC_SEC } memlcd_goutte_classe_t;

/* Wet rows for a percentage: 0 % -> 0, 100 % -> 26, rounded. */
static inline uint8_t memlcd_goutte_rangs(uint8_t pct)
{
    unsigned r = ((unsigned)(pct > 100 ? 100 : pct) * MEMLCD_CAVE_GOUTTE_H + 50) / 100;
    return (uint8_t)r;
}
static inline bool memlcd_goutte_dans(int x, int y)
{
    if (x < 0 || y < 0 || x >= MEMLCD_CAVE_GOUTTE_W || y >= MEMLCD_CAVE_GOUTTE_H) return false;
    const int xl = memlcd_goutte_xl[y];
    return xl != 0xFF && x >= xl && x <= MEMLCD_CAVE_GOUTTE_W - 1 - xl;
}
/* What (x, y) of the drop is: outside, outline, water or dry inside. */
static inline uint8_t memlcd_goutte_classe(uint8_t pct, bool low, int x, int y)
{
    if (!memlcd_goutte_dans(x, y)) return MEMLCD_GC_DEHORS;
    for (int k = 1; k <= (low ? 2 : 1); k++)
        if (!memlcd_goutte_dans(x - k, y) || !memlcd_goutte_dans(x + k, y) ||
            !memlcd_goutte_dans(x, y - k) || !memlcd_goutte_dans(x, y + k)) return MEMLCD_GC_BORD;
    const int seuil = MEMLCD_CAVE_GOUTTE_H - memlcd_goutte_rangs(pct) + (x % 2);
    return y >= seuil ? MEMLCD_GC_EAU : MEMLCD_GC_SEC;
}

/* The charge mark "+" (USB power, charging): 8 x 8, strokes 2 px, on the
 * drop's centre columns (6..13), its top row at py. 1 = the plus, 2 = its
 * ring (a 4-neighbour of the plus), 0 = neither. */
static inline int memlcd_goutte_plus_forme(int x, int y, int py)
{
    #define MEMLCD_PLUS_EN(xx, yy) ((((yy) - py) >= 0 && ((yy) - py) <= 7 && (xx) >= 9 && (xx) <= 10) || \
                                    (((yy) - py) >= 3 && ((yy) - py) <= 4 && (xx) >= 6 && (xx) <= 13))
    if (MEMLCD_PLUS_EN(x, y)) return 1;
    if (MEMLCD_PLUS_EN(x - 1, y) || MEMLCD_PLUS_EN(x + 1, y) || MEMLCD_PLUS_EN(x, y - 1) || MEMLCD_PLUS_EN(x, y + 1)) return 2;
    return 0;
    #undef MEMLCD_PLUS_EN
}
/* Where the "+" goes so it READS: the lowest place where it sits whole in
 * the dry part with a dry ring (ink, *creux = false), or whole in the water
 * with an ink ring (cut out of it, *creux = true); never half and half. If
 * no place holds, the belly, cut out with its ring forced to ink. */
#define MEMLCD_GOUTTE_PLUS_VENTRE 13
static inline uint8_t memlcd_goutte_plus_y(uint8_t pct, bool low, bool *creux)
{
    for (int py = MEMLCD_CAVE_GOUTTE_H - 9; py >= 1; py--) {
        bool sec = true, eau = true;
        for (int y = py - 1; y <= py + 8 && (sec || eau); y++)
            for (int x = 5; x <= 14 && (sec || eau); x++) {
                int f = memlcd_goutte_plus_forme(x, y, py);
                if (!f) continue;
                int c = memlcd_goutte_classe(pct, low, x, y);
                if (c != MEMLCD_GC_SEC) sec = false;
                if (f == 1 ? c != MEMLCD_GC_EAU : (c != MEMLCD_GC_EAU && c != MEMLCD_GC_BORD)) eau = false;
            }
        if (sec || eau) { *creux = !sec; return (uint8_t)py; }
    }
    *creux = true;
    return MEMLCD_GOUTTE_PLUS_VENTRE;
}
static inline bool memlcd_goutte_px_a(uint8_t pct, bool low, int plus_y, bool creux, int x, int y)
{
    int c = memlcd_goutte_classe(pct, low, x, y);
    if (c == MEMLCD_GC_DEHORS) return false;
    bool encre = c == MEMLCD_GC_BORD || c == MEMLCD_GC_EAU;
    if (plus_y >= 0) {
        int f = memlcd_goutte_plus_forme(x, y, plus_y);
        if (f == 1) encre = !creux;
        else if (f == 2) encre = creux;
    }
    return encre;
}
/* 1 = ink at (x, y) of the drop; plus = the charge mark in it. */
static inline bool memlcd_goutte_px(uint8_t pct, bool low, bool plus, int x, int y)
{
    bool creux = false;
    int py = plus ? memlcd_goutte_plus_y(pct, low, &creux) : -1;
    return memlcd_goutte_px_a(pct, low, py, creux, x, y);
}
/* The drop as an LV_IMG_CF_ALPHA_1BIT bitmap (MSB = leftmost pixel). */
static inline void memlcd_goutte_bitmap(uint8_t pct, bool low, bool plus,
                                        uint8_t out[MEMLCD_CAVE_GOUTTE_H * MEMLCD_CAVE_GOUTTE_STRIDE])
{
    bool creux = false;
    int py = plus ? memlcd_goutte_plus_y(pct, low, &creux) : -1;
    memset(out, 0, MEMLCD_CAVE_GOUTTE_H * MEMLCD_CAVE_GOUTTE_STRIDE);
    for (int y = 0; y < MEMLCD_CAVE_GOUTTE_H; y++)
        for (int x = 0; x < MEMLCD_CAVE_GOUTTE_W; x++)
            if (memlcd_goutte_px_a(pct, low, py, creux, x, y))
                out[y * MEMLCD_CAVE_GOUTTE_STRIDE + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

/* A Montserrat 12 reading of width w, its line's top at row yt of the drop,
 * centred at x0: its ink box [x0, x0 + w) x [yt + 3, yt + 12) and every
 * 4-neighbour of it in the DRY part — 1 px of air off the outline and the
 * water. */
static inline bool memlcd_goutte_texte_ok(uint8_t pct, bool low, int x0, int w, int yt)
{
    const int y0 = yt + MEMLCD_CAVE_M12_ENCRE_HAUT, y1 = yt + MEMLCD_CAVE_M12_ENCRE_BAS;   /* [y0, y1) */
    for (int y = y0 - 1; y <= y1; y++)
        for (int x = x0 - 1; x <= x0 + w; x++) {
            if ((y == y0 - 1 || y == y1) && (x == x0 - 1 || x == x0 + w)) continue;   /* no corners */
            if (memlcd_goutte_classe(pct, low, x, y) != MEMLCD_GC_SEC) return false;
        }
    return true;
}
/* The lowest line top (the bulb is widest low) where a reading of width w
 * fits, its line box ending by the band's gap under the drop. */
static inline bool memlcd_goutte_texte_y(uint8_t pct, bool low, uint8_t w, uint8_t *yt)
{
    const int x0 = (MEMLCD_CAVE_GOUTTE_W - w) / 2;
    for (int y = MEMLCD_CAVE_GOUTTE_H + MEMLCD_CAVE_GAP - MEMLCD_FW_M12_LINE_H; y >= 0; y--)
        if (memlcd_goutte_texte_ok(pct, low, x0, w, y)) { *yt = (uint8_t)y; return true; }
    return false;
}

/* ── The icons (20 x 20, 1-bit) ──────────────────────────────────────
 * The route beside the drop — the USB plug, or the radio waves: 3 arcs over
 * a FILLED dot when the dongle has seen us, 2 arcs over a HOLLOW dot when not
 * (no words: "USB", "RADIO", "SEEN" are gone) — and the caps flags under the
 * layer name: Caps Lock = the classic ⇪ (a hollow arrow over a bar), Caps
 * Word = the same arrow without the bar. */
typedef enum {
    MEMLCD_ICONE_AUCUNE, MEMLCD_ICONE_USB, MEMLCD_ICONE_RADIO_VU, MEMLCD_ICONE_RADIO_SEUL,
    MEMLCD_ICONE_CAPS_LOCK, MEMLCD_ICONE_CAPS_WORD,
} memlcd_icone_t;

static inline uint8_t memlcd_cave_route_icone(const memlcd_model_t *m)
{
    if (!m->route_rf) return MEMLCD_ICONE_USB;
    return m->dongle_vu ? MEMLCD_ICONE_RADIO_VU : MEMLCD_ICONE_RADIO_SEUL;
}
/* The radio: in half-pixels around (10, 18) — the dot r 2 (hollow: 1..2),
 * arcs of radius 5.5 / 9 / 12.5, ~1.5 px thick, within 35..145 degrees. */
static inline bool memlcd_icone_radio(int x, int y, bool vu)
{
    const int X = 2 * x + 1 - 20, Y = 36 - (2 * y + 1), d2 = X * X + Y * Y;
    if (d2 <= 16) return vu || d2 >= 4;
    if (Y <= 0 || 1000 * Y < 700 * (X < 0 ? -X : X)) return false;
    static const int r2[3] = { 11, 18, 25 };
    for (int i = 0; i < (vu ? 3 : 2); i++)
        if (d2 >= (r2[i] - 2) * (r2[i] - 2) && d2 <= (r2[i] + 1) * (r2[i] + 1)) return true;
    return false;
}
/* The plug: two prongs, a body outlined 2 px with rounded corners, the cable. */
static inline bool memlcd_icone_usb(int x, int y)
{
    if (y >= 1 && y <= 5 && ((x >= 6 && x <= 7) || (x >= 12 && x <= 13))) return true;
    if (y >= 6 && y <= 13 && x >= 3 && x <= 16) {
        if ((x == 3 || x == 16) && (y == 6 || y == 13)) return false;
        return x <= 4 || x >= 15 || y <= 7 || y >= 12;
    }
    return y >= 14 && y <= 18 && x >= 9 && x <= 10;
}
/* The caps arrow: a head (rows 1..9, 1 px wider a row) on a stem (x 6..13,
 * rows 10..14), outlined 2 px across, 1 px down; Caps Lock adds its bar. */
static inline bool memlcd_icone_fleche_dans(int x, int y)
{
    if (y >= 1 && y <= 9) return x >= 10 - y && x <= 9 + y;
    return y >= 10 && y <= 14 && x >= 6 && x <= 13;
}
static inline bool memlcd_icone_caps(int x, int y, bool lock)
{
    if (lock && y >= 17 && y <= 18 && x >= 6 && x <= 13) return true;
    if (!memlcd_icone_fleche_dans(x, y)) return false;
    return !memlcd_icone_fleche_dans(x - 1, y) || !memlcd_icone_fleche_dans(x + 1, y) ||
           !memlcd_icone_fleche_dans(x - 2, y) || !memlcd_icone_fleche_dans(x + 2, y) ||
           !memlcd_icone_fleche_dans(x, y - 1) || !memlcd_icone_fleche_dans(x, y + 1);
}
static inline bool memlcd_icone_px(uint8_t icone, int x, int y)
{
    if (x < 0 || y < 0 || x >= MEMLCD_CAVE_ICONE_W || y >= MEMLCD_CAVE_ICONE_H) return false;
    switch (icone) {
    case MEMLCD_ICONE_USB:        return memlcd_icone_usb(x, y);
    case MEMLCD_ICONE_RADIO_VU:   return memlcd_icone_radio(x, y, true);
    case MEMLCD_ICONE_RADIO_SEUL: return memlcd_icone_radio(x, y, false);
    case MEMLCD_ICONE_CAPS_LOCK:  return memlcd_icone_caps(x, y, true);
    case MEMLCD_ICONE_CAPS_WORD:  return memlcd_icone_caps(x, y, false);
    default:                      return false;
    }
}
static inline void memlcd_icone_bitmap(uint8_t icone, uint8_t out[MEMLCD_CAVE_ICONE_H * MEMLCD_CAVE_ICONE_STRIDE])
{
    memset(out, 0, MEMLCD_CAVE_ICONE_H * MEMLCD_CAVE_ICONE_STRIDE);
    for (int y = 0; y < MEMLCD_CAVE_ICONE_H; y++)
        for (int x = 0; x < MEMLCD_CAVE_ICONE_W; x++)
            if (memlcd_icone_px(icone, x, y))
                out[y * MEMLCD_CAVE_ICONE_STRIDE + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

/* ── Building blocks ─────────────────────────────────────────────────── */
static inline memlcd_cave_ligne_t *memlcd_cave_add(memlcd_cave_vue_t *v, uint8_t font, uint8_t x, uint8_t y,
                                                   uint8_t w, bool left, const char *s)
{
    if (v->n >= MEMLCD_CAVE_LIGNES) { v->tient = 0; return NULL; }
    memlcd_cave_ligne_t *l = &v->l[v->n++];
    l->font = font; l->x = x; l->y = y; l->w = w; l->left = left ? 1 : 0; l->marque = 0; l->dedans = 0;
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

/* A text in ONE font f (the layer name at its fixed size, a hero's last
 * rung): the whole string on one line within MEMLCD_W_BUDGET and max_h,
 * else two balanced lines ("NAVIG" / "ATION", never "NAVIGATIO" / "N"),
 * else the safe wrap; `coupe` lets the last arrangement cut it with a '~'.
 * Returns the height used, 0 when it does not fit max_h (nothing added). */
static inline uint8_t memlcd_cave_texte_f(memlcd_cave_vue_t *v, uint8_t y, uint8_t f, uint8_t max_h,
                                          const char *s, bool coupe)
{
    const uint8_t lh = memlcd_font_pas(f);
    if (memlcd_text_width(f, s) <= MEMLCD_W_BUDGET && lh <= max_h) {
        memlcd_cave_row(v, f, y, s);
        return lh;
    }
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
/* Hero text (the op title): the WIDEST ladder font, from `plafond` down,
 * holding the whole string on one line within MEMLCD_W_BUDGET and max_h;
 * below that, the floor font through memlcd_cave_texte_f. A hero is never a
 * name or a code, but the safe wrap costs nothing. */
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
    return memlcd_cave_texte_f(v, y, MEMLCD_CAVE_FLOOR, max_h, s, coupe);
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
    for (uint8_t i = 0; i < v->n; i++) {             /* the continuation marks */
        const memlcd_cave_ligne_t *l = &v->l[i];
        if (!l->marque) continue;
        b[n].x0 = l->x - MEMLCD_CAVE_MARQUE_PAS; b[n].y0 = l->y + memlcd_cave_marque_dy(l->font);
        b[n].x1 = b[n].x0 + MEMLCD_CAVE_MARQUE_W; b[n].y1 = b[n].y0 + MEMLCD_CAVE_MARQUE_H;
        n++;
    }
    if (v->logo)    { b[n].x0 = v->logo_x; b[n].y0 = v->logo_y; b[n].x1 = v->logo_x + v->logo; b[n].y1 = v->logo_y + v->logo; n++; }
    if (v->cadenas) { b[n].x0 = v->cadenas_x; b[n].y0 = v->cadenas_y; b[n].x1 = v->cadenas_x + MEMLCD_CAVE_CADENAS_W; b[n].y1 = v->cadenas_y + MEMLCD_CAVE_CADENAS_H; n++; }
    if (v->goutte)  { b[n].x0 = v->goutte_x; b[n].y0 = v->goutte_y; b[n].x1 = v->goutte_x + MEMLCD_CAVE_GOUTTE_W; b[n].y1 = v->goutte_y + MEMLCD_CAVE_GOUTTE_H; n++; }
    if (v->route)   { b[n].x0 = v->route_x; b[n].y0 = v->route_y; b[n].x1 = v->route_x + MEMLCD_CAVE_ICONE_W; b[n].y1 = v->route_y + MEMLCD_CAVE_ICONE_H; n++; }
    if (v->lien)    { b[n].x0 = v->lien_x; b[n].y0 = v->lien_y; b[n].x1 = v->lien_x + MEMLCD_CAVE_LIEN_W; b[n].y1 = v->lien_y + MEMLCD_CAVE_LIEN_H; n++; }
    if (v->caps_lock) { b[n].x0 = v->caps_lock_x; b[n].y0 = v->caps_y; b[n].x1 = v->caps_lock_x + MEMLCD_CAVE_ICONE_W; b[n].y1 = v->caps_y + MEMLCD_CAVE_ICONE_H; n++; }
    if (v->caps_word) { b[n].x0 = v->caps_word_x; b[n].y0 = v->caps_y; b[n].x1 = v->caps_word_x + MEMLCD_CAVE_ICONE_W; b[n].y1 = v->caps_y + MEMLCD_CAVE_ICONE_H; n++; }
    if (v->rule)    { b[n].x0 = 8; b[n].y0 = v->rule_y - 1; b[n].x1 = 61; b[n].y1 = v->rule_y + 2; n++; }
    if (v->eau)     { b[n].x0 = v->eau_x; b[n].y0 = v->eau_y; b[n].x1 = v->eau_x + MEMLCD_CAVE_EAU_W; b[n].y1 = v->eau_y + MEMLCD_CAVE_EAU_H + 1 + MEMLCD_CAVE_OMBRE_H; n++; }
    return n;
}
static inline bool memlcd_cave_tient(const memlcd_cave_vue_t *v)
{
    memlcd_cave_boite_t b[2 * MEMLCD_CAVE_LIGNES + 8];
    int n = memlcd_cave_boites(v, b);
    for (uint8_t i = 0; i < v->n; i++) {
        const memlcd_cave_ligne_t *l = &v->l[i];
        if (memlcd_text_width(l->font, l->text) + 2 > l->w) return false;
        if (l->x + l->w > MEMLCD_W) return false;
        if (l->dedans) {       /* inside the drop: its dry part, checked where it was placed */
            if (!v->goutte) return false;
            continue;
        }
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

/* What the drop says (spec "Status icons", Mae 2026-09-30: "n'afficher le
 * chiffre qu'à l'intérieur quand il est bas"):
 *  - unknown voltage or percentage: an EMPTY drop with "?" inside — an empty
 *    drop alone would read as a dead battery;
 *  - FULL (USB power only, batt_chg_affiche): the drop filled to the tip, no
 *    mark — on USB the only two states are charging and full, so a full drop
 *    without "+" IS full; the word took the room Mae wanted back;
 *  - charging (USB power only): the water at the percentage and a "+" in the
 *    drop (memlcd_goutte_plus_y), no number even when low — plugged in and
 *    charging, there is nothing to act on;
 *  - otherwise the water level, and the percentage INSIDE the drop only at
 *    <= MEMLCD_CAVE_PCT_BAS, digits alone ("15", no "%").
 * The TRRS ⇆ no longer takes the charge mark's place: it has its own spot in
 * the band. */
typedef struct {
    uint8_t remplie;               /* the water level, % */
    uint8_t plus;                  /* the charge mark */
    char    texte[4];              /* inside the drop: "" / "?" / "0".."15" */
} memlcd_cave_jauge_t;
static inline void memlcd_cave_jauge(const memlcd_model_t *m, uint8_t pct, memlcd_cave_jauge_t *j)
{
    memset(j, 0, sizeof *j);
    if (m->batt_local_dv == 0xFF || pct == 0xFF) { j->texte[0] = '?'; return; }
    const uint8_t p = pct > 100 ? 100 : pct;
    if (m->batt_local_chg == 2) { j->remplie = 100; return; }
    j->remplie = p;
    if (m->batt_local_chg == 1) { j->plus = 1; return; }
    if (p <= MEMLCD_CAVE_PCT_BAS) snprintf(j->texte, sizeof j->texte, "%u", (unsigned)p);
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
 * the top status kept): logo + padlock, then the STATUS BAND — the drop,
 * the route icon, the TRRS ⇆ — then the chest's rows, the layer name at its
 * fixed size, the caps icons and the one-shot flags (normal) or the layer
 * name, "i/N", the name and NO TIME (browse). `etage` picks the room: 0
 * normal rock + logo, 1 thin rock + logo, 2 thin rock, no logo row. */
static inline void memlcd_cave_bande(const memlcd_model_t *m, uint8_t pct, memlcd_cave_vue_t *v, uint8_t y)
{
    memlcd_cave_jauge_t j;
    memlcd_cave_jauge(m, pct, &j);
    v->goutte = 1; v->goutte_x = 2; v->goutte_y = y;
    v->goutte_pct = j.remplie; v->goutte_plus = j.plus;
    v->goutte_low = m->batt_niveau ? 1 : 0;
    if (j.texte[0]) {
        const uint8_t w = (uint8_t)memlcd_text_width(MEMLCD_F_M12, j.texte);
        uint8_t yt;
        if (memlcd_goutte_texte_y(v->goutte_pct, v->goutte_low, w, &yt)) {
            memlcd_cave_ligne_t *l = memlcd_cave_add(v, MEMLCD_F_M12, v->goutte_x, (uint8_t)(y + yt),
                                                     MEMLCD_CAVE_GOUTTE_W, false, j.texte);
            if (l) l->dedans = 1;
        }
    }
    v->route = memlcd_cave_route_icone(m);
    v->route_x = MEMLCD_CAVE_ROUTE_X;
    v->route_y = (uint8_t)(y + (MEMLCD_CAVE_GOUTTE_H - MEMLCD_CAVE_ICONE_H) / 2);
    if (m->lien_5v) {
        v->lien = 1; v->lien_x = MEMLCD_CAVE_LIEN_X;
        v->lien_y = (uint8_t)(y + (MEMLCD_CAVE_GOUTTE_H - MEMLCD_CAVE_LIEN_H) / 2);
    }
}

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
    memlcd_cave_bande(m, pct, v, y);
    y = (uint8_t)(y + MEMLCD_CAVE_GOUTTE_H + MEMLCD_CAVE_GAP);

    if (!browse) {
        char c[2][CHEST_MODE_LABEL_BUF + 4];
        int nc = memlcd_cave_coffre_lignes(m, c);
        for (int i = 0; i < nc; i++) { memlcd_cave_row(v, MEMLCD_F_M12, y, c[i]); y = (uint8_t)(y + lh); }

        /* Caps Lock / Caps Word: icons under the name; the one-shots
         * ("CSAG L3", at most 7 characters: one line) stay text under them. */
        char e1[MEMLCD_ETAT_BUF], e2[MEMLCD_ETAT_BUF];
        memlcd_ligne_etat(m, e1, e2);                  /* e1 = CAPS / CW: drawn as icons */
        const int ncaps = (m->caps_lock ? 1 : 0) + (m->caps_word ? 1 : 0);
        const uint8_t hcaps = ncaps ? MEMLCD_CAVE_ICONE_H : 0, hflags = e2[0] ? lh : 0;
        uint8_t reserve = (uint8_t)(hcaps + hflags + (hcaps || hflags ? MEMLCD_CAVE_GAP : 0)
                                    + (hcaps && hflags ? MEMLCD_CAVE_GAP : 0));
        uint8_t max_h = (uint8_t)(v->bottom > y + reserve ? v->bottom - y - reserve : 0);
        uint8_t h = memlcd_cave_texte_f(v, y, MEMLCD_CAVE_NOM_F, max_h, m->nom, etage == 2);
        if (!h) { v->tient = 0; return; }
        y = (uint8_t)(y + h + MEMLCD_CAVE_GAP);
        if (ncaps) {
            uint8_t x = (uint8_t)((MEMLCD_W - (ncaps * MEMLCD_CAVE_ICONE_W + (ncaps - 1) * 3)) / 2);
            v->caps_y = y;
            if (m->caps_lock) { v->caps_lock = 1; v->caps_lock_x = x; x = (uint8_t)(x + MEMLCD_CAVE_ICONE_W + 3); }
            if (m->caps_word) { v->caps_word = 1; v->caps_word_x = x; }
            y = (uint8_t)(y + MEMLCD_CAVE_ICONE_H + MEMLCD_CAVE_GAP);
        }
        if (e2[0]) memlcd_cave_row(v, MEMLCD_F_M12, y, e2);
    } else {
        bool no_time = !(m->coffre & CHEST_STATE_TIME);
        uint8_t reserve = (uint8_t)(lh + lh + (no_time ? lh + 3 : 2));
        uint8_t max_h = (uint8_t)(v->bottom > y + reserve ? v->bottom - y - reserve : 0);
        uint8_t h = memlcd_cave_texte_f(v, y, MEMLCD_CAVE_NOM_F, max_h, m->nom, etage == 2);
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
 * "PRESS" at the bottom. The label: on one line in Montserrat 14 or 12 when
 * it fits, else wrapped at 12 px with the continuation mark before every
 * line after the first (memlcd_safe_wrap_marque: a safe split first; when
 * none exists — "AWS:123456789012" — the marked lines may hold digits
 * alone). Room is made by shrinking the op title rung by rung down to
 * 12 px. Past that — only wide glyphs (34 'W' need 11 lines at 12 px) —
 * UNSCII 8, a bitmap font whose every glyph keeps its ink, wrapped the
 * same way over the full width: it always fits (static asserts below). The
 * label is NEVER cut short. */

/* The prompt's head: the op title, the divider. Returns the y where the
 * label starts, and in *band its height (N ACCTS and PRESS reserved). */
static inline uint8_t memlcd_cave_prompt_tete(const memlcd_model_t *m, memlcd_cave_vue_t *v, const char *op,
                                              uint8_t op_font, uint8_t *band)
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
    *band = (uint8_t)(v->bottom > y + reserve ? v->bottom - y - reserve : 0);
    return y;
}

/* One line of the label in the box [bx, bx + bw): centred like any line;
 * a continuation line's mark and text centred together as one group, the
 * text left-aligned after the mark. */
static inline bool memlcd_cave_label_ligne(memlcd_cave_vue_t *v, uint8_t f, uint8_t bx, uint8_t bw, uint8_t y,
                                           const char *s, bool marque)
{
    if (!marque) return memlcd_cave_add(v, f, bx, y, bw, false, s) != NULL;
    int gw = MEMLCD_CAVE_MARQUE_PAS + memlcd_text_width(f, s);
    int gx = bx + (bw - gw) / 2;
    if (gx < 0) gx = 0;
    int x = gx + MEMLCD_CAVE_MARQUE_PAS;
    memlcd_cave_ligne_t *l = memlcd_cave_add(v, f, (uint8_t)x, y, (uint8_t)(MEMLCD_W - x), true, s);
    if (l) l->marque = 1;
    return l != NULL;
}

static inline bool memlcd_cave_prompt_essai(const memlcd_model_t *m, memlcd_cave_vue_t *v, const char *op,
                                            uint8_t op_font, char lines[][MEMLCD_SW_LINE_BUF], int n,
                                            uint8_t lf, bool pleine_largeur)
{
    const bool cpt = m->coffre_op_count > 1;
    const uint8_t lh12 = memlcd_font_pas(MEMLCD_F_M12);
    uint8_t band;
    uint8_t y = memlcd_cave_prompt_tete(m, v, op, op_font, &band);
    const uint8_t pas = memlcd_font_pas(lf);
    if (n < 1 || n * pas > band) return false;
    const uint8_t bx = pleine_largeur ? 0 : MEMLCD_CAVE_LABEL_X, bw = pleine_largeur ? MEMLCD_W : MEMLCD_CAVE_LABEL_W;
    v->txt_i = v->n; v->txt_n = 0;
    for (int i = 0; i < n; i++)
        if (memlcd_cave_label_ligne(v, lf, bx, bw, (uint8_t)(y + i * pas), lines[i], i > 0)) v->txt_n++;
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
    const char *s = m->coffre_label;
    uint8_t lf = MEMLCD_F_M12, band;
    int n = 0;
    static const uint8_t une[] = { MEMLCD_F_M14, MEMLCD_F_M12 };   /* whole on one line */
    for (size_t i = 0; i < sizeof une && !n; i++)
        if (memlcd_text_width(une[i], s) <= MEMLCD_CAVE_LABEL_BUDGET) {
            lf = une[i];
            snprintf(lines[0], MEMLCD_SW_LINE_BUF, "%.*s", (int)(MEMLCD_SW_LINE_BUF - 1), s);
            n = 1;
        }
    if (!n) {   /* wrapped at 12 px, as many lines as the roomiest head (op at 12 px) leaves */
        (void)memlcd_cave_prompt_tete(m, v, op, MEMLCD_F_M12, &band);
        n = memlcd_safe_wrap_marque(s, MEMLCD_F_M12, MEMLCD_CAVE_LABEL_BUDGET, MEMLCD_CAVE_MARQUE_PAS,
                                    lines, band / memlcd_font_pas(MEMLCD_F_M12));
    }
    if (n)
        for (int rang = MEMLCD_CAVE_RANG(MEMLCD_F_M20); rang < MEMLCD_CAVE_LADDER_N; rang++)
            if (memlcd_cave_prompt_essai(m, v, op, memlcd_cave_ladder[rang], lines, n, lf, false)) return;
    /* UNSCII 8 over the full width, the op at 12 px: always fits */
    (void)memlcd_cave_prompt_tete(m, v, op, MEMLCD_F_M12, &band);
    n = memlcd_safe_wrap_marque(s, MEMLCD_F_U8, MEMLCD_W_BUDGET, MEMLCD_CAVE_MARQUE_PAS,
                                lines, band / memlcd_font_pas(MEMLCD_F_U8));
    if (!memlcd_cave_prompt_essai(m, v, op, MEMLCD_F_M12, lines, n, MEMLCD_F_U8, true)) v->tient = 0;
}
/* The UNSCII wrap always fits: its fewest-lines fallback puts 8 characters
 * on the first line and 7 after each mark, so the chest's 34 need 5 lines
 * at most; 5 of them + N ACCTS + PRESS fit under the op at 12 px. */
_Static_assert(MEMLCD_W_BUDGET / 8 + 4 * ((MEMLCD_W_BUDGET - MEMLCD_CAVE_MARQUE_PAS) / 8) >= CHEST_LABEL_MAX,
               "the chest's label must fit 5 UNSCII lines, the marks included");
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
