#include "status_display.h"
#include "hid_bluetooth_manager.h"
#include "keyboard_task.h"
#include "i2c_oled_display.h"
#include "keyboard_config.h"
#ifdef BOARD_DISPLAY_BACKEND_ROUND
#include "round_ui.h"
#include "spi_round_display.h"
#else
#include "assets/img_bluetooth.c"
#include "assets/img_usb.c"
#include "assets/img_signal.c"
#endif
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "keymap.h"
#include "esp_log.h"
/* Optional debug: create a minimal UI (only the layer label) to check if allocations cause crashes. Set to 1 to enable. */
#ifndef STATUS_DISPLAY_MINIMAL
#define STATUS_DISPLAY_MINIMAL 0
#endif

#ifndef BT_BLINK_INTERVAL_MS
#define BT_BLINK_INTERVAL_MS    500
#endif
#ifndef SPLASH_DURATION_MS
#define SPLASH_DURATION_MS      3000
#endif
#ifndef MOUSE_INDICATOR_MS
#define MOUSE_INDICATOR_MS      200
#endif

#include "matrix_scan.h"
#include "hid_report.h"
#include "version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

LV_FONT_DECLARE(lv_font_montserrat_28);

#ifndef BOARD_DISPLAY_BACKEND_ROUND

static int last_bt_state = -1;   // -1 = inconnu, 0 = OFF, 1 = ON, 2 = JUSTE BT
static int last_path_state = -1; // 0 = USB, 1 = BLE

static lv_obj_t *icon_bt = NULL;
static lv_obj_t *icon_path = NULL;
static lv_obj_t *indicator_mouse = NULL;
static lv_obj_t *label_layer_name = NULL; /* persistent label for layout name - avoid recreating each update */
static lv_obj_t *label_version = NULL;
static TickType_t last_mouse_activity = 0;

static bool bt_blink_visible = true;
static TickType_t bt_blink_last_tick = 0;
static const TickType_t bt_blink_interval_ticks = pdMS_TO_TICKS(BT_BLINK_INTERVAL_MS);
#endif /* !BOARD_DISPLAY_BACKEND_ROUND */

static bool status_display_initialized = false;
static bool status_display_sleeping = false;

/* Expose a controlled way to disable the display from other modules */
void status_display_force_disable(void)
{
    /* The display availability flag lives in i2c_oled_display.c */
    extern bool display_available;
    display_available = false;
    status_display_initialized = false;
}

static bool is_showing_splash = false;
static TickType_t splash_start_tick = 0;

/* LVGL lock functions will be used (lvgl_port_lock / lvgl_port_unlock) provided by the LVGL port */


/* Request wake flag so wake work runs in display task context */
/* Extern visible flag for display task loop */
volatile bool request_wake_request = false;

#ifndef BOARD_DISPLAY_BACKEND_ROUND
static void status_display_prepare_ui(bool clear_screen);
static void status_display_update_connection_icons(bool force);
static void status_display_init_icons(void);
#endif
void status_display_show_DFU_prog(void);

/* UI_SCALE and UI_FONT are now defined in board.h */

void draw_separator_line(void)
{
    /* Separator removed — version label uses top area */
}

void status_display_update_layer_name(void)
{ 
    if(display_available == false || status_display_sleeping) return;
    
#ifdef BOARD_DISPLAY_BACKEND_ROUND
    round_ui_update_layer();
#else
    is_showing_splash = false;

    if (!status_display_initialized)
    {
        status_display_prepare_ui(true);
    }
    else
    {
        status_display_prepare_ui(false);
    }

    /* Update persistent label text instead of recreating UI objects */
    if (label_layer_name) {
        if (lvgl_port_lock(50)) {
            lv_label_set_text(label_layer_name, default_layout_names[current_layout]);
            lvgl_port_unlock();
        }
    } else {
        /* Fallback: ensure UI prepared */
        status_display_prepare_ui(false);
    }
#endif
}

