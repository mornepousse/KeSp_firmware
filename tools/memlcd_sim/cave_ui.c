/* Cave engine v2 — see cave_ui.h. Round-2 rewrite after Mae's feedback that
 * round 1 (black background + lv_arc mascot) did not read as a cave. */
#include "cave_ui.h"
#include "chest_proto.h"
#include "safe_wrap.h"
#include <string.h>
#include <stdio.h>

LV_FONT_DECLARE(lv_font_montserrat_8);
LV_FONT_DECLARE(lv_font_montserrat_10);
LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_32);
extern const lv_img_dsc_t img_rock_top, img_rock_bottom, img_dither_small, img_dither_wide;
extern const lv_img_dsc_t img_niphargus_28, img_niphargus_56;

#define BUDGET 64                 /* px, matches the rest of the file's 2px-margin convention */
#define ROCK_TOP_H  12
#define ROCK_BOT_H  9
#define CONTENT_TOP 14
#define CONTENT_BOTTOM (MEMLCD_H - ROCK_BOT_H - 1)   /* 150: never touches the floor's dither */

/* Text-size floor (2026-09-29, punctuation-vanishes bug): check_glyph_ink.c
 * renders every printable ASCII glyph through the SAME 1-bit threshold the
 * panel uses and reports, per font, any glyph that loses all its ink and
 * whether ':' and '.' remain distinguishable. Montserrat 8 fails hard (7
 * glyphs incl. ':' '.' ',' '_' go to zero ink, and ':' == '.' — both
 * empty); Montserrat 10 still drops one glyph (apostrophe). Montserrat 12
 * is the smallest Montserrat rung where EVERY glyph keeps ink and ':' /
 * '.' stay visually distinct — see check_glyph_ink.c's own output for the
 * numbers. Nothing in this file uses a font below it any more. */
static const lv_font_t *const k_ladder[] = {
    &lv_font_montserrat_32, &lv_font_montserrat_28, &lv_font_montserrat_24, &lv_font_montserrat_20,
    &lv_font_montserrat_18, &lv_font_montserrat_16, &lv_font_montserrat_14, &lv_font_montserrat_12,
};
#define K_LADDER_N (int)(sizeof k_ladder / sizeof k_ladder[0])
#define K_LADDER_FLOOR (&lv_font_montserrat_12)

static bool fits(const char *s, const lv_font_t *f, lv_coord_t budget)
{
    return lv_txt_get_width(s, (uint32_t)strlen(s), f, 0, LV_TEXT_FLAG_NONE) <= budget;
}

/* Hero text: the WIDEST font (from the shared 32..12 ladder) that fits the
 * WHOLE string on one line — this is what "RESET's ! must not collide"
 * needs: a single-line guarantee wherever one is possible, not a
 * wrap-then-hope. Ladder search + wrap fallback both live in
 * hero_set_capped below, since below the floor the fallback is a real
 * (LVGL greedy) wrap, not a CLIP truncation — see that function's
 * comment for why ("NAVIGATION" no longer fits one line at the 12px
 * floor). */
/* start_rank lets a cramped view (the browser, which must keep the top
 * status block visible too — see cave_draw) cap the hero at a smaller
 * ceiling than the full 32px: rank 3 = start at Montserrat 20.
 *
 * Floor-wrap fallback (2026-09-29): raising the floor to 12px (see
 * K_LADDER_FLOOR) means the ladder can run out before a word fits ONE
 * line — "NAVIGATION" is 78px wide at 12px against a 66px budget, and
 * there is no smaller size left to try. The old behaviour at that point
 * was to render the SMALLEST ladder size anyway in CLIP mode, silently
 * truncating the string; that is exactly the kind of cut this whole
 * redesign exists to avoid, just relocated from a name/label to a layer
 * name. Since a hero string is NEVER an account name or a code
 * (requirement 3: only label_set's safe_wrap path renders those), it
 * needs none of safe_wrap's digit-strand safety check — wrapping it at
 * the floor with LVGL's own greedy wrap is enough — EXCEPT that LVGL's
 * greedy wrap fills the first line as full as it can regardless of
 * balance, and for a single unbroken word that means whatever is left
 * over: "NAVIGATION" against the object's actual (unmargined) width
 * lands as "NAVIGATIO" / "N" — a single stranded letter that reads like
 * a cut, exactly the class of bug this file exists to avoid, just for a
 * layer name instead of an account label. A balanced split (nearest to
 * the middle, both halves still within budget) avoids that: "NAVIG" /
 * "ATION". No safe_wrap digit-check needed here (see above), just a
 * nicer split than LVGL's own. */
