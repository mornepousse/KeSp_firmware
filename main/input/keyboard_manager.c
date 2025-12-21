#include "keyboard_manager.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hid_bluetooth_manager.h"
#include "key_definitions.h"
#include "keyboard_config.h"
#include "keymap.h"
#include "matrix.h"
#include "tinyusb.h"
#include "status_display.h"
#include "keyboard_worker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
// #include "graphic_manager.h"

/* Exported task handle so matrix ISR can notify keyboard task */
TaskHandle_t keyboard_task_handle = NULL;


static const char *KM_TAG = "Keyboard_manager";

/* Enable verbose debug traces for scanning (set to 1 to enable) */
#ifndef KEYBOARD_SCAN_DEBUG
#define KEYBOARD_SCAN_DEBUG 0
#endif

// Report IDs - must match usb_descriptors.c
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE 2

/* extern state used by HID sender (defined below) */
extern uint8_t usb_bl_state;

/* HID send queue and sender task to serialize TinyUSB calls and avoid contention
 * We enqueue keyboard and mouse reports and let a single HID sender task perform
 * the actual tud_hid_* calls. Mouse reports are queued to the front for priority. */

typedef enum { HID_MSG_KEYBOARD = 1, HID_MSG_MOUSE = 2 } hid_msg_type_t;

typedef struct {
  hid_msg_type_t type;
  union {
    uint8_t keycodes[6];
    struct {
      uint8_t buttons;
      int8_t x;
      int8_t y;
      int8_t wheel;
    } mouse;
  } payload;
} hid_msg_t;

static QueueHandle_t hid_queue = NULL;
static SemaphoreHandle_t hid_report_mutex = NULL;
static TaskHandle_t hid_sender_task_handle = NULL;

static void hid_sender_task(void *pvParameters) {
  (void)pvParameters;
  hid_msg_t msg;
  for (;;) {
    if (xQueueReceive(hid_queue, &msg, portMAX_DELAY) == pdTRUE) {
      /* Defensive mutex to protect TinyUSB calls, block up to 100 ms */
      if (xSemaphoreTake(hid_report_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (msg.type == HID_MSG_KEYBOARD) {
          if (usb_bl_state == 0) {
            if (tud_hid_ready()) {
              tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, msg.payload.keycodes);
            }
          } else {
            send_hid_bl_key(msg.payload.keycodes);
          }
        } else if (msg.type == HID_MSG_MOUSE) {
          if (usb_bl_state == 0) {
            if (tud_hid_ready()) {
              tud_hid_mouse_report(REPORT_ID_MOUSE, msg.payload.mouse.buttons,
                                   msg.payload.mouse.x, msg.payload.mouse.y,
                                   msg.payload.mouse.wheel, 0);
            }
          } else {
            send_hid_bl_mouse(msg.payload.mouse.buttons,
                              msg.payload.mouse.x,
                              msg.payload.mouse.y,
                              msg.payload.mouse.wheel);
          }
        }
        xSemaphoreGive(hid_report_mutex);
      } else {
        ESP_LOGW(KM_TAG, "hid_sender: mutex busy, dropping HID message of type %d", msg.type);
      }
    }
  }
}

/* Forward declarations for helpers used by process_matrix_changes */
void run_internal_funct(void);
void is_toggle_layer(uint16_t keycodeTMP);
void is_momentary_layer(int16_t keycodeTMP, uint8_t i);
bool is_internal_function(int16_t keycodeTMP);
void is_macro(uint16_t keycodeTMP);

uint8_t bl_state = 0;
uint8_t usb_bl_state = 0; // 0: USB, 1: BL
uint16_t keypress_internal_function = 0;
uint8_t current_row_layer_changer = 255;
uint8_t current_col_layer_changer = 255;

uint16_t extra_keycodes[6] = {0, 0, 0, 0, 0, 0};

/* Scan timing aggregation for diagnostics */
static uint64_t total_full_scan_us = 0;
static uint32_t full_scan_count = 0;
static uint64_t total_partial_scan_us = 0;
static uint32_t partial_scan_count = 0;
static uint32_t last_scan_report_ms = 0;
static const uint32_t SCAN_REPORT_INTERVAL_MS = 60000; // report every 60s to reduce log noise

