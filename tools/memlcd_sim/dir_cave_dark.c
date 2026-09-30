#include "dir_cave_dark.h"

static cave_ui_t s_u;

void dir_cave_dark_build(lv_obj_t *scr) { cave_build(&s_u, scr, lv_color_white(), lv_color_black()); }
void dir_cave_dark_draw(const memlcd_model_t *m, uint8_t batt_pct) { cave_draw(&s_u, m, batt_pct); }
cave_ui_t *dir_cave_dark_ui(void) { return &s_u; }
