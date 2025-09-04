/*
 * KaSe keyboard.
 *
 */

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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_DELAY_TIME_MS (3000)
/************* BL ****************/
#define CONFIG_BT_HID_DEVICE_ENABLED 1

/************* BL ****************/

static const char *TAG = "Main";

/************* TinyUSB descriptors ****************/

void app_main(void) {
  kase_tinyusb_init();
  init_cdc_commands();
  littlefs_init();
  ESP_LOGI(TAG, "LittleFS initialized");
  char *config_content = get_config_file_content();
  ESP_LOGI(TAG, "Config file content: %s", config_content);
  init_display();
  // Reset the rtc GPIOS
  rtc_matrix_deinit();
  matrix_setup();

  TaskHandle_t xHandleMatrix_Keybord = NULL;
  static uint8_t ucParameterToPass;
  xTaskCreate(vTaskKeyboard, "Matrix_Keyboard", 4096, &ucParameterToPass,
              tskIDLE_PRIORITY, &xHandleMatrix_Keybord);

  init_hid_bluetooth();

  write_text_to_display(default_layout_names[current_layout], 0, 0);
  
  for (;;) {
    if (is_layer_changed) {
      is_layer_changed = 0;
      write_text_to_display(default_layout_names[current_layout], 0, 0);
      send_data(default_layout_names[current_layout],
                strlen(default_layout_names[current_layout]));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
