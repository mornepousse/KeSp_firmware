#include "status_display.h"
#include "hid_bluetooth_manager.h"
#include "i2c_oled_display.h"
#include "keyboard_manager.h"

static int last_bt_state = -1;   // -1 = inconnu, 0 = OFF, 1 = ON, 2 = JUSTE BT
static int last_path_state = -1; // 0 = USB, 1 = BLE

void status_display_update(void)
{
    int bt_state;
    if (!hid_bluetooth_is_initialized()) {
        bt_state = 0;
    } else if (hid_bluetooth_is_connected()) {
        bt_state = 1;
    } else {
        bt_state = 2;
    }

    int path_state = (keyboard_get_usb_bl_state() == 0) ? 0 : 1;

    // Effacer l'ancienne zone seulement si quelque chose change
    if (bt_state != last_bt_state || path_state != last_path_state) {
        // Nettoyer une bande en bas de l'écran
        draw_rectangle(0, 48, 128, 16);

        if (bt_state == 0) {
            write_text_to_display("BT OFF", 70, 50);
        } else if (bt_state == 1) {
            write_text_to_display("BT ON", 70, 50);
        } else {
            write_text_to_display("BT", 70, 50);
        }

        if (path_state == 0) {
            write_text_to_display("USB", 2, 50);
        } else {
            write_text_to_display("BLE", 2, 50);
        }

        last_bt_state = bt_state;
        last_path_state = path_state;
    }
}
