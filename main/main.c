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
#include "esp_heap_caps.h"
#include "nrf24_receiver.h"
#include "cpu_time.h"


#define TEST_DELAY_TIME_MS (3000)

/* Runtime debug/experimental flags: set to 1 to skip starting the component for isolation testing */
#ifndef SKIP_NRF_TASK
#define SKIP_NRF_TASK 0
#endif
#ifndef SKIP_STATUS_DISPLAY
#define SKIP_STATUS_DISPLAY 0
#endif
/************* BL ****************/
#define CONFIG_BT_HID_DEVICE_ENABLED 1

/************* BL ****************/

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

/* Small diagnostic task: regularly checks heap integrity and logs free sizes so we can catch corruption early */
static void heap_diag_task(void *arg)
{
    (void)arg;
    /* External task handles (keyboard and nrf task provide their own exported handles) */
    extern TaskHandle_t keyboard_task_handle;
    extern TaskHandle_t nrf24_task_handle;
    extern TaskHandle_t status_display_task_handle; /* defined in this file when creating the task */

    for (;;) {
        if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
            size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            size_t free32 = heap_caps_get_free_size(MALLOC_CAP_32BIT);
            size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            ESP_LOGE(TAG, "HEAP_DIAG: integrity FAILED - free8=%u free32=%u largest8=%u", (unsigned)free8, (unsigned)free32, (unsigned)largest8);
        } else {
            static int cnt = 0;
            if ((cnt++ & 31) == 0) {
                size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                size_t free32 = heap_caps_get_free_size(MALLOC_CAP_32BIT);
                ESP_LOGI(TAG, "HEAP_DIAG: free8=%u free32=%u", (unsigned)free8, (unsigned)free32);
            }
        }

        /* Log stack high-water marks for critical tasks at a reduced frequency to avoid log noise */
        static int stack_diag_cnt = 0;
        if ((stack_diag_cnt++ & 7) == 0) { /* every 8 loops (~4s) */
            TaskHandle_t handles[] = { keyboard_task_handle, nrf24_task_handle, status_display_task_handle };
            const char *names[] = { "keyboard", "nrf24", "status_display" };
            for (int i = 0; i < sizeof(handles)/sizeof(handles[0]); i++) {
                if (handles[i] != NULL) {
                    UBaseType_t words_left = uxTaskGetStackHighWaterMark(handles[i]);
                    size_t bytes_left = words_left * sizeof(StackType_t);
                    ESP_LOGI(TAG, "STACK_DIAG: %s stack free ~= %u bytes", names[i], (unsigned)bytes_left);
                    /* Warn if stack remaining too low (e.g. < 256 bytes) */
                    if (bytes_left < 256) {
                        ESP_LOGW(TAG, "STACK_DIAG: %s stack low (%u bytes left)", names[i], (unsigned)bytes_left);
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Task handling status display updates, sleep/wake and layer change handling.
static void status_display_task(void *arg) {
  (void)arg;
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

    if (!display_sleep && last != 0 && (now - last) > 60000) {
      status_display_sleep();
      display_sleep = 1;
    }

    if (display_sleep && last != 0 && (now - last) <= 500) {
      status_display_wake();
      display_sleep = 0;
    }

    /* Quick heap sanity check to avoid calling LVGL if heap is corrupt */
    if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
      ESP_LOGE(TAG, "Heap integrity FAILED in status_display_task loop - disabling display to avoid crash");
      status_display_force_disable();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    /* Handle deferred wake requests in display task context */
    extern bool request_wake_request;
    if (request_wake_request) {
      request_wake_request = false;
      status_display_refresh_all();
    }

    status_display_update();
    vTaskDelay(pdMS_TO_TICKS(400));
  }
}

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
#if MATRIX_IRQ_ENABLED
  /* IRQ setup is handled by the keyboard_button shim; no explicit call needed here. */
#endif

  ESP_LOGI(TAG, "Task Matrix init");
  TaskHandle_t xHandleMatrix_Keybord = NULL;
  static uint8_t ucParameterToPass;
  // Keyboard on CPU 0, priority 3 (below LVGL=4)
  /* Increase keyboard task stack to reduce stack pressure (was 4096) */
  xTaskCreatePinnedToCore(vTaskKeyboard, "Matrix_Keyboard", 6144, &ucParameterToPass,
              3, &xHandleMatrix_Keybord, 0);
  ESP_LOGI(TAG, "bluetooth init");
  init_hid_bluetooth();

  ESP_LOGI(TAG, "NRF24 init");
#if !SKIP_NRF_TASK
  // NRF24 on CPU 1, priority 3 (won't block IDLE on CPU 1)
  /* Increase nrf24 task stack to reduce stack pressure (was 4096) */
  xTaskCreatePinnedToCore(nrf24_task, "nrf24_task", 6144, NULL, 3, NULL, 1);
#else
  ESP_LOGW(TAG, "SKIP_NRF_TASK enabled: NRF24 task not started");
#endif

  // Start periodic CPU usage logger on core 1 (avoid interfering with keyboard task on core 0)
  xTaskCreatePinnedToCore(cpu_time_logger_task, "cpu_time", 4096, NULL, 2, NULL, 1);

  // status_display_refresh_all();

  /* Start heap diagnostic task to catch heap corruption early */
  /* Increase stack to avoid overflow (logs and system calls can use significant stack) */
  //xTaskCreatePinnedToCore(heap_diag_task, "heap_diag", 4096, NULL, 1, NULL, 1);

  for (;;) {
    // keep main light; display handled in its own task
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
