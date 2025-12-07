#include "status_display.h"
#include "hid_bluetooth_manager.h"
#include "keyboard_manager.h"
#include "i2c_oled_display.h"
#include "keyboard_config.h"
#include "img_bluetooth.c"
#include "img_usb.c"
#include "img_signal.c"
#include "lvgl.h"
#include "keymap.h"
#include "matrix.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int last_bt_state = -1;   // -1 = inconnu, 0 = OFF, 1 = ON, 2 = JUSTE BT
static int last_path_state = -1; // 0 = USB, 1 = BLE

static lv_obj_t *icon_bt = NULL;
static lv_obj_t *icon_path = NULL;
static bool status_display_initialized = false;
static bool status_display_sleeping = false;
static const char *status_display_version_text = GATTS_TAG;
static bool bt_blink_visible = true;
static TickType_t bt_blink_last_tick = 0;
static const TickType_t bt_blink_interval_ticks = pdMS_TO_TICKS(500);

static bool is_showing_splash = false;
static TickType_t splash_start_tick = 0;

static void status_display_prepare_ui(bool clear_screen);
static void status_display_update_connection_icons(bool force);
static void status_display_init_icons(void);
static void status_display_show_version_splash(void);
void status_display_show_DFU_prog(void);

void draw_separator_line(void)
{
    if(display_available == false) return;
    draw_rectangle_White(0, 37, 128, 2);
}

void status_display_update_layer_name(void)
{ 
    if(display_available == false || status_display_sleeping) return;
    
    is_showing_splash = false;

    if (!status_display_initialized)
    {
        status_display_prepare_ui(true);
    }
    else
    {
        status_display_prepare_ui(false);
    }
    // Efface une bande autour de la zone du texte
    draw_separator_line();
    draw_rectangle(38, 40, 128-38, 24);
    // Réécrit le nom du layout à la même position qu'avant
    write_text_to_display(default_layout_names[current_layout], 38, 48);
}

void status_display_update(void)
{
    if(display_available == false || status_display_sleeping) return;

    if (is_showing_splash) {
        if ((xTaskGetTickCount() - splash_start_tick) > pdMS_TO_TICKS(3000)) {
            is_showing_splash = false;
            status_display_refresh_all();
        } else {
            return;
        }
    }

    status_display_update_connection_icons(false);
}

void status_display_start(void)
{
    display_hw_config_t cfg = keyboard_get_display_config();
    display_set_hw_config(&cfg);
    init_display();
    if (!display_available)
    {
        status_display_initialized = false;
        return;
    }

    status_display_sleeping = false;
    status_display_show_version_splash();
    // status_display_refresh_all();
}

void status_display_refresh_all(void)
{
    if (display_available == false)
        return;

    is_showing_splash = false;
    status_display_sleeping = false;
    status_display_prepare_ui(true);
    status_display_update_layer_name();
    status_display_update_connection_icons(true);
}

void status_display_sleep(void)
{
    if (display_available == false || status_display_sleeping)
        return;

    display_clear_screen();
    status_display_sleeping = true;
    icon_bt = NULL;
    icon_path = NULL;
    last_bt_state = -1;
    last_path_state = -1;
    bt_blink_visible = true;
    bt_blink_last_tick = xTaskGetTickCount();
    status_display_initialized = false;
}

void status_display_wake(void)
{
    if (display_available == false)
        return;

    if (!status_display_sleeping)
        return;

    status_display_refresh_all();
}

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

    if (!force && bt_state == last_bt_state && path_state == last_path_state && bt_state != 2) {
        return;
    }

    status_display_init_icons();
    if (!icon_path) return;

    // Gestion de l'icône BT : visible uniquement si le Bluetooth est initialisé
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
        TickType_t now = xTaskGetTickCount();
        if (force) {
            bt_blink_visible = true;
            bt_blink_last_tick = now;
        } else if ((now - bt_blink_last_tick) >= bt_blink_interval_ticks) {
            bt_blink_visible = !bt_blink_visible;
            bt_blink_last_tick = now;
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

    // Choix de l'icône chemin (USB/BLE)
    if (path_state == 0) {
        lv_img_set_src(icon_path, &flash);        // USB
    } else {
        lv_img_set_src(icon_path, &bluetooth_16px);         // BLE (on réutilise wifi comme “onde”)
    }

    last_bt_state = bt_state;
    last_path_state = path_state;
}

static void status_display_init_icons(void)
{
    if(display_available == false) return;
    if (icon_bt != NULL || icon_path != NULL) return;

    lv_obj_t *scr = lv_scr_act();

    icon_bt = lv_img_create(scr);
    lv_obj_set_pos(icon_bt, 20, 48);   // bas droite (16x16)

    icon_path = lv_img_create(scr);
    lv_obj_set_pos(icon_path, 0, 48);  // bas gauche (16x16)
}

static void status_display_prepare_ui(bool clear_screen)
{
    if (display_available == false)
        return;

    if (clear_screen)
    {
        display_clear_screen();
        draw_separator_line();
        icon_bt = NULL;
        icon_path = NULL;
        last_bt_state = -1;
        last_path_state = -1;
        bt_blink_visible = true;
        bt_blink_last_tick = xTaskGetTickCount();
    }

    status_display_init_icons();
    status_display_initialized = true;
}

static void status_display_show_version_splash(void)
{
    if (display_available == false)
        return;

    display_clear_screen();
    write_text_to_display_centre(status_display_version_text, 0, 0);
    
    is_showing_splash = true;
    splash_start_tick = xTaskGetTickCount();
    status_display_initialized = false;
}


void status_display_show_DFU_prog(void)
{
    if (display_available == false)
        return;
    display_clear_screen();
    write_text_to_display_centre("DFU Mode", 0, 0); 
}