static lv_coord_t hero_set_capped(lv_obj_t *l, lv_coord_t x, lv_coord_t y, lv_coord_t w, const char *s, int start_rank)
{
    lv_coord_t budget = w - 2;
    const lv_font_t *f = NULL;
    for (int i = start_rank; i < K_LADDER_N; i++) if (fits(s, k_ladder[i], budget)) { f = k_ladder[i]; break; }
    lv_obj_set_pos(l, x, y); lv_obj_set_width(l, w);
    if (f) {
        lv_obj_set_style_text_font(l, f, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_label_set_text(l, s);
    } else {
        size_t len = strlen(s);
        size_t best = len / 2;
        lv_coord_t best_diff = 0x7FFF;
        for (size_t i = 1; i < len; i++) {
            lv_coord_t w1 = lv_txt_get_width(s, (uint32_t)i, K_LADDER_FLOOR, 0, LV_TEXT_FLAG_NONE);
            lv_coord_t w2 = lv_txt_get_width(s + i, (uint32_t)(len - i), K_LADDER_FLOOR, 0, LV_TEXT_FLAG_NONE);
            if (w1 > budget || w2 > budget) continue;
            lv_coord_t diff = (lv_coord_t)(w1 > w2 ? w1 - w2 : w2 - w1);
            if (diff < best_diff) { best_diff = diff; best = i; }
        }
        char buf[80];
        snprintf(buf, sizeof buf, "%.*s\n%.*s", (int)best, s, (int)(len - best), s + best);
        lv_obj_set_style_text_font(l, K_LADDER_FLOOR, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);   /* the \n IS the wrap — no further wrapping needed */
        lv_label_set_text(l, buf);
    }
    lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(l);
    return lv_obj_get_height(l);
}
static lv_coord_t hero_set(lv_obj_t *l, lv_coord_t x, lv_coord_t y, lv_coord_t w, const char *s)
{
    return hero_set_capped(l, x, y, w, s, 0);
}

/* Account name/label text: safe_wrap (never a digit-only line — see
 * safe_wrap.h) at the widest of a small font ladder that makes every
 * resulting line fit the budget; falls back to the floor (12px, see
 * K_LADDER_FLOOR) as a last resort rather than ever violate the safety
 * rule OR drop below the text-size floor. NEVER the hero ladder — this is
 * the "different size" half of "the code must be unmistakable next to a
 * name" (round 2, requirement 3). */
static const lv_font_t *const k_label_ladder[] = {
    &lv_font_montserrat_14, &lv_font_montserrat_12,
};
#define K_LABEL_LADDER_N 2

/* Bug found on the round-2 bench render (Mae's follow-up): the old version
 * picked a font by checking "does every WRAPPED line fit its own width" —
 * which a greedy wrap satisfies BY CONSTRUCTION at every font size, so it
 * always stopped at the first (largest, 14px) rung and never tried a
 * smaller one. "BANQUE:4021" wrapped at 14px into "BANQU"/"E:4021" — a
 * mid-word cut — when a smaller size fit the WHOLE label on one line with
 * no wrap needed at all (at the time, 8px; that size no longer exists —
 * see the floor note above K_LADDER, and safe_wrap.h's 2026-09-29
 * revision note for how the mid-word cut itself came back, on purpose,
 * once nothing in the ladder can shrink the label onto one line any more).
 *
 * Fixed strategy, in order:
 *   1. Does the WHOLE string fit at ANY ladder size, no wrap? Use the
 *      LARGEST such size — this is what "no mid-word cut when a smaller
 *      size fits one line" means, and it is checked BEFORE ever wrapping,
 *      not as a fallback.
 *   2. Only if nothing in the ladder fits one line: wrap, at the FLOOR
 *      (12px) — minimises total height and how often a mid-word cut is
 *      even possible, and still runs through safe_wrap's digit-safety
 *      rule (which, at the floor, is where its own mid-word-shift
 *      fallback engages — see safe_wrap.h).
 * `max_h` is the caller's real available band (not a guess): if the
 * result doesn't fit it, this prints a warning that shows up in the
 * render log — the render-time equivalent of an assert, since aborting
 * mid-batch would lose every other state's PNG along with the one at
 * fault. */
#define K_LABEL_POOL_N 8   /* matches cave_ui_t.l_label[8] */

static lv_coord_t label_set(cave_ui_t *u, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t max_h, const char *s)
{
    lv_coord_t budget = w - 2;

    for (int fi = 0; fi < K_LABEL_LADDER_N; fi++) {
        if (!fits(s, k_label_ladder[fi], budget)) continue;
        lv_obj_t *l = u->l_label[0];
        lv_obj_set_style_text_font(l, k_label_ladder[fi], 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(l, x, y); lv_obj_set_width(l, w);
        lv_label_set_text(l, s);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
        lv_coord_t h = lv_font_get_line_height(k_label_ladder[fi]);
        if (h > max_h) fprintf(stderr, "cave_ui label_set: OVERFLOW single line %dpx > band %dpx for \"%s\"\n", (int)h, (int)max_h, s);
        return h;
    }

    const lv_font_t *chosen = k_label_ladder[K_LABEL_LADDER_N - 1];   /* 12px, the floor */
    char lines[SAFE_WRAP_MAX_LINES][SAFE_WRAP_LINE_BUF];
    int n = safe_wrap_lines(s, chosen, budget, lines, SAFE_WRAP_MAX_LINES);
    lv_coord_t lh = lv_font_get_line_height(chosen);
    lv_coord_t total = (lv_coord_t)(n * lh);
    if (total > max_h)
        fprintf(stderr, "cave_ui label_set: OVERFLOW %d lines * %dpx = %dpx > band %dpx for \"%s\"\n",
               n, (int)lh, (int)total, (int)max_h, s);
    if (n > K_LABEL_POOL_N)
        fprintf(stderr, "cave_ui label_set: %d lines needed, only %d widgets in the pool — TRUNCATED for \"%s\"\n",
               n, K_LABEL_POOL_N, s);
    lv_coord_t yy = y;
    for (int i = 0; i < n && i < K_LABEL_POOL_N; i++) {
        lv_obj_t *l = u->l_label[i];
        lv_obj_set_style_text_font(l, chosen, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(l, x, yy); lv_obj_set_width(l, w);
        lv_label_set_text(l, lines[i]);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
        yy += lh;
    }
    return yy - y;
}

static lv_obj_t *mklabel(lv_obj_t *parent, lv_color_t ink)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, ink, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static lv_obj_t *mkimg(lv_obj_t *parent, const lv_img_dsc_t *src, lv_color_t ink)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, src);
    lv_obj_set_style_img_recolor(img, ink, 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

/* Water gauge, vertical (battery): outline "vessel", a solid fill rising
 * from the bottom, a wavy zigzag AT the water's surface (never a flat
 * bar-top), and a Bayer-dithered shadow strip cast on the rock just below
 * the vessel — dither is reserved for stone/shadow, never for the water OR
 * for any text (round 2, requirement 2c). */
/* Round 2 follow-up (Mae, on review): at high/low fill the vessel read as
 * "a solid black rectangle", not water — the 1px zigzag amplitude was lost
 * against the fill's own ink at 4x panel scale, and a square-cornered box
 * around a square-cornered fill has no vessel silhouette of its own. Fixed
 * with a visibly rounded vessel (a real container, not a bar) and a
 * 3px-amplitude wave with SEVEN points (a visible crest+trough, not a
 * single zigzag tooth) drawn with a wider stroke so it survives the
 * threshold at every fill level, including 0% and 100%. */
#define WAVE_AMP 3
static void water_v(lv_obj_t *box, lv_obj_t *fill, lv_obj_t *wave, lv_obj_t *shadow,
                    lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, uint8_t pct, bool thick)
{
    lv_obj_set_pos(box, x, y); lv_obj_set_size(box, w, h);
    lv_obj_set_style_border_width(box, thick ? 2 : 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, (lv_coord_t)(w / 3), LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t inner_h = h - 2;
    lv_coord_t fh = (lv_coord_t)(((int)pct * inner_h + 50) / 100);
    if (fh > inner_h) fh = inner_h; if (fh < 0) fh = 0;
    lv_coord_t wy = (lv_coord_t)(y + 1 + (inner_h - fh));
    lv_obj_set_pos(fill, x + 1, wy);
    lv_obj_set_size(fill, w - 2, fh);
    lv_obj_set_style_radius(fill, (lv_coord_t)(w / 3), LV_PART_MAIN);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_HIDDEN);

    static lv_point_t pts[7];
    lv_coord_t iw = w - 2;
    for (int i = 0; i < 7; i++) {
        pts[i].x = (lv_coord_t)(x + 1 + (iw * i) / 6);
        pts[i].y = (lv_coord_t)(wy + ((i % 2) ? WAVE_AMP : -WAVE_AMP));
    }
    /* clamp the wave inside the vessel even at 0%/100% fill, where its
     * centre sits right on the rim */
    for (int i = 0; i < 7; i++) {
        if (pts[i].y < y + 1) pts[i].y = (lv_coord_t)(y + 1);
        if (pts[i].y > y + h - 2) pts[i].y = (lv_coord_t)(y + h - 2);
    }
    lv_line_set_points(wave, pts, 7);
    lv_obj_clear_flag(wave, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_pos(shadow, x - 1, y + h + 1);
    lv_obj_clear_flag(shadow, LV_OBJ_FLAG_HIDDEN);
}

/* Same idea, horizontal (the TOTP countdown): fills from the left (time
 * elapsed), wavy vertical surface at the draining edge. */
static void water_h(lv_obj_t *box, lv_obj_t *fill, lv_obj_t *wave, lv_obj_t *shadow,
                    lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, uint8_t pct)
{
    lv_obj_set_pos(box, x, y); lv_obj_set_size(box, w, h);
    lv_obj_set_style_radius(box, (lv_coord_t)(h / 3), LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t inner_w = w - 2;
    lv_coord_t fw = (lv_coord_t)(((int)pct * inner_w + 50) / 100);
    if (fw > inner_w) fw = inner_w; if (fw < 0) fw = 0;
    lv_obj_set_pos(fill, x + 1, y + 1);
    lv_obj_set_size(fill, fw, h - 2);
    lv_obj_set_style_radius(fill, (lv_coord_t)(h / 3), LV_PART_MAIN);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_HIDDEN);

    static lv_point_t pts[7];
    lv_coord_t ih = h - 2, wx = (lv_coord_t)(x + 1 + fw);
    for (int i = 0; i < 7; i++) {
        pts[i].x = (lv_coord_t)(wx + ((i % 2) ? WAVE_AMP : -WAVE_AMP));
        pts[i].y = (lv_coord_t)(y + 1 + (ih * i) / 6);
    }
    for (int i = 0; i < 7; i++) {
        if (pts[i].x < x + 1) pts[i].x = (lv_coord_t)(x + 1);
        if (pts[i].x > x + w - 2) pts[i].x = (lv_coord_t)(x + w - 2);
    }
    lv_line_set_points(wave, pts, 7);
    lv_obj_clear_flag(wave, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_pos(shadow, x, y + h + 1);
    lv_obj_clear_flag(shadow, LV_OBJ_FLAG_HIDDEN);
}

void cave_build(cave_ui_t *u, lv_obj_t *scr, lv_color_t ink, lv_color_t paper)
{
    memset(u, 0, sizeof *u);
    u->scr = scr; u->ink = ink; u->paper = paper;

    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, paper, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scr, MEMLCD_W, MEMLCD_H);

    u->rock_top = mkimg(scr, &img_rock_top, ink);
    lv_obj_set_pos(u->rock_top, 0, 0);
    u->rock_bottom = mkimg(scr, &img_rock_bottom, ink);
    lv_obj_set_pos(u->rock_bottom, 0, MEMLCD_H - ROCK_BOT_H);

    u->logo_small = mkimg(scr, &img_niphargus_28, ink);
    lv_obj_add_flag(u->logo_small, LV_OBJ_FLAG_HIDDEN);
    u->logo_large = mkimg(scr, &img_niphargus_56, ink);
    lv_obj_add_flag(u->logo_large, LV_OBJ_FLAG_HIDDEN);

    u->l_route = mklabel(scr, ink); lv_obj_set_style_text_font(u->l_route, &lv_font_montserrat_12, 0);
    u->l_seen  = mklabel(scr, ink); lv_obj_set_style_text_font(u->l_seen, &lv_font_montserrat_10, 0);
    u->l_volt  = mklabel(scr, ink); lv_obj_set_style_text_font(u->l_volt, &lv_font_montserrat_10, 0);
    u->l_zz    = mklabel(scr, ink); lv_obj_set_style_text_font(u->l_zz, &lv_font_montserrat_14, 0);

    for (int i = 0; i < 3; i++) { u->l_big[i] = mklabel(scr, ink); }
    for (int i = 0; i < 6; i++) { u->l_line[i] = mklabel(scr, ink); }
    for (int i = 0; i < 8; i++) { u->l_label[i] = mklabel(scr, ink); }

    u->rule = lv_line_create(scr);
    lv_obj_set_style_line_color(u->rule, ink, 0);
    lv_obj_set_style_line_width(u->rule, 1, 0);
    lv_obj_set_style_line_rounded(u->rule, true, 0);
    lv_obj_add_flag(u->rule, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t **boxes[] = { &u->batt_box, &u->code_box };
    lv_obj_t **fills[] = { &u->batt_fill, &u->code_fill };
    for (int k = 0; k < 2; k++) {
        lv_obj_t *box = lv_obj_create(scr);
        lv_obj_remove_style_all(box);
        lv_obj_set_style_border_color(box, ink, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_bg_color(box, paper, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
        *boxes[k] = box;

        lv_obj_t *fill = lv_obj_create(scr);
        lv_obj_remove_style_all(fill);
        lv_obj_set_style_bg_color(fill, ink, 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
        *fills[k] = fill;
    }
    u->batt_wave = lv_line_create(scr);
    u->code_wave = lv_line_create(scr);
    lv_obj_t *waves[] = { u->batt_wave, u->code_wave };
    for (int k = 0; k < 2; k++) {
        lv_obj_set_style_line_color(waves[k], ink, 0);
        lv_obj_set_style_line_width(waves[k], 2, 0);
        lv_obj_set_style_line_rounded(waves[k], true, 0);
        lv_obj_add_flag(waves[k], LV_OBJ_FLAG_HIDDEN);
    }
    u->batt_shadow = mkimg(scr, &img_dither_small, ink);
    lv_obj_add_flag(u->batt_shadow, LV_OBJ_FLAG_HIDDEN);
    u->code_shadow = mkimg(scr, &img_dither_wide, ink);
    lv_obj_add_flag(u->code_shadow, LV_OBJ_FLAG_HIDDEN);
}

static void hide_all(cave_ui_t *u)
{
    lv_obj_add_flag(u->logo_small, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->logo_large, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->l_route, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->l_seen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->l_volt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->l_zz, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) lv_obj_add_flag(u->l_big[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 6; i++) lv_obj_add_flag(u->l_line[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 8; i++) lv_obj_add_flag(u->l_label[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->rule, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->batt_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->batt_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->batt_wave, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->batt_shadow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->code_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->code_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->code_wave, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(u->code_shadow, LV_OBJ_FLAG_HIDDEN);
}

static void set_text(lv_obj_t *l, const lv_font_t *f, lv_coord_t x, lv_coord_t y, lv_coord_t w, const char *s)
{
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_pos(l, x, y); lv_obj_set_width(l, w);
    lv_label_set_text(l, s);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
}

void cave_draw(cave_ui_t *u, const memlcd_model_t *m, uint8_t batt_pct)
{
    hide_all(u);
    bool sleeping = m->veille;
    bool has_op = !sleeping && m->coffre_op != 0;
    bool has_code = !sleeping && !has_op && m->coffre_code_visible;
    bool has_browse = !sleeping && !has_op && !has_code && m->coffre_browsing;
    bool full = has_op || has_code;
    lv_coord_t g_browse_y = MEMLCD_ZONE_BAS_Y + 2;   /* overwritten below when has_browse */

    if (sleeping) {
        /* "large at rest/sleep": the identity mark, not the status row, is
         * the dominant element while nothing is happening. */
        lv_coord_t lx = (MEMLCD_W - 56) / 2, ly = (CONTENT_TOP + CONTENT_BOTTOM) / 2 - 28;
        lv_obj_set_pos(u->logo_large, lx, ly);
        lv_obj_clear_flag(u->logo_large, LV_OBJ_FLAG_HIDDEN);
        set_text(u->l_zz, &lv_font_montserrat_14, lx + 56 - 20, ly - 14, 20, "zZ");
        return;
    }

    /* Text-size floor room (2026-09-29): raising every ladder's floor to
     * 12px (see K_LADDER_FLOOR) costs real height everywhere text is
     * measured, not just on the prompt — state 7's 34-char label needs 6
     * lines at 12px where it used to take fewer at 8px, and the browser's
     * 2-word names (e.g. "OVH:PERSO") no longer fit their line at 12px
     * either, so they wrap too. The brief explicitly allows reclaiming
     * room by letting the rock recede on screens that need it ("rock
     * edges may shrink on the prompt"): here on both the full-screen
     * prompt/code views AND the browser, which has the exact same
     * long-label problem. Achieved by sliding each rock bitmap partly
     * off-screen (LVGL clips outside 0..MEMLCD_H) rather than a separate
     * smaller asset — visible tooth height drops from 12px to 8px at the
     * top and from 9px to 4px at the bottom: thinner, still a real rocky
     * edge, never gone. Plain normal screens (1-4) keep the full rock —
     * they were never the problem. */
    bool roomy = full || has_browse;
    lv_coord_t content_top = roomy ? 10 : CONTENT_TOP;
    lv_coord_t content_bottom = roomy ? 154 : CONTENT_BOTTOM;
    lv_obj_set_pos(u->rock_top, 0, roomy ? -4 : 0);
    lv_obj_set_pos(u->rock_bottom, 0, roomy ? MEMLCD_H - ROCK_BOT_H + 5 : MEMLCD_H - ROCK_BOT_H);

    if (!full) {
        /* The detail block (logo, route, water battery, voltage) stays up
         * during the browser too (Mae: "state 5 lost the route and
         * battery — keep the top status in the browser (only prompt and
         * code are full screen)").
         *
         * 2026-09-29 restructure: route/SEEN used to sit BESIDE the small
         * logo, in the ~36px column left over from its 28px width — that
         * relied on dropping to 10px/8px ("RADIO" only fits 36px at 10px,
         * not at the 12px floor: 42px). There is no beside-the-logo column
         * wide enough for "RADIO" at the floor (the logo's own width is
         * fixed, and the full remaining column tops out at ~38px). Route
         * and SEEN now get their own full-width rows below the logo
         * instead — 66px of budget is generous for either word at 12px,
         * and it's the same "own line" treatment SEEN already had by
         * design (requirement 6). Voltage moves for the matching reason
         * (requirement 4): "3.9V" at 8px is 18px wide and its '.' has ZERO
         * ink after the panel threshold (check_glyph_ink.c) — at the 12px
         * floor it's legible and keeps its '.', at the cost of the vessel
         * and the number sharing a row instead of the vessel standing
         * alone next to the logo. */
        lv_obj_set_pos(u->logo_small, 2, content_top);
        lv_obj_clear_flag(u->logo_small, LV_OBJ_FLAG_HIDDEN);

        lv_coord_t row_h = lv_font_get_line_height(&lv_font_montserrat_12);   /* 15 */
        lv_coord_t ry = content_top + 29;
        set_text(u->l_route, &lv_font_montserrat_12, 0, ry, MEMLCD_W, m->route_rf ? "RADIO" : "USB");
        ry += row_h;
        if (m->dongle_vu) { set_text(u->l_seen, &lv_font_montserrat_12, 0, ry, MEMLCD_W, "SEEN"); ry += row_h; }

        lv_coord_t batt_h = 14;   /* trimmed for browse's tighter budget (was 18, then 16) */
        water_v(u->batt_box, u->batt_fill, u->batt_wave, u->batt_shadow,
               2, ry, 14, batt_h, batt_pct, m->batt_niveau != 0);
        char volt[16];
        /* "3.4~" read as a typo, not "linked" (round-2 follow-up): the
         * real firmware drops the unit suffix entirely when the TRRS link
         * is up (lien_5v) because the link itself is the tell; a bare "~"
         * here had no such meaning to anyone reading it. Always spell the
         * unit out now — "V", or "+"/"#" while actually charging. */
        if (m->batt_local_dv == 0xFF) snprintf(volt, sizeof volt, "?");
        else snprintf(volt, sizeof volt, "%u.%u%s", m->batt_local_dv / 10, m->batt_local_dv % 10,
                     m->batt_local_chg == 2 ? "#" : (m->batt_local_chg == 1 ? "+" : "V"));
        set_text(u->l_volt, &lv_font_montserrat_12, 22, ry + (lv_coord_t)((batt_h - row_h) / 2) + 1, 44, volt);
        /* nothing left-clipped (round 2 defect #3): x is always >= 0 and
         * every label's box stays inside 0..68 — checked for every call
         * in this function, not just this one that broke in round 1. */

        lv_coord_t y = ry + batt_h + 1;
        if (!has_browse && (m->coffre & MEMLCD_COFFRE_PRESENT)) {
            char mode[CHEST_MODE_LABEL_BUF] = "";
            if (m->coffre & MEMLCD_COFFRE_BADVER) strcpy(mode, "?");
            else if (!(m->coffre & CHEST_STATE_READY)) strcpy(mode, "..");
            else chest_mode_label((chest_mode_state_t)m->coffre_mode_state, m->coffre_mode_active, m->coffre_mode_wanted, mode);
            if (mode[0]) { set_text(u->l_line[0], &lv_font_montserrat_12, 0, y, MEMLCD_W, mode); y += row_h; }
        }

        /* Browsing caps the hero further than before (rank 4 = Montserrat
         * 18, was rank 3 = 20): the extra row cost of moving route/SEEN
         * off the logo's row (above) has to come from somewhere, and the
         * hero is the one element on this screen that was already
         * explicitly a compromise size (never the point of the browse
         * screen — the name being browsed is). */
        y = hero_set_capped(u->l_big[0], 0, y, MEMLCD_W, m->nom, has_browse ? 5 : 0) + y + 2;

        if (!has_browse) {
            char e1[MEMLCD_ETAT_BUF], e2[MEMLCD_ETAT_BUF];
            memlcd_ligne_etat(m, e1, e2);
            char flags[24]; flags[0] = '\0';
            if (e1[0]) strcat(flags, e1);
            if (e2[0]) { if (flags[0]) strcat(flags, " "); strcat(flags, e2); }
            if (flags[0]) set_text(u->l_line[1], &lv_font_montserrat_12, 0, y, MEMLCD_W, flags);
        }
        if (has_browse) g_browse_y = y + 1;
    }

    if (has_browse) {
        lv_coord_t row_h = lv_font_get_line_height(&lv_font_montserrat_12);   /* 15 */
        char pos[16];
        snprintf(pos, sizeof pos, "%u/%u", (unsigned)m->coffre_pos + 1, (unsigned)m->coffre_total);
        lv_coord_t by = g_browse_y;
        set_text(u->l_line[2], &lv_font_montserrat_12, 0, by, MEMLCD_W, pos);
        bool no_time = !(m->coffre & CHEST_STATE_TIME);
        lv_coord_t name_y = by + row_h;
        lv_coord_t max_h = content_bottom - (no_time ? row_h + 2 : 2) - name_y;
        lv_coord_t used = label_set(u, 2, name_y, MEMLCD_W - 4, max_h, m->coffre_nom);
        if (no_time) {
            lv_coord_t ny = name_y + used + 3;
            if (ny > content_bottom - row_h - 2) ny = content_bottom - row_h - 2;
            set_text(u->l_line[5], &lv_font_montserrat_12, 0, ny, MEMLCD_W, "NO TIME");
        }
    }

    if (has_op) {
        char op[CHEST_LABEL_BUF]; chest_op_label(m->coffre_op, op);
        lv_coord_t hy = content_top + 2;
        /* Op title capped at rank 3 (Montserrat 20, was uncapped up to
         * 32): the review that found the punctuation-vanishing bug also
         * found the fix for it needs room, and the op word is the
         * cheapest place to give it back — it is one short word ("TOTP",
         * "RESET!"), not the thing being read carefully on this screen
         * (the chest's own label is). "RESET!" still doesn't fit 20px
         * (70px > 66px budget) and falls to 18px on its own, same ladder
         * logic as always — this is a ceiling, not a fixed size. */
        lv_coord_t hh = hero_set_capped(u->l_big[0], 0, hy, MEMLCD_W, op, 3);
        lv_coord_t y = hy + hh + 3;                 /* gap trimmed 6->3: dynamic, still never collides (defect #4) */

        /* Wave divider, thinner (amplitude 2->1px) and a shorter gap
         * after it (8->4px): reclaims ~7px total for the label band
         * below, which is where a 34-char name (state 7) needs it. */
        static lv_point_t rp[4];
        rp[0].x = 8; rp[0].y = y; rp[1].x = 24; rp[1].y = (lv_coord_t)(y + 1);
        rp[2].x = 44; rp[2].y = (lv_coord_t)(y - 1); rp[3].x = 60; rp[3].y = y;
        lv_line_set_points(u->rule, rp, 4);
        lv_obj_clear_flag(u->rule, LV_OBJ_FLAG_HIDDEN);
        y += 4;

        /* The CHEST's own label — never cut, never digit-only (safe_wrap),
         * and always at the small label ladder: never the hero size, so it
         * can never be mistaken for the code (requirement 3). Reserve the
         * bottom area (N ACCTS + PRESS) BEFORE laying out the label, and
         * pass the real remaining band to label_set — this is what "the
         * whole label must be visible, assert sum of line heights <=
         * available band" (Mae's follow-up) needs: state 7's 34-char label
         * was overflowing under PRESS because the label was sized first,
         * at whatever font trivially fit its own wrapped lines (see
         * label_set's history comment), with no upper bound tying it to
         * what was actually left underneath it. PRESS itself also drops
         * to 12px (was 14, and below the text-size floor is not an option
         * — see K_LADDER_FLOOR), shrinking its own reserve to match. */
        bool cpt = m->coffre_op_count > 1;
        lv_coord_t press_h = lv_font_get_line_height(&lv_font_montserrat_12);   /* 15 */
        lv_coord_t bottom_reserve = (lv_coord_t)((cpt ? press_h + 3 : 0) + press_h + 3);
        lv_coord_t max_h = content_bottom - bottom_reserve - y;
        lv_coord_t used = label_set(u, 2, y, MEMLCD_W - 4, max_h, m->coffre_label);
        y += used + 2;

        if (cpt) {
            char c[16]; snprintf(c, sizeof c, "%u ACCTS", (unsigned)m->coffre_op_count);
            set_text(u->l_line[4], &lv_font_montserrat_12, 0, y, MEMLCD_W, c);
            y += press_h + 3;
        }
        lv_coord_t press_anchor = content_bottom - press_h - 2;
        lv_coord_t press_y = y + 4 > press_anchor ? y + 4 : press_anchor;
        if (press_y > press_anchor) {
            fprintf(stderr, "cave_ui has_op: PRESS pushed past its anchor (%dpx > %dpx) for op \"%s\" label \"%s\" — band too tight\n",
                    (int)press_y, (int)press_anchor, op, m->coffre_label);
            press_y = press_anchor;
        }
        set_text(u->l_line[5], &lv_font_montserrat_12, 0, press_y, MEMLCD_W, "PRESS");
    }

    if (has_code) {
        /* the account name: small ladder, safe-wrapped — distinct from the
         * code below both in size and in having NO water-frame around it
         * (requirement 3: the frame is reserved for a real code). */
        label_set(u, 2, content_top + 2, MEMLCD_W - 4, 22, m->coffre_nom);

        size_t len = strnlen(m->coffre_code, sizeof m->coffre_code);
        uint8_t half = len == 8 ? 4 : 3;
        char h1[5], h2[5];
        snprintf(h1, sizeof h1, "%.*s", (int)half, m->coffre_code);
        snprintf(h2, sizeof h2, "%.4s", m->coffre_code + half);
        set_text(u->l_big[0], &lv_font_montserrat_32, 0, content_top + 26, MEMLCD_W, h1);
        set_text(u->l_big[1], &lv_font_montserrat_32, 0, content_top + 62, MEMLCD_W, h2);

        unsigned s = m->coffre_code_secs;
        uint8_t pct = (uint8_t)(s >= MEMLCD_CODE_FENETRE_S ? 100 : s * 100u / MEMLCD_CODE_FENETRE_S);
        water_h(u->code_box, u->code_fill, u->code_wave, u->code_shadow, 12, content_top + 104, 44, 14, pct);
        char secs[16]; snprintf(secs, sizeof secs, "%u s", s > 99 ? 99u : s);
        set_text(u->l_line[4], &lv_font_montserrat_14, 0, content_top + 122, MEMLCD_W, secs);
    }
}

void cave_draw_battery_demo(cave_ui_t *u, uint8_t pct)
{
    hide_all(u);
    water_v(u->batt_box, u->batt_fill, u->batt_wave, u->batt_shadow, 27, 60, 14, 40, pct, pct <= 15);
    char t[8]; snprintf(t, sizeof t, "%u%%", pct);
    set_text(u->l_line[0], &lv_font_montserrat_14, 0, 106, MEMLCD_W, t);
}

void cave_draw_code_demo(cave_ui_t *u, const char *name, const char *code, uint8_t secs)
{
    hide_all(u);
    label_set(u, 2, CONTENT_TOP + 2, MEMLCD_W - 4, 22, name);
    size_t len = strlen(code);
    uint8_t half = len == 8 ? 4 : 3;
    char h1[5], h2[5];
    snprintf(h1, sizeof h1, "%.*s", (int)half, code);
    snprintf(h2, sizeof h2, "%.4s", code + half);
    set_text(u->l_big[0], &lv_font_montserrat_32, 0, CONTENT_TOP + 26, MEMLCD_W, h1);
    set_text(u->l_big[1], &lv_font_montserrat_32, 0, CONTENT_TOP + 62, MEMLCD_W, h2);
    uint8_t pct = (uint8_t)(secs >= MEMLCD_CODE_FENETRE_S ? 100 : secs * 100u / MEMLCD_CODE_FENETRE_S);
    water_h(u->code_box, u->code_fill, u->code_wave, u->code_shadow, 12, CONTENT_TOP + 104, 44, 14, pct);
    char t[16]; snprintf(t, sizeof t, "%u s", secs);
    set_text(u->l_line[4], &lv_font_montserrat_14, 0, CONTENT_TOP + 122, MEMLCD_W, t);
}