/* Follow-up / burst scan config to better detect quick presses when mashing */
#ifndef FOLLOW_UP_SCANS
#define FOLLOW_UP_SCANS 6
#endif
#ifndef FOLLOW_UP_INTERVAL_MS
#define FOLLOW_UP_INTERVAL_MS 2
#endif

#ifndef BURST_MS
#define BURST_MS 40
#endif
#ifndef BURST_SCAN_INTERVAL_MS
#define BURST_SCAN_INTERVAL_MS 2
#endif

/* Helper to process matrix changes (internal functions and send) */
static void process_matrix_changes(void)
{
  if (keypress_internal_function != 0) {
    uint8_t Key_is_up = 0;
    for (uint8_t i = 0; i < 6; i++) {
      if (extra_keycodes[i] == keypress_internal_function) {
        Key_is_up = 1;
        extra_keycodes[i] = 0;
        break;
      }
    }
    if (Key_is_up == 0) {
      ESP_LOGI(KM_TAG, "internal func: %d", keypress_internal_function);

      run_internal_funct();
      is_toggle_layer(keypress_internal_function);
      keypress_internal_function = 0;
    }
  }
}

/* Build keycodes array from current_press_* and apply macros/layers */
static void build_keycode_report(void)
{
  for (uint8_t i = 0; i < 6; i++) {
    if (current_press_col[i] != 255) {
      uint16_t keycodeTMP = keymaps[current_layout][current_press_row[i]][current_press_col[i]];

      is_momentary_layer(keycodeTMP, i);
      is_internal_function(keycodeTMP);
      is_macro(keycodeTMP);

      if (current_row_layer_changer == current_press_row[i] &&
          current_col_layer_changer == current_press_col[i]) {
        // layer changer pressed -> skip mapping
      } else {
        if (keycodeTMP == K_NO) {
          keycodeTMP = keymaps[last_layer][current_press_row[i]][current_press_col[i]];
        }
        if (keycodeTMP > 255) {
          extra_keycodes[i] = keycodeTMP;
        } else {
          keycodes[i] = keycodeTMP;
        }
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
        changer = 1;
        break;
      }
    }

    if (changer == 0) {
      current_layout = last_layer;
      layer_changed();
      current_col_layer_changer = 255;
      current_row_layer_changer = 255;
    }
  }
}




#ifndef KEY_ENQUEUE_MIN_MS
#define KEY_ENQUEUE_MIN_MS 8
#endif

void send_hid_key() {
  static uint8_t last_enqueued_keycodes[6] = {0};
  static TickType_t last_key_enqueue_tick = 0;

  /* Coalesce identical key reports to avoid saturating HID queue during bursts */
  if (memcmp(keycodes, last_enqueued_keycodes, sizeof(keycodes)) == 0) {
    TickType_t ticks_now = xTaskGetTickCount();
    TickType_t elapsed = ticks_now - last_key_enqueue_tick;
    if (elapsed < pdMS_TO_TICKS(KEY_ENQUEUE_MIN_MS)) {
      /* Too soon and identical — drop to avoid queue flood */
      return;
    }
  }

  hid_msg_t msg;
  msg.type = HID_MSG_KEYBOARD;
  memcpy(msg.payload.keycodes, keycodes, sizeof(msg.payload.keycodes));

  if (hid_queue != NULL) {
    if (xQueueSend(hid_queue, &msg, pdMS_TO_TICKS(5)) != pdTRUE) {
      ESP_LOGW(KM_TAG, "send_hid_key: hid_queue full, dropping keyboard report");
      return;
    }
    memcpy(last_enqueued_keycodes, keycodes, sizeof(keycodes));
    last_key_enqueue_tick = xTaskGetTickCount();
  } else {
    /* fallback: direct send if queue not initialized yet */
    if (usb_bl_state == 0) {
      if (tud_hid_ready()) {
        tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keycodes);
      }
    } else {
      send_hid_bl_key(keycodes);
    }
    memcpy(last_enqueued_keycodes, keycodes, sizeof(keycodes));
    last_key_enqueue_tick = xTaskGetTickCount();
  }
}

