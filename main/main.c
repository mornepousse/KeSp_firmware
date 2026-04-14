/*
 * KeSp — keyboard firmware framework
 */
#include <stdint.h>
#include "cdc_acm_com.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_bluetooth_manager.h"
#include "keyboard_task.h"
#include "keymap.h"
#include "matrix_scan.h"
#include "key_stats.h"
#include "usb_hid.h"
#include "status_display.h"
#include "display_backend.h"
#include "cpu_time.h"
#include "led_strip_anim.h"
#include "tama_engine.h"
#include "key_features.h"

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
    if (request_wake_request) {
      request_wake_request = false;
      status_display_refresh_all();
    }

    status_display_update();
    
    /* Periodically save stats + tick WPM every second */
    key_stats_check_save();
    {
      static uint32_t last_wpm_tick = 0;
      uint32_t now_wpm = esp_timer_get_time() / 1000;
      if (now_wpm - last_wpm_tick > 1000) {
        wpm_tick();
        last_wpm_tick = now_wpm;
      }
    }

    /* Save tama every 60s if changed */
    {
      static uint32_t last_tama_save = 0;
      static uint32_t last_tama_keys = 0;
      const tama2_stats_t *ts = tama_engine_get_stats();
      if (ts && ts->total_keys != last_tama_keys) {
        uint32_t now = esp_timer_get_time() / 1000;
        if (now - last_tama_save > 60000) {
          tama_engine_save();
          last_tama_save = now;
          last_tama_keys = ts->total_keys;
        }
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));  // 100ms polling — fast enough for UI, no keyboard lag
  }
}

/* Boot crash detection: RTC memory survives soft reboot but not power cycle */
static RTC_NOINIT_ATTR uint32_t boot_crash_count;
static RTC_NOINIT_ATTR uint32_t boot_crash_magic;
#define BOOT_CRASH_MAGIC 0xB007FA11
#define BOOT_CRASH_LIMIT 3

static bool safe_mode = false;

void app_main(void) {
  ESP_LOGI(TAG, "--------------- KeSp [%s] ----------------", PRODUCT_NAME);

  /* Crash-loop detection — RTC memory is random on first power-on,
     so we validate with a magic number AND a reasonable count range */
  if (boot_crash_magic != BOOT_CRASH_MAGIC || boot_crash_count > 100) {
      boot_crash_magic = BOOT_CRASH_MAGIC;
      boot_crash_count = 0;
  }
  boot_crash_count++;
  ESP_LOGI(TAG, "Boot count: %lu", (unsigned long)boot_crash_count);

  if (boot_crash_count > BOOT_CRASH_LIMIT) {
      ESP_LOGW(TAG, "Crash loop detected (%lu boots) — SAFE MODE", (unsigned long)boot_crash_count);
      safe_mode = true;
      boot_crash_count = 0;
      /* Don't erase NVS — user data is precious.
         Safe mode just skips display/BLE/NVS loading. */
  }

  kase_tinyusb_init();
  init_cdc_commands();

  /* Register binary CDC protocol handlers */
  {
    extern void cdc_binary_cmds_init(void);
    cdc_binary_cmds_init();
  }

  keymap_init_nvs();

  if (!safe_mode) {
    load_keymaps((uint16_t *)keymaps, LAYERS * MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t));
    load_layout_names(default_layout_names, LAYERS);
    load_macros(macros_list, MAX_MACROS);
    load_key_stats();
    load_bigram_stats();

    /* Load advanced feature configs */
    extern void tap_dance_load(void);
    extern void combo_load(void);
    extern void leader_load(void);
    extern void tama_engine_init(void);
    tap_dance_load();
    combo_load();
    leader_load();
    key_override_load();
    bt_devices_load();
    tama_engine_init();
  } else {
    ESP_LOGW(TAG, "Safe mode: skipping NVS config loading");
  }

  if (!safe_mode) {
    ESP_LOGI(TAG, "display init");
#if !SKIP_STATUS_DISPLAY
    {
      extern const display_backend_t
#ifdef BOARD_DISPLAY_BACKEND_ROUND
        round_display_backend;
      display_set_backend(&round_display_backend);
#else
        oled_display_backend;
      display_set_backend(&oled_display_backend);
#endif
    }
    status_display_start();
    xTaskCreatePinnedToCore(status_display_task, "status_disp", 6144, NULL, 2, &status_display_task_handle, 1);
#endif
  } else {
    ESP_LOGW(TAG, "Safe mode: skipping display");
  }

  rtc_matrix_deinit();
  ESP_LOGI(TAG, "Matrix setup init");
  matrix_setup();

  ESP_LOGI(TAG, "Keyboard manager init");
  keyboard_manager_init();

  ESP_LOGI(TAG, "Task Matrix init");
  TaskHandle_t xHandleMatrixKeyboard = NULL;
  static uint8_t ucParameterToPass;
  xTaskCreatePinnedToCore(vTaskKeyboard, "Matrix_Keyboard", 6144, &ucParameterToPass,
              3, &xHandleMatrixKeyboard, 0);

  if (!safe_mode) {
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
  }

  xTaskCreatePinnedToCore(cpu_time_logger_task, "cpu_time", 4096, NULL, 2, NULL, 1);

  /* Boot succeeded — reset crash counter and validate OTA */
  boot_crash_count = 0;
  esp_ota_mark_app_valid_cancel_rollback();
  ESP_LOGI(TAG, "Boot OK%s", safe_mode ? " (SAFE MODE)" : "");

  for (;;) {
    // keep main light; display handled in its own task
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
