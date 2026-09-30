#pragma once
#include "lvgl.h"

#define PANEL_W 68
#define PANEL_H 160

/* One-time init: registers the 68x160 "panel" display (thresholded exactly
 * like memlcd_backend.c's flush_cb: lv_color_brightness(px) < 128 = ink)
 * and a wide "label" display used only to render contact-sheet captions in
 * real LVGL type (not thresholded — captions are documentation, not panel
 * content). */
void sim_init(void);

/* Fresh blank 68x160 screen (white bg, no scrollbar), becomes the active
 * screen of the panel display. Caller builds widgets on it, then calls
 * panel_capture(). */
lv_obj_t *panel_new_screen(void);

/* Renders the panel's current screen now and captures the 1-bit-thresholded
 * image into the internal buffer used by panel_save_png / sheet_blit_panel. */
void panel_capture(void);

/* Writes the last captured panel image as PNG, nearest-neighbour upscaled
 * `scale`x with a thin mid-grey frame. */
void panel_save_png(const char *path, int scale);

/* A contact sheet: a grid of `cols` thumbnails (panel images at `thumb_scale`)
 * each with a caption strip (rendered in `cap_font`) below it. Call
 * sheet_begin, then for each state: panel render + panel_capture() +
 * sheet_add(caption), then sheet_end(path). */
void sheet_begin(int cols, int rows, int thumb_scale, const lv_font_t *cap_font, const char *title);
void sheet_add(const char *caption);
void sheet_end(const char *path);
