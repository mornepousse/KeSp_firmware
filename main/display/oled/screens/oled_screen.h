#pragma once
/* oled_screen.h — Common interface for all OLED screens (SSD1306 mono).
 *
 * Mono convention: lv_color_black() = lit pixel (on).
 * The LVGL lock is held by the caller (oled_screens_tick) before build/update/destroy.
 */
#include "lvgl.h"

/* Vtable of an OLED screen. */
typedef struct {
    void (*build)(lv_obj_t *parent);  /* creates the LVGL objects; parent = lv_scr_act() */
    void (*update)(void);             /* refreshes the content (lock already held) */
    void (*destroy)(void);            /* deletes the objects and sets ptrs to NULL */
} oled_screen_t;

/* Creates an LVGL card object (lit border, transparent background, no padding).
 * Must be called with the LVGL lock held. */
lv_obj_t *oled_make_card(lv_obj_t *parent, int x, int y, int w, int h, int radius);
