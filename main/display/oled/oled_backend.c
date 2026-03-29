/* OLED I2C (SSD1306) backend implementation */
#include "display_backend.h"
#include "status_display.h"
#include "board.h"
#include "i2c_oled_display.h"
#include "hid_bluetooth_manager.h"
#include "hid_report.h"
#include "keyboard_config.h"
#include "keymap.h"
#include "version.h"
#include "tama_engine.h"
#include "tama_render.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "assets/img_bluetooth.c"
#include "assets/img_usb.c"
#include "assets/img_signal.c"

LV_FONT_DECLARE(lv_font_montserrat_28);

#ifndef BT_BLINK_INTERVAL_MS
#define BT_BLINK_INTERVAL_MS    500
#endif
#ifndef MOUSE_INDICATOR_MS
#define MOUSE_INDICATOR_MS      200
#endif

static int last_bt_state = -1;
static int last_path_state = -1;
static lv_obj_t *icon_bt = NULL;
static lv_obj_t *icon_path = NULL;
static lv_obj_t *indicator_mouse = NULL;
static lv_obj_t *label_layer_name = NULL;
static lv_obj_t *label_version = NULL;
static TickType_t last_mouse_activity = 0;
static bool bt_blink_visible = true;
static TickType_t bt_blink_last_tick = 0;
static bool oled_initialized = false;
static lv_obj_t *tama_label = NULL;

static void oled_init_icons(void)
{
    if (!display_available) return;
    if (icon_bt != NULL || icon_path != NULL) return;

    lv_obj_t *scr = lv_scr_act();

    /* ── Top: version label ───────────────────────────────────── */

    if (!label_version) {
        label_version = lv_label_create(scr);
        lv_label_set_text(label_version, "v" FW_VERSION);
        lv_obj_set_style_text_font(label_version, &lv_font_montserrat_28, 0);
        lv_obj_align(label_version, LV_ALIGN_TOP_MID, 0, 2);
    }

    /* ── Bottom bar: icons + layer name + mouse ───────────────── */

    icon_path = lv_img_create(scr);
    if (icon_path)
        lv_obj_set_pos(icon_path, 0, 48);

    icon_bt = lv_img_create(scr);
    lv_obj_set_pos(icon_bt, 18, 48);

    if (!label_layer_name) {
        label_layer_name = lv_label_create(scr);
        lv_label_set_text(label_layer_name, default_layout_names[current_layout]);
        lv_obj_set_style_text_font(label_layer_name, UI_FONT, 0);
        lv_obj_set_pos(label_layer_name, 38, 48);
    }

    indicator_mouse = lv_label_create(scr);
    lv_label_set_text(indicator_mouse, "M");
    lv_obj_set_style_text_font(indicator_mouse, UI_FONT, 0);
    lv_obj_set_pos(indicator_mouse, 118, 48);
    lv_obj_add_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
}

static void oled_prepare_ui(bool clear_screen)
{
    if (!display_available) return;
    if (!lvgl_port_lock(200)) return;

    if (clear_screen) {
        display_clear_screen();
        icon_bt = NULL;
        icon_path = NULL;
        indicator_mouse = NULL;
        label_version = NULL;
        label_layer_name = NULL;
        last_bt_state = -1;
        last_path_state = -1;
        bt_blink_visible = true;
        bt_blink_last_tick = xTaskGetTickCount();
    }

    oled_init_icons();
    oled_initialized = true;
    lvgl_port_unlock();
}

