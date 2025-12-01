#include "status_display.h"
#include "hid_bluetooth_manager.h"
#include "keyboard_manager.h"
#include "i2c_oled_display.h"
#include "img_bluetooth.c"
#include "img_usb.c"
#include "img_signal.c"
#include "lvgl.h"
#include "keymap.h"
#include "matrix.h"

static int last_bt_state = -1;   // -1 = inconnu, 0 = OFF, 1 = ON, 2 = JUSTE BT
static int last_path_state = -1; // 0 = USB, 1 = BLE

static lv_obj_t *icon_bt = NULL;
static lv_obj_t *icon_path = NULL;

void draw_separator_line(void)
{
    if(display_available == false) return;
    draw_rectangle_White(0, 37, 128, 2);
}

void status_display_update_layer_name(void)
{ 
    if(display_available == false) return;
     // Efface une bande autour de la zone du texte
     draw_separator_line();
    draw_rectangle(38, 40, 128-38, 24);
    // Réécrit le nom du layout à la même position qu'avant
    write_text_to_display(default_layout_names[current_layout], 38, 48);
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

void status_display_update(void)
{
    if(display_available == false) return;
    int bt_state;
    if (!hid_bluetooth_is_initialized()) {
        bt_state = 0;
    } else if (hid_bluetooth_is_connected()) {
        bt_state = 1;
    } else {
        bt_state = 2;
    }

    int path_state = (keyboard_get_usb_bl_state() == 0) ? 0 : 1;

    if (bt_state == last_bt_state && path_state == last_path_state) {
        return;
    }

    status_display_init_icons();
    if (!icon_path) return;

    // Gestion de l'icône BT : visible uniquement si le Bluetooth est initialisé
    if (bt_state == 0) {
        if (icon_bt) lv_obj_add_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (icon_bt) {
            lv_obj_clear_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
            lv_img_set_src(icon_bt, &wifi); // même icône pour BT actif/BT connecté
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
