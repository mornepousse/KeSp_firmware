#include "gauge_lab.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_32);
extern const lv_img_dsc_t img_rock_top, img_rock_bottom;
extern const lv_img_dsc_t img_niphargus_28;

/* Same ladder + single-line "widest font that fits" rule as
 * cave_ui.c's hero_set (see K_LADDER_FLOOR there) — a one-line hero
 * ("BASE") must never overflow the 66px budget the way an unconditional
 * Montserrat 32 does. This file only ever needs the single-line case
 * (the two-line case, e.g. "NAVIG"/"ATION", is passed in pre-split by
 * the caller, matching what cave_ui.c's own balanced-wrap fallback
 * already produces for that exact string). */
static const lv_font_t *const k_ladder[] = {
    &lv_font_montserrat_32, &lv_font_montserrat_28, &lv_font_montserrat_24, &lv_font_montserrat_20,
    &lv_font_montserrat_18, &lv_font_montserrat_16, &lv_font_montserrat_14, &lv_font_montserrat_12,
};
#define K_LADDER_N (int)(sizeof k_ladder / sizeof k_ladder[0])
#define HERO_BUDGET 66

#define MEMLCD_W 68
#define MEMLCD_H 160
#define ROCK_TOP_H 12
#define ROCK_BOT_H 9
#define CONTENT_TOP 14
#define GAP 2                       /* the hard floor this whole file exists to guarantee: never less. */
#define ROW_H 15                    /* lv_font_montserrat_12 line height */

/* ---- tiny local widget helpers (this file never touches cave_ui.c's
 * persistent-object model — every frame gets a clean screen via
 * panel_new_screen(), so plain create-per-frame is simplest here). ---- */

static lv_obj_t *mklabel(lv_obj_t *parent, lv_color_t ink, const lv_font_t *f, bool center)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, ink, 0);
    lv_obj_set_style_text_align(l, center ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    return l;
}

static lv_obj_t *text(lv_obj_t *scr, lv_color_t ink, const lv_font_t *f, lv_coord_t x, lv_coord_t y, lv_coord_t w, bool center, const char *s)
{
    lv_obj_t *l = mklabel(scr, ink, f, center);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_text(l, s);
    return l;
}

/* widest ladder font whose single-line width fits HERO_BUDGET; falls
 * back to the 12px floor (still CLIP, never smaller — see
 * K_LADDER_FLOOR's rationale in cave_ui.c) if even that doesn't fit,
 * since every string this file passes single-line ("BASE") does. */
static lv_obj_t *hero_set(lv_obj_t *scr, lv_color_t ink, lv_coord_t x, lv_coord_t y, lv_coord_t w, const char *s)
{
    const lv_font_t *f = &lv_font_montserrat_12;
    for (int i = 0; i < K_LADDER_N; i++) {
        if (lv_txt_get_width(s, (uint32_t)strlen(s), k_ladder[i], 0, LV_TEXT_FLAG_NONE) <= HERO_BUDGET) { f = k_ladder[i]; break; }
    }
    lv_obj_t *l = mklabel(scr, ink, f, true);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_text(l, s);
    return l;
}

static lv_obj_t *rect(lv_obj_t *scr, lv_color_t color, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

static lv_obj_t *outline(lv_obj_t *scr, lv_color_t ink, lv_color_t paper, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_coord_t border, lv_coord_t radius)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, paper, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, ink, 0);
    lv_obj_set_style_border_width(o, border, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}


/* ================================================================
 * G3 — water-drop silhouette: pointed top tapering into a round
 * bottom bulb (tip = linear taper to the bulb radius, bulb = a
 * circle's lower half — see the design report for the two-part
 * formula). Filled from the bottom; the top wet row is staggered
 * 0/1px column-by-column for a wavy surface instead of a flat cut.
 * Rendered on an lv_canvas since the shape has no LVGL primitive.
 * Bounding box: 13 x 16.
 * ================================================================ */