void send_mouse_report(uint8_t buttons, int8_t x, int8_t y, int8_t wheel) {
  /* Throttle logs: only log every Nth silent sample, or always log when buttons pressed or large movement */
  static uint32_t mouse_report_cnt = 0;
  bool heavy = (buttons != 0) || (abs(x) > 8) || (abs(y) > 8);
  if (heavy || ((mouse_report_cnt++ & 0x3F) == 0)) { /* log 1/64th of normal reports */
      ESP_LOGI(KM_TAG, "send_mouse_report: btn=0x%02X x=%d y=%d w=%d usb_bl_state=%d", buttons, x, y, wheel, usb_bl_state);
  }

  hid_msg_t msg;
  msg.type = HID_MSG_MOUSE;
  msg.payload.mouse.buttons = buttons;
  msg.payload.mouse.x = x;
  msg.payload.mouse.y = y;
  msg.payload.mouse.wheel = wheel;

  if (hid_queue != NULL) {
    /* Mouse has priority: send to front if possible. Short wait to avoid blocking */
    if (xQueueSendToFront(hid_queue, &msg, pdMS_TO_TICKS(5)) != pdTRUE) {
      /* If queue full, attempt a short-block send (we prefer mouse) */
      if (xQueueSendToFront(hid_queue, &msg, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(KM_TAG, "send_mouse_report: hid_queue full, dropping mouse report");
      }
    }
  } else {
    /* fallback: direct send if queue not initialized yet */
    if (usb_bl_state == 0) {
      if (tud_hid_ready()) {
        tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, x, y, wheel, 0);
      }
    } else {
      send_hid_bl_mouse(buttons, x, y, wheel);
    }
  }
}

uint8_t keyboard_get_usb_bl_state(void) {
  return usb_bl_state;
}

// code degueu....

