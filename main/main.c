/*
 * KaSe keyboard.
 *
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "cdc_acm_com.h"
#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_bluetooth_manager.h"
#include "i2c_oled_display.h"
#include "keyboard_manager.h"
#include "keymap.h"
#include "littlefs_manager.h"
#include "matrix.h"
#include "usb_descriptors.h"


#define TEST_DELAY_TIME_MS (3000)
/************* BL ****************/
#define CONFIG_BT_HID_DEVICE_ENABLED 1

/************* BL ****************/

static const char *TAG = "Main";

/************* TinyUSB descriptors ****************/

void app_main(void) {
  ESP_LOGI(TAG, "--------------- KaSe keyboard ----------------");
  kase_tinyusb_init();
  init_cdc_commands();
  keymap_init_nvs();
  load_keymaps((uint16_t *)keymaps, LAYERS * MATRIX_ROWS * MATRIX_COLS);
  ESP_LOGI(TAG, "display init");
  init_display();
  // Reset the rtc GPIOS
  rtc_matrix_deinit();
  ESP_LOGI(TAG, "Matrix setup init");
  matrix_setup();

  ESP_LOGI(TAG, "Task Matrix init");
  TaskHandle_t xHandleMatrix_Keybord = NULL;
  static uint8_t ucParameterToPass;
  xTaskCreate(vTaskKeyboard, "Matrix_Keyboard", 4096, &ucParameterToPass,
              tskIDLE_PRIORITY, &xHandleMatrix_Keybord);
  ESP_LOGI(TAG, "bluetooth init");
  init_hid_bluetooth();

  write_text_to_display(default_layout_names[current_layout], 0, 0);
  int t = 1000;
  for (;;) {
    if (is_layer_changed) {
      is_layer_changed = 0;
      draw_rectangle(0, 0, 128, 16);
      write_text_to_display(default_layout_names[current_layout], 0, 0);
      cdc_send_layer(current_layout);
    }
    ESP_LOGI(TAG, "boucle main");
vTaskDelay(pdMS_TO_TICKS(10));
    t--;
    if(t<=0)
    {
      ESP_LOGI(TAG, "coucou");
      t = 1000;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
