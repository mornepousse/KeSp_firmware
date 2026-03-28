/* Key processing: keycode building, layer switching, macros, internal functions */
#include "key_processor.h"
#include "matrix_scan.h"
#include "keyboard_config.h"
#include "key_definitions.h"
#include "keymap.h"
#include "keyboard_actions.h"
#include "keyboard_task.h"
#include "hid_bluetooth_manager.h"
#include "esp_log.h"

static const char *TAG = "KEY_PROC";

uint16_t keypress_internal_function = 0;
uint8_t current_row_layer_changer = INVALID_KEY_POS;
uint8_t current_col_layer_changer = INVALID_KEY_POS;
uint16_t extra_keycodes[6] = {0};

/* ── Layer switching ─────────────────────────────────────────────── */

static void apply_momentary_layer(int16_t keycode, uint8_t key_idx)
{
    if (keycode >= MO_L0 && keycode <= MO_L9) {
        last_layer = current_layout;
        current_layout = (keycode - MO_L0) / 256;
        layer_changed();
        current_row_layer_changer = current_press_row[key_idx];
        current_col_layer_changer = current_press_col[key_idx];
    }
}

static void apply_toggle_layer(uint16_t keycode)
{
    if (keycode >= TO_L0 && keycode <= TO_L9) {
        int16_t new_layer = (keycode - TO_L0) / 256;
        if (current_layout == new_layer) {
            current_layout = 0;
        } else {
            current_layout = new_layer;
        }
        last_layer = current_layout;
        layer_changed();
    }
}

/* ── Internal function detection & dispatch ───────────────────────── */

static bool detect_internal_function(int16_t keycode)
{
    if (keycode >= TO_L0) {
        if (keypress_internal_function == 0) {
            keypress_internal_function = keycode;
            return true;
        }
    }
    return false;
}

static void dispatch_internal_function(void)
{
    switch (keypress_internal_function) {
    case BT_SWITCH_DEVICE:
        if (usb_bl_state == 0 && hid_bluetooth_is_initialized() && hid_bluetooth_is_connected()) {
            usb_bl_state = 1;
        } else {
            usb_bl_state = 0;
        }
        km_post_display_update();
        break;
    case BT_TOGGLE:
        ESP_LOGI(TAG, "BT_TOGGLE");
        km_post_bt_toggle();
        break;
    }
}

/* ── Macro expansion ─────────────────────────────────────────────── */

static void expand_macro(uint16_t keycode)
{
    if (keycode >= MACRO_1 && keycode <= MACRO_20) {
        int16_t macro_i = (keycode - MACRO_1) / 256;
        if (macro_i < MAX_MACROS && macros_list[macro_i].name[0] != '\0') {
            uint8_t j = 0;
            for (uint8_t i = 0; i < 6; i++) {
                if (keycodes[i] == 0) {
                    keycodes[i] = macros_list[macro_i].keys[j];
                    j++;
                }
            }
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

void build_keycode_report(void)
{
    for (uint8_t i = 0; i < 6; i++) {
        if (current_press_col[i] != INVALID_KEY_POS) {
            uint16_t kc = keymaps[current_layout][current_press_row[i]][current_press_col[i]];

            apply_momentary_layer(kc, i);
            detect_internal_function(kc);
            expand_macro(kc);

            if (current_row_layer_changer == current_press_row[i] &&
                current_col_layer_changer == current_press_col[i]) {
                /* layer changer key — skip normal mapping */
            } else {
                if (kc == K_NO) {
                    kc = keymaps[last_layer][current_press_row[i]][current_press_col[i]];
                }
                if (kc > 255) {
                    extra_keycodes[i] = kc;
                } else {
                    keycodes[i] = kc;
                }
            }
        } else {
            extra_keycodes[i] = 0;
            keycodes[i] = 0;
        }
    }

    /* Check if momentary layer key was released */
    if (current_layout != last_layer) {
        bool changer_held = false;
        for (uint8_t i = 0; i < 6; i++) {
            if (current_press_col[i] == current_col_layer_changer &&
                current_press_row[i] == current_row_layer_changer) {
                changer_held = true;
                break;
            }
        }
        if (!changer_held) {
            current_layout = last_layer;
            layer_changed();
            current_col_layer_changer = INVALID_KEY_POS;
            current_row_layer_changer = INVALID_KEY_POS;
        }
    }
}

void process_matrix_changes(void)
{
    if (keypress_internal_function == 0) return;

    /* Check if the internal function key is still held */
    bool still_held = false;
    for (uint8_t i = 0; i < 6; i++) {
        if (extra_keycodes[i] == keypress_internal_function) {
            still_held = true;
            extra_keycodes[i] = 0;
            break;
        }
    }

    if (!still_held) {
        ESP_LOGI(TAG, "Executing internal func: 0x%04X", keypress_internal_function);
        dispatch_internal_function();
        apply_toggle_layer(keypress_internal_function);
        keypress_internal_function = 0;
    }
}