void run_internal_funct() {

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
    km_post_bt_toggle();
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
    if (macro_i >= 0 && macro_i < MAX_MACROS && macros_list[macro_i].name[0] != '\0') {
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
    /* Register own task handle for ISR notifications */
    if (keyboard_task_handle == NULL) {
      keyboard_task_handle = xTaskGetCurrentTaskHandle();
    }

    /* Quick heap sanity check to detect corruption early */
    if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
      size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      size_t free32 = heap_caps_get_free_size(MALLOC_CAP_32BIT);
      size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      ESP_LOGE(KM_TAG, "Heap integrity FAILED in vTaskKeyboard - free8=%u free32=%u largest8=%u", (unsigned)free8, (unsigned)free32, (unsigned)largest8);
    }

    /* Wait for notification (from ISR) or timeout to do periodic/partial scan */
    const TickType_t wait_ticks = pdMS_TO_TICKS(10);
    uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);
    if (notified) {
      /* ISR triggered: enter burst-scan mode */
      uint32_t burst_start_ms = esp_timer_get_time() / 1000;
      uint32_t now_ms = burst_start_ms;

      while ((now_ms - burst_start_ms) < BURST_MS) {
        uint64_t scan_start = esp_timer_get_time();
        scan_matrix_full_once();
        uint64_t scan_end = esp_timer_get_time();
        uint64_t scan_dur = scan_end - scan_start;
        if (scan_dur > 2000) {
          ESP_LOGW(KM_TAG, "burst scan_matrix_full_once took %llu us", (unsigned long long)scan_dur);
        }
        total_full_scan_us += scan_dur;
        full_scan_count++;

#if KEYBOARD_SCAN_DEBUG
        /* Debug: dump current presses and timestamp (safe, bounded) */
        {
          uint32_t now = esp_timer_get_time() / 1000;
          char buf[128];
          int off = snprintf(buf, sizeof(buf), "scan@%u ms:", now);
          for (int k = 0; k < 6; k++) {
            int rem = (int)sizeof(buf) - off;
            if (rem <= 1) break; /* no space left */
            int n = snprintf(buf + off, rem, " [%d,%d]", current_press_row[k], current_press_col[k]);
            if (n < 0) break;
            if (n >= rem) { off = (int)sizeof(buf) - 1; break; }
            off += n;
          }
          buf[sizeof(buf) - 1] = '\0';
          ESP_LOGI(KM_TAG, "%s", buf);
        }
#endif

        /* Build report and process internal funcs/releases */
        build_keycode_report();
        if (stat_matrix_changed == 1) {
          stat_matrix_changed = 0;
          process_matrix_changes();
        }

        /* Send HID only when keycodes changed to reduce redundant reports */
        send_hid_key();
        taskYIELD(); /* give NRF task a chance to run if it was contending */

        vTaskDelay(pdMS_TO_TICKS(BURST_SCAN_INTERVAL_MS));
        now_ms = esp_timer_get_time() / 1000;
      }

      /* final scan to ensure states are stable */
      uint64_t scan_start = esp_timer_get_time();
      scan_matrix_full_once();
      uint64_t scan_end = esp_timer_get_time();
      uint64_t scan_dur = scan_end - scan_start;
      total_full_scan_us += scan_dur;
      full_scan_count++;
      taskYIELD(); /* hint to scheduler that others can run */

    } else {
      uint64_t scan_start = esp_timer_get_time();
      scan_matrix();
      uint64_t scan_end = esp_timer_get_time();
      uint64_t scan_dur = scan_end - scan_start;
      if (scan_dur > 2000) {
        ESP_LOGW(KM_TAG, "scan_matrix took %llu us", (unsigned long long)scan_dur);
      }
      total_partial_scan_us += scan_dur;
      partial_scan_count++;
    }
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
    /* Process any matrix changes that may have been set by scans above */
    process_matrix_changes();

    /* Periodic scan timing report */
    {
      uint32_t now_ms = esp_timer_get_time() / 1000;
      if (last_scan_report_ms == 0) last_scan_report_ms = now_ms;
      if ((now_ms - last_scan_report_ms) >= SCAN_REPORT_INTERVAL_MS) {
        if (full_scan_count > 0) {
          uint64_t avg_full = total_full_scan_us / full_scan_count;
          ESP_LOGD(KM_TAG, "Scan avg FULL: %llu us over %u samples", (unsigned long long)avg_full, full_scan_count);
        }

        if (partial_scan_count > 0) {
          uint64_t avg_partial = total_partial_scan_us / partial_scan_count;
          ESP_LOGD(KM_TAG, "Scan avg PARTIAL: %llu us over %u samples", (unsigned long long)avg_partial, partial_scan_count);
        }

        /* reset counters */
        total_full_scan_us = 0;
        full_scan_count = 0;
        total_partial_scan_us = 0;
        partial_scan_count = 0;
        last_scan_report_ms = now_ms;
      }
    }

  }
}

void keyboard_manager_init() {
  ESP_LOGI(KM_TAG, "Keyboard manager initialized");

  /* Create hid queue + mutex + hid sender task to serialize TinyUSB calls */
  if (hid_queue == NULL) {
    hid_queue = xQueueCreate(32, sizeof(hid_msg_t));
    if (hid_queue == NULL) {
      ESP_LOGW(KM_TAG, "Failed to create hid_queue");
    }
  }
  if (hid_report_mutex == NULL) {
    hid_report_mutex = xSemaphoreCreateMutex();
    if (hid_report_mutex == NULL) {
      ESP_LOGW(KM_TAG, "Failed to create hid_report_mutex");
    }
  }
  if (hid_sender_task_handle == NULL) {
    xTaskCreatePinnedToCore(hid_sender_task, "hid_sender", 4096, NULL, 4, &hid_sender_task_handle, 0);
  }

  /* Start keyboard worker to handle display / BT operations off-task */
  keyboard_worker_init();

  // init graphic parts
  // graphic_init();

  // gtext_create(0, 30, 0xff0000, 0xffffff, "Layer 0");
}
