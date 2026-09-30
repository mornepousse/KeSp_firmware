/* Host renderer harness shared by the "current UI" replica and the three
 * design directions. Two LVGL displays share one lv_init(): the 68x160
 * "panel" display, thresholded exactly like the real flush_cb, and a wide
 * "label" display used only to draw contact-sheet captions in real type. */
#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "common.h"

/* ---- panel display (68x160, thresholded) ---- */
static lv_color_t s_panel_drawbuf[PANEL_W * PANEL_H];
static lv_disp_draw_buf_t s_panel_db;
static lv_disp_drv_t s_panel_drv;
static lv_disp_t *s_panel_disp;
static uint8_t s_ink[PANEL_W * PANEL_H];   /* 1 = ink (black), captured after render */

static void panel_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    for (lv_coord_t y = area->y1; y <= area->y2; y++)
        for (lv_coord_t x = area->x1; x <= area->x2; x++, px++)
            s_ink[y * PANEL_W + x] = lv_color_brightness(*px) < 128 ? 1 : 0;
    lv_disp_flush_ready(drv);
}

/* ---- label display (wide, real anti-aliased type for captions) ---- */
#define LBL_W 420
#define LBL_H 40
static lv_color_t s_lbl_drawbuf[LBL_W * LBL_H];
static lv_disp_draw_buf_t s_lbl_db;
static lv_disp_drv_t s_lbl_drv;
static lv_disp_t *s_lbl_disp;
static uint8_t s_lbl_gray[LBL_W * LBL_H];   /* 0 = black .. 255 = white */

static void lbl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    for (lv_coord_t y = area->y1; y <= area->y2; y++)
        for (lv_coord_t x = area->x1; x <= area->x2; x++, px++)
            s_lbl_gray[y * LBL_W + x] = (uint8_t)lv_color_brightness(*px);
    lv_disp_flush_ready(drv);
}

/* Persistent screens, one per display, created once and reused (lv_obj_clean)
 * rather than created+loaded+deleted per call: deleting the display's
 * CURRENT act_scr without ever loading a replacement first leaves
 * disp->act_scr dangling (lv_scr_load_anim's next lv_scr_act() then derefs
 * freed memory) — found by ASan on the second contact-sheet caption. */
static lv_obj_t *s_panel_scr;
static lv_obj_t *s_lbl_scr;

void sim_init(void)
{
    lv_init();

    lv_disp_draw_buf_init(&s_panel_db, s_panel_drawbuf, NULL, PANEL_W * PANEL_H);
    lv_disp_drv_init(&s_panel_drv);
    s_panel_drv.draw_buf = &s_panel_db;
    s_panel_drv.flush_cb = panel_flush_cb;
    s_panel_drv.hor_res = PANEL_W;
    s_panel_drv.ver_res = PANEL_H;
    s_panel_drv.full_refresh = 1;
    s_panel_disp = lv_disp_drv_register(&s_panel_drv);
    s_panel_scr = lv_disp_get_scr_act(s_panel_disp);   /* the display's own default screen */

    lv_disp_draw_buf_init(&s_lbl_db, s_lbl_drawbuf, NULL, LBL_W * LBL_H);
    lv_disp_drv_init(&s_lbl_drv);
    s_lbl_drv.draw_buf = &s_lbl_db;
    s_lbl_drv.flush_cb = lbl_flush_cb;
    s_lbl_drv.hor_res = LBL_W;
    s_lbl_drv.ver_res = LBL_H;
    s_lbl_drv.full_refresh = 1;
    s_lbl_disp = lv_disp_drv_register(&s_lbl_drv);
    s_lbl_scr = lv_disp_get_scr_act(s_lbl_disp);
}

lv_obj_t *panel_new_screen(void)
{
    lv_obj_t *scr = s_panel_scr;
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scr, PANEL_W, PANEL_H);
    return scr;
}

void panel_capture(void)
{
    lv_refr_now(s_panel_disp);
}

static void write_rgb_png(const char *path, const uint8_t *rgb, int w, int h)
{
    stbi_write_png(path, w, h, 3, rgb, w * 3);
    printf("wrote %s (%dx%d)\n", path, w, h);
}

