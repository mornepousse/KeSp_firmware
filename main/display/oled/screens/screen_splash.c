/* screen_splash.c — SPLASH screen: "KaSe" name + firmware version. */
#include "oled_screen.h"
#include "lvgl.h"
#include "esp_app_desc.h"
#include "board.h"

LV_FONT_DECLARE(lv_font_montserrat_28);

static lv_obj_t *label_name    = NULL;
static lv_obj_t *label_version = NULL;

static void build(lv_obj_t *parent)
{
    /* "KaSe" in large text, centered toward the top (screen 128x64) */
    label_name = lv_label_create(parent);
    lv_obj_set_style_text_font(label_name, &lv_font_montserrat_28, 0);
    lv_label_set_text(label_name, "KaSe");
    lv_obj_align(label_name, LV_ALIGN_CENTER, 0, -12);

    /* Firmware version below, UI_FONT (board-defined) */
    label_version = lv_label_create(parent);
    lv_obj_set_style_text_font(label_version, UI_FONT, 0);
    lv_label_set_text(label_version, esp_app_get_description()->version);
    lv_obj_align(label_version, LV_ALIGN_CENTER, 0, 18);
}

static void update(void) {}

static void destroy(void)
{
    if (label_name && lv_obj_is_valid(label_name)) lv_obj_del(label_name);
    label_name = NULL;
    if (label_version && lv_obj_is_valid(label_version)) lv_obj_del(label_version);
    label_version = NULL;
}

const oled_screen_t screen_splash = { build, update, destroy };