void status_display_update(void)
{
    if(display_available == false || status_display_sleeping) return;

#ifdef BOARD_DISPLAY_BACKEND_ROUND
    if (is_showing_splash) {
        if ((xTaskGetTickCount() - splash_start_tick) > pdMS_TO_TICKS(SPLASH_DURATION_MS)) {
            is_showing_splash = false;
            round_ui_refresh_all();
        }
        return;
    }
    round_ui_update();
#else
    if (is_showing_splash) {
        if ((xTaskGetTickCount() - splash_start_tick) > pdMS_TO_TICKS(SPLASH_DURATION_MS)) {
            is_showing_splash = false;
            status_display_refresh_all();
        }
        return;
    }

    status_display_update_connection_icons(false);

    if (indicator_mouse) {
        if (lvgl_port_lock(50)) {
            if ((xTaskGetTickCount() - last_mouse_activity) < pdMS_TO_TICKS(MOUSE_INDICATOR_MS)) {
                lv_obj_clear_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
            }
            lvgl_port_unlock();
        }
    }
#endif
} 

void status_display_start(void)
{
    display_hw_config_t cfg = keyboard_get_display_config();
    
#ifdef BOARD_DISPLAY_BACKEND_ROUND
    /* Use dedicated SPI driver for round display */
    if (!spi_display_init(&cfg)) {
        status_display_initialized = false;
        display_available = false;
        return;
    }
    display_available = true;
#else
    /* Use I2C OLED driver */
    display_set_hw_config(&cfg);
    init_display();
    if (!display_available)
    {
        status_display_initialized = false;
        return;
    }
#endif

    /* Silence STATUS_DISP informational logs in normal runs to reduce log noise */
    esp_log_level_set("STATUS_DISP", ESP_LOG_WARN);

    /* LVGL port provides a lock; ensure it's initialized (lvgl_port_init was called in init_display) */

    status_display_sleeping = false;
    status_display_refresh_all();
}

void status_display_refresh_all(void)
{
    if (display_available == false)
        return;

    is_showing_splash = false;
    status_display_sleeping = false;
#ifdef BOARD_DISPLAY_BACKEND_ROUND
    round_ui_refresh_all();
#else
    status_display_prepare_ui(true);
    status_display_update_layer_name();
    status_display_update_connection_icons(true);
#endif
}

void status_display_sleep(void)
{
    if (display_available == false || status_display_sleeping)
        return;

#ifdef BOARD_DISPLAY_BACKEND_ROUND
    round_ui_sleep();
    status_display_sleeping = true;
    status_display_initialized = false;
#else
    if (lvgl_port_lock(100)) {
        display_clear_screen();
        status_display_sleeping = true;
        icon_bt = NULL;
        icon_path = NULL;
        /* persistent label can be freed by display_clear_screen; reset it to avoid dangling pointer */
        label_layer_name = NULL;
        last_bt_state = -1;
        last_path_state = -1;
        bt_blink_visible = true;
        bt_blink_last_tick = xTaskGetTickCount();
        status_display_initialized = false;
        lvgl_port_unlock();
    } else {
        ESP_LOGE("STATUS_DISP", "Could not take LVGL port lock to enter sleep - aborting sleep");
    }
#endif /* !BOARD_DISPLAY_BACKEND_ROUND */
}

void status_display_wake(void)
{
    if (display_available == false)
        return;

    if (!status_display_sleeping)
        return;

#ifdef BOARD_DISPLAY_BACKEND_ROUND
    round_ui_wake();
    status_display_sleeping = false;
#else
    /* Defer actual UI work to the display task to avoid calling LVGL from other contexts */
    request_wake_request = true;
#endif
}

