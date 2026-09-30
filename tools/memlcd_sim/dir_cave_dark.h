#pragma once
#include "lvgl.h"
#include "memlcd_model.h"
#include "cave_ui.h"

/* Cave / Dark: inverted (pale ink on black paper). Same layout as
 * Cave / Light, only the palette differs. */
void dir_cave_dark_build(lv_obj_t *scr);
void dir_cave_dark_draw(const memlcd_model_t *m, uint8_t batt_pct);
cave_ui_t *dir_cave_dark_ui(void);