#define G3_W 13
#define G3_H 16
#define G3_R 6
#define G3_BULB_TOP (G3_H - 1 - G3_R)   /* 9 */

static lv_coord_t g3_halfwidth(int y)
{
    if (y < G3_BULB_TOP) {
        double hw = (double)G3_R * (double)y / (double)G3_BULB_TOP;
        return (lv_coord_t)(hw + 0.5);
    }
    int dy = y - G3_BULB_TOP;
    double v = (double)(G3_R * G3_R - dy * dy);
    if (v < 0) v = 0;
    return (lv_coord_t)(sqrt(v) + 0.5);
}

static lv_coord_t g3_draw(lv_obj_t *scr, lv_color_t ink, lv_color_t paper, lv_coord_t x, lv_coord_t y, uint8_t pct, bool low, lv_coord_t *w_out)
{
    static lv_color_t buf[G3_W * G3_H];
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, buf, G3_W, G3_H, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(canvas, paper, LV_OPA_COVER);

    lv_coord_t cx = G3_W / 2;
    lv_coord_t fill_rows = (lv_coord_t)(((int)pct * G3_H + 50) / 100);
    lv_coord_t base_water_y = (lv_coord_t)(G3_H - fill_rows);   /* rows >= this are "wet" (before wave stagger) */
    int outline_px = low ? 2 : 1;

    for (int row = 0; row < G3_H; row++) {
        lv_coord_t hw = g3_halfwidth(row);
        if (hw <= 0) { if (row == 0) lv_canvas_set_px(canvas, cx, 0, ink); continue; }
        lv_coord_t xl = (lv_coord_t)(cx - hw), xr = (lv_coord_t)(cx + hw);
        for (lv_coord_t px = xl; px <= xr; px++) {
            bool edge = (px - xl < outline_px) || (xr - px < outline_px);
            lv_color_t c;
            if (edge) {
                c = ink;
            } else {
                /* wavy waterline: stagger the threshold row 0/+1 every
                 * other column, exactly like the 1px zigzag amplitude
                 * used elsewhere in this file's other options. */
                lv_coord_t thresh = (lv_coord_t)(base_water_y + (px % 2));
                c = (row >= thresh) ? ink : paper;
            }
            lv_canvas_set_px(canvas, px, row, c);
        }
    }
    lv_obj_set_pos(canvas, x, y);
    if (w_out) *w_out = G3_W;
    return G3_H;
}

static void rock_frame(lv_obj_t *scr, lv_color_t ink, lv_color_t paper)
{
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, paper, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scr, MEMLCD_W, MEMLCD_H);

    lv_obj_t *top = lv_img_create(scr);
    lv_img_set_src(top, &img_rock_top);
    lv_obj_set_style_img_recolor(top, ink, 0);
    lv_obj_set_style_img_recolor_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_pos(top, 0, 0);

    lv_obj_t *bot = lv_img_create(scr);
    lv_img_set_src(bot, &img_rock_bottom);
    lv_obj_set_style_img_recolor(bot, ink, 0);
    lv_obj_set_style_img_recolor_opa(bot, LV_OPA_COVER, 0);
    lv_obj_set_pos(bot, 0, MEMLCD_H - ROCK_BOT_H);
}

void gauge_lab_standalone(lv_obj_t *scr, lv_color_t ink, lv_color_t paper, uint8_t pct, bool low)
{
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, paper, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scr, MEMLCD_W, MEMLCD_H);

    lv_coord_t w = 0;
    lv_coord_t gy = 64;
    lv_coord_t gx_guess = 27;   /* refined below once we know w */
    lv_coord_t h = g3_draw(scr, ink, paper, gx_guess, gy, pct, low, &w);
    /* re-centre now that w is known (drawing twice is wasteful but this is
     * a one-shot mockup tool, not firmware — clarity over cycles) */
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, paper, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scr, MEMLCD_W, MEMLCD_H);
    lv_coord_t gx = (lv_coord_t)((MEMLCD_W - w) / 2);
    h = g3_draw(scr, ink, paper, gx, gy, pct, low, &w);

    char cap[16];
    if (low) snprintf(cap, sizeof cap, "%u%% LOW", pct);
    else snprintf(cap, sizeof cap, "%u%%", pct);
    text(scr, ink, &lv_font_montserrat_14, 0, (lv_coord_t)(gy + h + 10), MEMLCD_W, true, cap);
}

