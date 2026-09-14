/* PLACEHOLDER — remplacé par scripts/gen_logo_memlcd.sh (Task 5). */
#include "lvgl.h"
static const uint8_t img_niphargus_60_map[] = { 0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff, 0x00 };
const lv_img_dsc_t img_niphargus_60 = { .header.cf = LV_IMG_CF_ALPHA_1BIT, .header.always_zero = 0,
    .header.w = 1, .header.h = 1, .data_size = sizeof(img_niphargus_60_map), .data = img_niphargus_60_map };
