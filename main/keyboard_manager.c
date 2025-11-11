#include "keyboard_manager.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "hid_bluetooth_manager.h"
#include "key_definitions.h"
#include "keyboard_config.h"
#include "keymap.h"
#include "matrix.h"
#include "tinyusb.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
// #include "graphic_manager.h"

static const char *KM_TAG = "Keyboard_manager";

uint8_t bl_state = 0;
uint8_t usb_bl_state = 0; // 0: USB, 1: BL
uint16_t keypress_internal_function = 0;
uint8_t current_row_layer_changer = 255;
uint8_t current_col_layer_changer = 255;

uint16_t extra_keycodes[6] = {0, 0, 0, 0, 0, 0};



// Liste des macros prédéfinies
macro_t macros_list[] = {
    {{K_LCTRL, K_C}, MACRO_1}, // Macro 1 = Ctrl + C (Copier)
    {{K_LCTRL, K_V}, MACRO_2}, // Macro 2 = Ctrl + V (Coller)
};
size_t macros_count = sizeof(macros_list) / sizeof(macros_list[0]);
void send_hid_key() {
  if (usb_bl_state == 0) {
    if (tud_mounted())
      tud_hid_keyboard_report(1, 0, keycodes);
  } else {
    send_hid_bl_key(keycodes);
  }
}

void run_internal_funct() {

  switch (keypress_internal_function) {
  case TO_L3:
    if (current_layout == 2) {
      current_layout = 0;
      layer_changed();
      last_layer = current_layout;
      ESP_LOGI(KM_TAG, "layer: 0");

    } else {
      current_layout = 2;
      layer_changed();
      last_layer = current_layout;
      ESP_LOGI(KM_TAG, "layer: 2");
    }
    break;
  case K_INT3:
    if (usb_bl_state == 0) {
      usb_bl_state = 1;

    } else {
      usb_bl_state = 0;
    }
    break;
  default:
    break;
  }
}

bool is_internal_function(int16_t keycodeTMP) {
  if (keycodeTMP >= TO_L0) {
    ESP_LOGI(KM_TAG, "%d.", keycodeTMP);
    if (keypress_internal_function == 0) {
      keypress_internal_function = keycodeTMP;
      return true;
    }
  }
  return false;
}

void is_momentary_layer(int16_t keycodeTMP, uint8_t i) {
  if ((keycodeTMP >= MO_L0) && (keycodeTMP <= MO_L9)) {

    last_layer = current_layout;
    current_layout = (keycodeTMP - MO_L0) / 256;
    layer_changed();
    ESP_LOGI(KM_TAG, "last_layer: %d current : %d\n", last_layer,
             current_layout);
    current_row_layer_changer = current_press_row[i];
    current_col_layer_changer = current_press_col[i];
  }
}

void is_toggle_layer(uint16_t keycodeTMP) {
  if ((keycodeTMP >= TO_L0) && (keycodeTMP <= TO_L9)) {

    int16_t new_layer = (keycodeTMP - TO_L0) / 256;

    if (current_layout == new_layer) {
      current_layout = 0;
      last_layer = current_layout;
      layer_changed();
      ESP_LOGI(KM_TAG, "layer: 0 %d %d %d", new_layer, keycodeTMP, TO_L0);
      // write_txt("Layer %d", n), 0, -30);
      //  gpio_set_level(CURSOR_LED_WHT_PIN, 0);
    } else {
      current_layout = new_layer;
      last_layer = current_layout;
      layer_changed();
      ESP_LOGI(KM_TAG, "layer: pp %d %d %d", new_layer, keycodeTMP, TO_L0);

      // write_txt("Layer %d", n), 0, -30);

      // gpio_set_level(CURSOR_LED_WHT_PIN, 1);
    }
  }
}

void is_macro(uint16_t keycodeTMP) {
  if ((keycodeTMP >= MACRO_1) && (keycodeTMP <= MACRO_20)) {
    int16_t macro_i = (keycodeTMP - MACRO_1) / 256;
    ESP_LOGI(KM_TAG, "macro: %d ", macro_i);
    if (macro_i < macros_count) {
      uint8_t j = 0;
      for (uint8_t i = 0; i < 6; i++) {
        if (keycodes[i] == 0) {
          keycodes[i] = macros_list[macro_i].keys[j];
          j++;
        }
      }
    }
    send_hid_key();
  }
}

void vTaskKeyboard(void *pvParameters) {
  for (;;) {
    // Task code goes here.
    scan_matrix();
    uint16_t keycodeTMP = 0;
    for (uint8_t i = 0; i < 6; i++) {
      if (current_press_col[i] != 255) {
        keycodeTMP =
            keymaps[current_layout][current_press_row[i]][current_press_col[i]];

        is_momentary_layer(keycodeTMP, i);

        is_internal_function(keycodeTMP); // si c'est pas une touche special
        is_macro(keycodeTMP);
        if (current_row_layer_changer == current_press_row[i] &&
            current_col_layer_changer == current_press_col[i]) {
        } else {
          if (keycodeTMP == K_NO) {
            keycodeTMP =
                keymaps[last_layer][current_press_row[i]][current_press_col[i]];
          }
          if (keycodeTMP > 255) {
            extra_keycodes[i] = keycodeTMP;
          } else
            keycodes[i] = keycodeTMP;
        }

      } else {

        extra_keycodes[i] = 0;
        keycodes[i] = 0;
      }
    }
    if (current_layout != last_layer) {

      uint8_t changer = 0;
      for (uint8_t i = 0; i < 6; i++) {
        if (current_press_col[i] == current_col_layer_changer &&
            current_press_row[i] == current_row_layer_changer) {
          // ESP_LOGI(KM_TAG, "change 1\n");
          changer = 1;
          break;
        }
      }

      if (changer == 0) {
        // ESP_LOGI(KM_TAG, "change 0\n");
        current_layout = last_layer;
        layer_changed();
        current_col_layer_changer = 255;
        current_row_layer_changer = 255;
      }
    }
    if (stat_matrix_changed == 1) {
      stat_matrix_changed = 0;

      if (keypress_internal_function != 0) {
        uint8_t Key_is_up = 0;
        for (uint8_t i = 0; i < 6; i++) {
          if (extra_keycodes[i] == keypress_internal_function) {
            Key_is_up = 1;
            extra_keycodes[i] = 0;
            // ESP_LOGI(KM_TAG, "pressed");
            break;
          }
        }
        if (Key_is_up == 0) {
          ESP_LOGI(KM_TAG, "asdf: %d : ", keypress_internal_function);

          // run_internal_funct();
          is_toggle_layer(keypress_internal_function);
          keypress_internal_function = 0;
        }
      }

      ESP_LOGI(KM_TAG, "layer: %d : %d %d %d %d %d %d ", current_layout,
               keycodes[0], keycodes[1], keycodes[2], keycodes[3], keycodes[4],
               keycodes[5]);
      send_hid_key();
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void keyboard_manager_init() {
  ESP_LOGI(KM_TAG, "Keyboard manager initialized");
  // init graphic parts
  // graphic_init();

  // gtext_create(0, 30, 0xff0000, 0xffffff, "Layer 0");
}