#ifndef BOARD_DISPLAY_BACKEND_ROUND
static void status_display_update_connection_icons(bool force)
{
    if (display_available == false || status_display_sleeping)
        return;

    int bt_state;
    if (!hid_bluetooth_is_initialized()) {
        bt_state = 0;
    } else if (hid_bluetooth_is_connected()) {
        bt_state = 1;
    } else {
        bt_state = 2;
    }

    int path_state = (keyboard_get_usb_bl_state() == 0) ? 0 : 1;

    /* For blink mode (bt_state==2), check if blink toggle is due */
    bool blink_toggled = false;
    if (bt_state == 2 && !force) {
        TickType_t now = xTaskGetTickCount();
        if ((now - bt_blink_last_tick) >= bt_blink_interval_ticks) {
            bt_blink_visible = !bt_blink_visible;
            bt_blink_last_tick = now;
            blink_toggled = true;
        }
    }

    /* Short-circuit: nothing changed */
    if (!force && bt_state == last_bt_state && path_state == last_path_state && !blink_toggled) {
        return;
    }

    /* Take LVGL lock and init icons if needed */
    if (!lvgl_port_lock(50)) {
        return;
    }
    status_display_init_icons();

    if (!icon_path) {
        lvgl_port_unlock();
        return;
    }

    /* BT icon: only visible when Bluetooth is initialized */
    if (bt_state == 0) {
        bt_blink_visible = false;
        bt_blink_last_tick = xTaskGetTickCount();
        if (icon_bt) {
            lv_obj_add_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (bt_state == 1) {
        bt_blink_visible = true;
        bt_blink_last_tick = xTaskGetTickCount();
        if (icon_bt) {
            lv_obj_clear_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
            lv_img_set_src(icon_bt, &wifi);
        }
    } else {
        if (force) {
            bt_blink_visible = true;
            bt_blink_last_tick = xTaskGetTickCount();
        }
        if (icon_bt) {
            if (bt_blink_visible) {
                lv_obj_clear_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
                lv_img_set_src(icon_bt, &wifi);
            } else {
                lv_obj_add_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    /* Path icon: USB or BLE */
    if (path_state == 0) {
        lv_img_set_src(icon_path, &flash);        // USB
    } else {
        lv_img_set_src(icon_path, &bluetooth_16px);         // BLE
    }

    last_bt_state = bt_state;
    last_path_state = path_state;

    lvgl_port_unlock();
}


static void status_display_init_icons(void)
{
    if(display_available == false) return;
    if (icon_bt != NULL || icon_path != NULL) return;

    /* Note: caller must hold lvgl_mutex */
    lv_obj_t *scr = lv_scr_act();

    icon_bt = lv_img_create(scr);
    lv_obj_set_pos(icon_bt, 20 * UI_SCALE, 48 * UI_SCALE);
    #ifdef BOARD_DISPLAY_BACKEND_ROUND
        lv_img_set_zoom(icon_bt, 512);
    #endif

#if !STATUS_DISPLAY_MINIMAL
    icon_path = lv_img_create(scr);
#endif
    if (icon_path) {
        lv_obj_set_pos(icon_path, 0, 48 * UI_SCALE);
        #ifdef BOARD_DISPLAY_BACKEND_ROUND
            lv_img_set_zoom(icon_path, 512);
        #endif
    }

#if !STATUS_DISPLAY_MINIMAL
    indicator_mouse = lv_label_create(scr);
#endif
    if (indicator_mouse) {
        lv_label_set_text(indicator_mouse, "M");
        lv_obj_set_style_text_font(indicator_mouse, UI_FONT, 0);
        lv_obj_set_pos(indicator_mouse, 118 * UI_SCALE, 48 * UI_SCALE);
        lv_obj_add_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
    }

    /* persistent label for layer name (created once) */
    if (!label_version) {
        label_version = lv_label_create(scr);
        lv_label_set_text(label_version, "v" FW_VERSION);
        lv_obj_set_style_text_font(label_version, &lv_font_montserrat_28, 0);
        lv_obj_align(label_version, LV_ALIGN_TOP_MID, 0, 2 * UI_SCALE);
    }

    if (!label_layer_name) {
        label_layer_name = lv_label_create(scr);
        lv_label_set_text(label_layer_name, default_layout_names[current_layout]);
        lv_obj_set_style_text_font(label_layer_name, UI_FONT, 0);
        lv_obj_set_pos(label_layer_name, 38 * UI_SCALE, 48 * UI_SCALE);
    } else {
        lv_label_set_text(label_layer_name, default_layout_names[current_layout]);
        lv_obj_set_style_text_font(label_layer_name, UI_FONT, 0);
        lv_obj_set_pos(label_layer_name, 38 * UI_SCALE, 48 * UI_SCALE);
    }
}

static void status_display_prepare_ui(bool clear_screen)
{
    if (display_available == false)
        return;

    if (!lvgl_port_lock(200)) {
        ESP_LOGW("STATUS_DISP", "Could not take LVGL port lock to prepare UI");
        return;
    }

    if (clear_screen)
    {
        display_clear_screen();
        draw_separator_line();
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

    status_display_init_icons();

    status_display_initialized = true;
    lvgl_port_unlock();
}

#endif /* !BOARD_DISPLAY_BACKEND_ROUND */


void status_display_show_DFU_prog(void)
{
    if (display_available == false)
        return;
    display_clear_screen();
    write_text_to_display_centre("DFU Mode", 0, 0); 
}

void status_display_notify_mouse_activity(void)
{
#ifdef BOARD_DISPLAY_BACKEND_ROUND
    round_ui_notify_mouse();
#else
    last_mouse_activity = xTaskGetTickCount();
#endif
}

