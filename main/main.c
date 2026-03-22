/*
 * KeSp — keyboard firmware framework
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
#include "usb_hid.h"
#include "status_display.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cpu_time.h"
#include "led_strip_anim.h"

/* Runtime debug/experimental flags: set to 1 to skip starting the component for isolation testing */
#ifndef SKIP_STATUS_DISPLAY
#define SKIP_STATUS_DISPLAY 0
#endif

static const char *TAG = "Main";

static int display_sleep = 0; // 0 = on, 1 = off

/* Task handle exported for diagnostics: status display task */
TaskHandle_t status_display_task_handle = NULL;

static void cpu_time_logger_task(void *arg) {
  (void)arg;
  char buf[512];
  for (;;) {
    if (cpu_time_measure_period(1000, buf, sizeof(buf)) == 0) {
      ESP_LOGI(TAG, "CPU usage:\n%s", buf);
    } else {
      ESP_LOGW(TAG, "cpu_time_measure_period failed");
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// Task handling status display updates, sleep/wake and layer change handling.
static uint8_t last_displayed_layer = 255;  // Track what layer is currently shown

static void status_display_task(void *arg) {
  (void)arg;
  for (;;) {
    // Check both the flag AND if the displayed layer matches current
    // This catches rapid layer changes that might set/clear the flag quickly
    if (is_layer_changed || (last_displayed_layer != current_layout)) {
      is_layer_changed = 0;
      last_displayed_layer = current_layout;
      status_display_update_layer_name();
      cdc_send_layer(current_layout);
    }

    /* Display sleep: turn off after inactivity, wake on next keypress */
    uint32_t now = esp_timer_get_time() / 1000;
    uint32_t last = get_last_activity_time_ms();

    if (!display_sleep && last != 0 && (now - last) > BOARD_DISPLAY_SLEEP_MS) {
      status_display_sleep();
      display_sleep = 1;
    }

    if (display_sleep && last != 0 && (now - last) <= 500) {
      status_display_wake();
      display_sleep = 0;
    }

    /* Handle deferred wake requests in display task context */
    extern bool request_wake_request;
    if (request_wake_request) {
      request_wake_request = false;
      status_display_refresh_all();
    }

    status_display_update();
    
    /* Periodically check if key stats need saving */
    key_stats_check_save();
    
    vTaskDelay(pdMS_TO_TICKS(10));  // 10ms polling for responsive layer display
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "--------------- KeSp [%s] ----------------", PRODUCT_NAME);
  kase_tinyusb_init();
  init_cdc_commands();
  keymap_init_nvs();
  load_keymaps((uint16_t *)keymaps, LAYERS * MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t));
  load_layout_names(default_layout_names, LAYERS);
  load_macros(macros_list, MAX_MACROS);
  load_key_stats();
  load_bigram_stats();

  ESP_LOGI(TAG, "display init");
#if !SKIP_STATUS_DISPLAY
  status_display_start();
  // Start status display task on core 1 to avoid interfering with keyboard
  xTaskCreatePinnedToCore(status_display_task, "status_disp", 6144, NULL, 4, &status_display_task_handle, 1);
#else
  ESP_LOGW(TAG, "SKIP_STATUS_DISPLAY enabled: display task not started");
#endif
  // Reset the rtc GPIOS
  rtc_matrix_deinit();
  ESP_LOGI(TAG, "Matrix setup init");
  matrix_setup();

  ESP_LOGI(TAG, "Task Matrix init");
  TaskHandle_t xHandleMatrixKeyboard = NULL;
  static uint8_t ucParameterToPass;
  // Keyboard on CPU 0, priority 3 (below LVGL=4)
  /* Increase keyboard task stack to reduce stack pressure (was 4096) */
  xTaskCreatePinnedToCore(vTaskKeyboard, "Matrix_Keyboard", 6144, &ucParameterToPass,
              3, &xHandleMatrixKeyboard, 0);
  ESP_LOGI(TAG, "bluetooth init check");
  if (load_bt_state()) {
      ESP_LOGI(TAG, "Starting bluetooth (saved state: ON)");
      init_hid_bluetooth();
  } else {
      ESP_LOGI(TAG, "Bluetooth disabled (saved state: OFF)");
  }

#if BOARD_HAS_LED_STRIP
  ESP_LOGI(TAG, "LED Strip init");
  led_strip_test();
  led_strip_start_task();
#endif

  // Start periodic CPU usage logger on core 1 (avoid interfering with keyboard task on core 0)
  xTaskCreatePinnedToCore(cpu_time_logger_task, "cpu_time", 4096, NULL, 2, NULL, 1);

  for (;;) {
    // keep main light; display handled in its own task
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
