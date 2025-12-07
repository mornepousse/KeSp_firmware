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
#include "keyboard_manager.h"
#include "keymap.h"
#include "littlefs_manager.h"
#include "matrix.h"
#include "usb_descriptors.h"
#include "status_display.h"
#include "esp_timer.h"


#define TEST_DELAY_TIME_MS (3000)
/************* BL ****************/
#define CONFIG_BT_HID_DEVICE_ENABLED 1

/************* BL ****************/

static const char *TAG = "Main";

static int display_sleep = 0; // 0 = on, 1 = off

/************* TinyUSB descriptors ****************/

void app_main(void) {
  ESP_LOGI(TAG, "--------------- KaSe keyboard ----------------");
  kase_tinyusb_init();
  init_cdc_commands();
  keymap_init_nvs();
  load_keymaps((uint16_t *)keymaps, LAYERS * MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t));
  load_layout_names(default_layout_names, LAYERS);
  load_macros(macros_list, MAX_MACROS);

  ESP_LOGI(TAG, "display init");
  status_display_start();
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

  // status_display_refresh_all();
  for (;;) {
    if (is_layer_changed) {
      is_layer_changed = 0;
      status_display_update_layer_name();
      cdc_send_layer(current_layout);
    }

    // Gestion veille écran : efface après 1 minute d'inactivité clavier,
    // puis rallume et réaffiche les infos à la prochaine activité.
    uint32_t now = esp_timer_get_time() / 1000;
    uint32_t last = get_last_activity_time_ms();

    // Si aucune activité depuis 60s et écran encore allumé
    if (!display_sleep && last != 0 && (now - last) > 60000) {
      status_display_sleep();
      display_sleep = 1;
    }

    // Si une activité récente et écran en veille, on réaffiche
    if (display_sleep && last != 0 && (now - last) <= 500) {
      status_display_wake();
      display_sleep = 0;
    }

    status_display_update();
    //ESP_LOGI(TAG, "boucle main");
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