static void oled_update_connection_icons(bool force)
{
    if (!display_available) return;

    int bt_state;
    if (!hid_bluetooth_is_initialized())      bt_state = 0;
    else if (hid_bluetooth_is_connected())    bt_state = 1;
    else                                       bt_state = 2;

    int path_state = (keyboard_get_usb_bl_state() == 0) ? 0 : 1;

    bool blink_toggled = false;
    if (bt_state == 2 && !force) {
        TickType_t now = xTaskGetTickCount();
        if ((now - bt_blink_last_tick) >= pdMS_TO_TICKS(BT_BLINK_INTERVAL_MS)) {
            bt_blink_visible = !bt_blink_visible;
            bt_blink_last_tick = now;
            blink_toggled = true;
        }
    }

    if (!force && bt_state == last_bt_state && path_state == last_path_state && !blink_toggled)
        return;

    if (!lvgl_port_lock(50)) return;
    oled_init_icons();
    if (!icon_path) { lvgl_port_unlock(); return; }

    if (bt_state == 0) {
        bt_blink_visible = false;
        if (icon_bt) lv_obj_add_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
    } else if (bt_state == 1) {
        bt_blink_visible = true;
        if (icon_bt) { lv_obj_clear_flag(icon_bt, LV_OBJ_FLAG_HIDDEN); lv_img_set_src(icon_bt, &wifi); }
    } else {
        if (force) { bt_blink_visible = true; bt_blink_last_tick = xTaskGetTickCount(); }
        if (icon_bt) {
            if (bt_blink_visible) { lv_obj_clear_flag(icon_bt, LV_OBJ_FLAG_HIDDEN); lv_img_set_src(icon_bt, &wifi); }
            else lv_obj_add_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (path_state == 0) lv_img_set_src(icon_path, &flash);
    else                  lv_img_set_src(icon_path, &bluetooth_16px);

    last_bt_state = bt_state;
    last_path_state = path_state;
    lvgl_port_unlock();
}

/* ── Backend interface ───────────────────────────────────────────── */

static bool oled_init(void)
{
    display_hw_config_t cfg = {
        .bus_type = BOARD_DISPLAY_BUS,
        .width = BOARD_DISPLAY_WIDTH,
        .height = BOARD_DISPLAY_HEIGHT,
        .pixel_clock_hz = BOARD_DISPLAY_CLK_HZ,
        .reset_pin = BOARD_DISPLAY_RESET,
        .i2c = {
            .host = BOARD_DISPLAY_I2C_HOST,
            .sda = BOARD_DISPLAY_I2C_SDA,
            .scl = BOARD_DISPLAY_I2C_SCL,
            .address = BOARD_DISPLAY_I2C_ADDR,
            .enable_internal_pullups = BOARD_DISPLAY_I2C_PULLUPS,
        },
    };
    display_set_hw_config(&cfg);
    init_display();
    return display_available;
}

static void oled_update_layer(void)
{
    if (!oled_initialized) oled_prepare_ui(true);
    else                    oled_prepare_ui(false);

    if (label_layer_name && lvgl_port_lock(50)) {
        lv_label_set_text(label_layer_name, default_layout_names[current_layout]);
        lvgl_port_unlock();
    }
}

static void oled_update(void)
{
    oled_update_connection_icons(false);
    if (lvgl_port_lock(50)) {
        if (indicator_mouse) {
            if ((xTaskGetTickCount() - last_mouse_activity) < pdMS_TO_TICKS(MOUSE_INDICATOR_MS))
                lv_obj_clear_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
        }
        /* Update tama */
        if (tama_engine_is_enabled())
            tama_render_update(tama_engine_get_state(), tama_engine_get_stats(), tama_engine_get_critter());
        lvgl_port_unlock();
    }
}

static void oled_refresh_all(void)
{
    oled_prepare_ui(true);
    oled_update_layer();
    oled_update_connection_icons(true);

    /* Tama sprite canvas */
    if (tama_engine_is_enabled() && lvgl_port_lock(50)) {
        lv_obj_t *scr = lv_scr_act();
        tama_render_create(scr, BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
        lvgl_port_unlock();
    }
}

static void oled_sleep(void)
{
    if (lvgl_port_lock(100)) {
        display_clear_screen();
        icon_bt = NULL; icon_path = NULL; indicator_mouse = NULL;
        label_version = NULL; label_layer_name = NULL;
        last_bt_state = -1; last_path_state = -1;
        tama_render_destroy();
        oled_initialized = false;
        lvgl_port_unlock();
    }
}

static void oled_wake(void)
{
    /* Defer to display task via request_wake_request flag */
    request_wake_request = true;
}

static void oled_notify_mouse(void)
{
    last_mouse_activity = xTaskGetTickCount();
}

static void oled_notify_keypress(void)
{
    if (tama_engine_is_enabled())
        tama_engine_keypress(0); /* no KPM tracking on OLED */
}

static void oled_show_dfu(void)
{
    display_clear_screen();
    write_text_to_display_centre("DFU Mode", 0, 0);
}

const display_backend_t oled_display_backend = {
    .init          = oled_init,
    .update_layer  = oled_update_layer,
    .update        = oled_update,
    .refresh_all   = oled_refresh_all,
    .sleep         = oled_sleep,
    .wake          = oled_wake,
    .notify_mouse    = oled_notify_mouse,
    .notify_keypress = oled_notify_keypress,
    .show_dfu        = oled_show_dfu,
};