void gauge_lab_mockup(lv_obj_t *scr, lv_color_t ink, lv_color_t paper,
                       bool radio, bool seen,
                       const char *hero1, const char *hero2, const char *flags,
                       uint8_t pct, uint16_t dv, uint8_t chg, bool low)
{
    rock_frame(scr, ink, paper);

    lv_obj_t *logo = lv_img_create(scr);
    lv_img_set_src(logo, &img_niphargus_28);
    lv_obj_set_style_img_recolor(logo, ink, 0);
    lv_obj_set_style_img_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_pos(logo, 2, CONTENT_TOP);

    lv_coord_t ry = (lv_coord_t)(CONTENT_TOP + 28 + GAP);   /* logo bottom + GAP */
    text(scr, ink, &lv_font_montserrat_12, 0, ry, MEMLCD_W, true, radio ? "RADIO" : "USB");
    ry = (lv_coord_t)(ry + ROW_H + GAP);
    if (seen) {
        text(scr, ink, &lv_font_montserrat_12, 0, ry, MEMLCD_W, true, "SEEN");
        ry = (lv_coord_t)(ry + ROW_H + GAP);
    }

    /* gauge + voltage: one row, gauge left-anchored at x=2, voltage text
     * starts >= GAP px after the gauge's own right edge, both vertically
     * centred against max(gauge height, text line height) so neither
     * pokes out past the other — this row, and the GAP before/after it,
     * is the fix for "vessel touches BASE" / "voltage touches TOTP". */
    lv_coord_t gw = 0;
    lv_coord_t row_h = ROW_H;
    lv_coord_t gh = G3_H;
    if (gh > row_h) row_h = gh;
    lv_coord_t gauge_y = (lv_coord_t)(ry + (row_h - gh) / 2);
    g3_draw(scr, ink, paper, 2, gauge_y, pct, low, &gw);

    char volt[16];
    if (dv == 0xFF) snprintf(volt, sizeof volt, "?");
    else snprintf(volt, sizeof volt, "%u.%u%s", dv / 10, dv % 10, chg == 2 ? "#" : (chg == 1 ? "+" : "V"));
    lv_coord_t volt_x = (lv_coord_t)(2 + gw + GAP);
    lv_coord_t volt_y = (lv_coord_t)(ry + (row_h - ROW_H) / 2);
    text(scr, ink, &lv_font_montserrat_12, volt_x, volt_y, (lv_coord_t)(MEMLCD_W - volt_x), false, volt);

    lv_coord_t y = (lv_coord_t)(ry + row_h + GAP);
    y = (lv_coord_t)(y + 0);   /* hero starts here, GAP already applied above */

    if (hero2) {
        text(scr, ink, &lv_font_montserrat_12, 0, y, MEMLCD_W, true, hero1);
        y = (lv_coord_t)(y + ROW_H);
        text(scr, ink, &lv_font_montserrat_12, 0, y, MEMLCD_W, true, hero2);
        y = (lv_coord_t)(y + ROW_H);
    } else {
        lv_obj_t *l = hero_set(scr, ink, 0, y, MEMLCD_W, hero1);
        lv_obj_update_layout(l);
        y = (lv_coord_t)(y + lv_obj_get_height(l));
    }

    if (flags) {
        y = (lv_coord_t)(y + GAP);
        text(scr, ink, &lv_font_montserrat_12, 0, y, MEMLCD_W, true, flags);
    }
}