void panel_save_png(const char *path, int scale)
{
    const int frame = 4;
    int w = PANEL_W * scale + frame * 2, h = PANEL_H * scale + frame * 2;
    uint8_t *out = (uint8_t *)malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int o = (y * w + x) * 3;
            uint8_t v;
            if (x < frame || y < frame || x >= w - frame || y >= h - frame) {
                v = 0xB0;   /* thin mid-grey frame */
            } else {
                int px = (x - frame) / scale, py = (y - frame) / scale;
                v = s_ink[py * PANEL_W + px] ? 0x00 : 0xFF;
            }
            out[o] = out[o + 1] = out[o + 2] = v;
        }
    }
    write_rgb_png(path, out, w, h);
    free(out);
}

/* ---- contact sheet ---- */
static uint8_t *s_sheet;
static int s_sheet_w, s_sheet_h;
static int s_cols, s_rows, s_thumb_scale, s_cell_w, s_cell_h, s_idx;
static const lv_font_t *s_cap_font;
static const int PAD = 10, CAP_H = 42, TITLE_H = 26;

static void blit_gray(int ox, int oy, int w, int h, const uint8_t *gray_src, int src_w)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t v = gray_src[y * src_w + x];
            int o = ((oy + y) * s_sheet_w + (ox + x)) * 3;
            if (o < 0 || o + 2 >= s_sheet_w * s_sheet_h * 3) continue;
            s_sheet[o] = s_sheet[o + 1] = s_sheet[o + 2] = v;
        }
    }
}

static void render_caption(const char *text, int max_w)
{
    lv_obj_t *scr = s_lbl_scr;
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_size(scr, LBL_W, LBL_H);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, s_cap_font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, max_w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 0);
    lv_refr_now(s_lbl_disp);
}

void sheet_begin(int cols, int rows, int thumb_scale, const lv_font_t *cap_font, const char *title)
{
    s_cols = cols; s_rows = rows; s_thumb_scale = thumb_scale; s_cap_font = cap_font; s_idx = 0;
    s_cell_w = PANEL_W * thumb_scale + PAD * 2;
    s_cell_h = PANEL_H * thumb_scale + CAP_H + PAD * 2;
    s_sheet_w = s_cell_w * cols;
    s_sheet_h = s_cell_h * rows + TITLE_H;
    s_sheet = (uint8_t *)malloc((size_t)s_sheet_w * s_sheet_h * 3);
    memset(s_sheet, 0xFF, (size_t)s_sheet_w * s_sheet_h * 3);
    if (title && title[0]) {
        render_caption(title, s_sheet_w - 2 * PAD);
        blit_gray(PAD, 2, LBL_W < s_sheet_w - 2 * PAD ? LBL_W : s_sheet_w - 2 * PAD,
                  TITLE_H - 4 < LBL_H ? TITLE_H - 4 : LBL_H, s_lbl_gray, LBL_W);
    }
}

void sheet_add(const char *caption)
{
    int col = s_idx % s_cols, row = s_idx / s_cols;
    int ox = col * s_cell_w + PAD, oy = TITLE_H + row * s_cell_h + PAD;
    /* thumbnail: nearest-neighbour scale of s_ink into a temp gray buffer, then blit */
    int tw = PANEL_W * s_thumb_scale, th = PANEL_H * s_thumb_scale;
    static uint8_t tmp[400 * 900];   /* generous scratch, larger than any thumb we use */
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++)
            tmp[y * tw + x] = s_ink[(y / s_thumb_scale) * PANEL_W + (x / s_thumb_scale)] ? 0x00 : 0xFF;
    /* thin frame around the thumbnail */
    for (int x = 0; x < tw; x++) { tmp[x] = 0xA0; tmp[(th - 1) * tw + x] = 0xA0; }
    for (int y = 0; y < th; y++) { tmp[y * tw] = 0xA0; tmp[y * tw + tw - 1] = 0xA0; }
    blit_gray(ox, oy, tw, th, tmp, tw);

    render_caption(caption, s_cell_w - 2 * PAD < LBL_W ? s_cell_w - 2 * PAD : LBL_W);
    int cw = s_cell_w - 2 * PAD < LBL_W ? s_cell_w - 2 * PAD : LBL_W;
    int ch = CAP_H < LBL_H ? CAP_H : LBL_H;
    blit_gray(ox, oy + th + 2, cw, ch, s_lbl_gray, LBL_W);
    s_idx++;
}

void sheet_end(const char *path)
{
    write_rgb_png(path, s_sheet, s_sheet_w, s_sheet_h);
    free(s_sheet);
    s_sheet = NULL;
}
