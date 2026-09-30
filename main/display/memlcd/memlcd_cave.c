/* The left half's screen, "cave" — the LVGL engine (memlcd_cave.h).
 *
 * Every layout decision is in the pure view (memlcd_cave_vue, tested on
 * host); this file only builds the objects ONCE (memlcd_cave_build, called
 * from memlcd_backend.c's construire() under the LVGL lock) and applies a
 * view to them (memlcd_cave_draw, from dessiner(), under the same lock).
 * No allocation after the build: the view, the drop's bitmap and the line
 * points are static. tools/memlcd_sim compiles THIS file: the mockups are
 * the firmware's drawing.
 *
 * Dark: pale ink on black paper. memlcd_backend.c's flush thresholds at
 * lv_color_brightness < 128 = ink (a black panel pixel): the black paper IS
 * the panel's ink, the white text is the reflective background showing
 * through — the image reads as pale lines in a dark cave. */
#include "memlcd_cave.h"
#include "lvgl.h"

LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_32);
LV_FONT_DECLARE(lv_font_unscii_8);

extern const lv_img_dsc_t img_niphargus_28, img_niphargus_56;
extern const lv_img_dsc_t memlcd_img_rock_top, memlcd_img_rock_bottom, memlcd_img_dither_wide;
extern const lv_img_dsc_t memlcd_img_cadenas, memlcd_img_lien;

/* memlcd_font_t -> the LVGL font. Exported: tools/memlcd_sim's glyph-ink
 * gate checks every font of THIS table through the panel's threshold. */
const lv_font_t *const memlcd_cave_fonts[MEMLCD_F_N] = {
    [MEMLCD_F_M12] = &lv_font_montserrat_12, [MEMLCD_F_M14] = &lv_font_montserrat_14,
    [MEMLCD_F_M16] = &lv_font_montserrat_16, [MEMLCD_F_M18] = &lv_font_montserrat_18,
    [MEMLCD_F_M20] = &lv_font_montserrat_20, [MEMLCD_F_M24] = &lv_font_montserrat_24,
    [MEMLCD_F_M28] = &lv_font_montserrat_28, [MEMLCD_F_M32] = &lv_font_montserrat_32,
    [MEMLCD_F_U8]  = &lv_font_unscii_8,
};
_Static_assert(MEMLCD_F_U8 + 1 == MEMLCD_F_N, "memlcd_cave_fonts covers every memlcd_font_t");

static struct {
    lv_obj_t *rock_top, *rock_bottom;
    lv_obj_t *logo_s, *logo_l, *cadenas, *lien, *goutte;
    lv_obj_t *rule;
    lv_obj_t *eau_box, *eau_fill, *eau_wave, *eau_ombre;
    lv_obj_t *l[MEMLCD_CAVE_LIGNES];
} s;

static uint8_t s_goutte_map[MEMLCD_CAVE_GOUTTE_H * MEMLCD_CAVE_GOUTTE_STRIDE];
static lv_img_dsc_t s_goutte_img = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT, .header.always_zero = 0,
    .header.w = MEMLCD_CAVE_GOUTTE_W, .header.h = MEMLCD_CAVE_GOUTTE_H,
    .data_size = sizeof s_goutte_map, .data = s_goutte_map,
};
static memlcd_cave_vue_t s_vue;                 /* ~600 bytes: static, not on the LVGL task's stack */
static lv_point_t s_rule_pts[4], s_wave_pts[7];

#define INK   lv_color_white()
#define PAPER lv_color_black()

static lv_obj_t *image(lv_obj_t *scr, const lv_img_dsc_t *src)
{
    lv_obj_t *img = lv_img_create(scr);
    lv_img_set_src(img, src);
    lv_obj_set_style_img_recolor(img, INK, 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}
static lv_obj_t *boite(lv_obj_t *scr, bool plein)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, plein ? INK : PAPER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (!plein) { lv_obj_set_style_border_color(o, INK, 0); lv_obj_set_style_border_width(o, 1, 0); }
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}
static lv_obj_t *trait(lv_obj_t *scr, lv_coord_t epaisseur)
{
    lv_obj_t *l = lv_line_create(scr);
    lv_obj_set_style_line_color(l, INK, 0);
    lv_obj_set_style_line_width(l, epaisseur, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

void memlcd_cave_build(lv_obj_t *scr)
{
    memset(&s, 0, sizeof s);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, PAPER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scr, MEMLCD_W, MEMLCD_H);

    s.rock_top = image(scr, &memlcd_img_rock_top);
    s.rock_bottom = image(scr, &memlcd_img_rock_bottom);
    s.logo_s = image(scr, &img_niphargus_28);
    s.logo_l = image(scr, &img_niphargus_56);
    s.cadenas = image(scr, &memlcd_img_cadenas);
    s.lien = image(scr, &memlcd_img_lien);
    s.goutte = image(scr, &s_goutte_img);
    s.rule = trait(scr, 1);
    /* the countdown: a vessel (outline), its water (fill), a wavy surface,
     * and a dithered shadow cast on the rock under it */
    s.eau_box = boite(scr, false);
    lv_obj_set_style_radius(s.eau_box, MEMLCD_CAVE_EAU_H / 3, 0);
    s.eau_fill = boite(scr, true);
    lv_obj_set_style_radius(s.eau_fill, MEMLCD_CAVE_EAU_H / 3, 0);
    s.eau_wave = trait(scr, 2);
    s.eau_ombre = image(scr, &memlcd_img_dither_wide);
    for (int i = 0; i < MEMLCD_CAVE_LIGNES; i++) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_set_style_text_color(l, INK, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_label_set_text(l, "");
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        s.l[i] = l;
    }
}

static void montre(lv_obj_t *o, bool oui, lv_coord_t x, lv_coord_t y)
{
    if (!oui) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_set_pos(o, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* The countdown's water, draining from the right: fill from the left over
 * pct % of the vessel, a 7-point wavy surface at the water's edge. */
static void eau(const memlcd_cave_vue_t *v)
{
    const lv_coord_t x = v->eau_x, y = v->eau_y, w = MEMLCD_CAVE_EAU_W, h = MEMLCD_CAVE_EAU_H;
    montre(s.eau_box, v->eau, x, y);
    montre(s.eau_ombre, v->eau, x, y + h + 1);
    if (!v->eau) { montre(s.eau_fill, false, 0, 0); montre(s.eau_wave, false, 0, 0); return; }
    lv_obj_set_size(s.eau_box, w, h);
    lv_coord_t iw = w - 2, fw = (lv_coord_t)((v->eau_pct * iw + 50) / 100);
    if (fw > iw) fw = iw;
    lv_obj_set_size(s.eau_fill, fw > 0 ? fw : 1, h - 2);
    montre(s.eau_fill, fw > 0, x + 1, y + 1);
    const lv_coord_t amp = 3, wx = x + 1 + fw;
    for (int i = 0; i < 7; i++) {
        lv_coord_t px = (lv_coord_t)(wx + ((i % 2) ? amp : -amp));
        if (px < x + 1) px = x + 1;
        if (px > x + w - 2) px = x + w - 2;
        s_wave_pts[i].x = px;
        s_wave_pts[i].y = (lv_coord_t)(y + 1 + ((h - 2) * i) / 6);
    }
    lv_line_set_points(s.eau_wave, s_wave_pts, 7);
    lv_obj_set_pos(s.eau_wave, 0, 0);
    lv_obj_clear_flag(s.eau_wave, LV_OBJ_FLAG_HIDDEN);
}

void memlcd_cave_draw(const memlcd_model_t *m, uint8_t batt_pct)
{
    /* built, and not cleaned since (show_dfu cleans the screen) */
    if (!s.l[0] || !lv_obj_is_valid(s.l[0]) || !lv_obj_is_valid(s.eau_wave)) return;
    memlcd_cave_vue_t *v = &s_vue;
    memlcd_cave_vue(m, batt_pct, v);

    montre(s.rock_top, true, 0, v->rock_thin ? MEMLCD_CAVE_ROCK_TOP_THIN_Y : 0);
    montre(s.rock_bottom, true, 0, v->rock_thin ? MEMLCD_CAVE_ROCK_BOT_THIN_Y : MEMLCD_H - MEMLCD_CAVE_ROCK_BOT_H);
    montre(s.logo_s, v->logo == MEMLCD_CAVE_LOGO_S, v->logo_x, v->logo_y);
    montre(s.logo_l, v->logo == MEMLCD_CAVE_LOGO_L, v->logo_x, v->logo_y);
    montre(s.cadenas, v->cadenas, v->cadenas_x, v->cadenas_y);
    montre(s.lien, v->lien, v->lien_x, v->lien_y);
    if (v->goutte) {
        memlcd_goutte_bitmap(v->goutte_pct, v->goutte_low, s_goutte_map);
        lv_img_cache_invalidate_src(&s_goutte_img);   /* same descriptor, new pixels */
        lv_img_set_src(s.goutte, &s_goutte_img);
        lv_obj_invalidate(s.goutte);
    }
    montre(s.goutte, v->goutte, v->goutte_x, v->goutte_y);
    if (v->rule) {
        const lv_coord_t y = v->rule_y;
        s_rule_pts[0].x = 8;  s_rule_pts[0].y = y;
        s_rule_pts[1].x = 24; s_rule_pts[1].y = (lv_coord_t)(y + 1);
        s_rule_pts[2].x = 44; s_rule_pts[2].y = (lv_coord_t)(y - 1);
        s_rule_pts[3].x = 60; s_rule_pts[3].y = y;
        lv_line_set_points(s.rule, s_rule_pts, 4);
        lv_obj_set_pos(s.rule, 0, 0);
    }
    montre(s.rule, v->rule, 0, 0);
    eau(v);

    for (int i = 0; i < MEMLCD_CAVE_LIGNES; i++) {
        lv_obj_t *l = s.l[i];
        if (i >= v->n) { lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN); continue; }
        const memlcd_cave_ligne_t *li = &v->l[i];
        lv_obj_set_style_text_font(l, memlcd_cave_fonts[li->font < MEMLCD_F_N ? li->font : MEMLCD_F_M12], 0);
        lv_obj_set_style_text_align(l, li->left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, li->w);
        lv_label_set_text(l, li->text);
        montre(l, true, li->x, li->y);
    }
}
