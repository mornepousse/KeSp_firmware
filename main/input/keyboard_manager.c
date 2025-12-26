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

/* Keyboard manager logging macros: enable verbose logs by defining KEYBOARD_MANAGER_DEBUG */
#ifdef KEYBOARD_MANAGER_DEBUG
#define KM_LOGI(fmt, ...) ESP_LOGI(KM_TAG, fmt, ##__VA_ARGS__)
#define KM_LOGW(fmt, ...) ESP_LOGW(KM_TAG, fmt, ##__VA_ARGS__)
#define KM_LOGD(fmt, ...) ESP_LOGD(KM_TAG, fmt, ##__VA_ARGS__)
#else
#define KM_LOGI(fmt, ...) do {} while(0)
#define KM_LOGW(fmt, ...) do {} while(0)
#define KM_LOGD(fmt, ...) do {} while(0)
#endif
#define KM_LOGE(fmt, ...) ESP_LOGE(KM_TAG, fmt, ##__VA_ARGS__)

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

typedef enum { HID_MSG_KEYBOARD = 1, HID_MSG_MOUSE = 2, HID_MSG_KB_MOUSE = 3 } hid_msg_type_t;

typedef struct {
  hid_msg_type_t type;
  TickType_t enqueue_tick; // for latency measurement
  union {
    uint8_t keycodes[6];
    struct {
      uint8_t buttons;
      int8_t x;
      int8_t y;
      int8_t wheel;
    } mouse;
    struct {
      uint8_t keycodes[6];
      uint8_t modifier;
      uint8_t buttons;
      int8_t x;
      int8_t y;
      int8_t wheel;
    } kb_mouse;
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
        // Simplified: always send combined keyboard+mouse for USB; always send both for BLE as well.
        {
          uint8_t kb_buf[6] = {0};
          uint8_t m_buttons = 0;
          int8_t m_x = 0, m_y = 0, m_wheel = 0;

          switch (msg.type) {
            case HID_MSG_KEYBOARD:
              memcpy(kb_buf, msg.payload.keycodes, sizeof(kb_buf));
              // try to consume one mouse msg if immediately available
              {
                hid_msg_t next_msg;
                if (xQueueReceive(hid_queue, &next_msg, 0) == pdTRUE) {
                  if (next_msg.type == HID_MSG_MOUSE) {
                    m_buttons = next_msg.payload.mouse.buttons;
                    m_x = next_msg.payload.mouse.x;
                    m_y = next_msg.payload.mouse.y;
                    m_wheel = next_msg.payload.mouse.wheel;
                  } else {
                    // put non-mouse back
                    if (xQueueSendToFront(hid_queue, &next_msg, 0) != pdTRUE) {
                      // ignore
                    }
                  }
                }
              }
              break;

            case HID_MSG_MOUSE:
              memcpy(kb_buf, keycodes, sizeof(kb_buf)); // snapshot current keyboard state
              m_buttons = msg.payload.mouse.buttons;
              m_x = msg.payload.mouse.x;
              m_y = msg.payload.mouse.y;
              m_wheel = msg.payload.mouse.wheel;
              break;

            case HID_MSG_KB_MOUSE:
              memcpy(kb_buf, msg.payload.kb_mouse.keycodes, sizeof(kb_buf));
              m_buttons = msg.payload.kb_mouse.buttons;
              m_x = msg.payload.kb_mouse.x;
              m_y = msg.payload.kb_mouse.y;
              m_wheel = msg.payload.kb_mouse.wheel;
              break;

            default:
              break;
          }

          if (usb_bl_state == 0) {
            if (tud_hid_ready()) {
              // always send combined report even if parts are zero
              if (!tud_hid_kb_mouse_report(REPORT_ID_KEYBOARD, REPORT_ID_MOUSE, 0, kb_buf,
                                           m_buttons, m_x, m_y, m_wheel, 0)) {
                KM_LOGW("tud_hid_kb_mouse_report failed (ready=%d q=%u)", (int)tud_hid_ready(), (unsigned)(hid_queue?uxQueueMessagesWaiting(hid_queue):0));
              }
            }
          } else {
            // BLE: send keyboard snapshot then mouse, always (even if zeros)
            send_hid_bl_key(kb_buf);
            send_hid_bl_mouse(m_buttons, m_x, m_y, m_wheel);
          }
        }
        // measure latency and log occasionally
        {
          TickType_t now = xTaskGetTickCount();
          uint32_t latency_ticks = (now >= msg.enqueue_tick) ? (now - msg.enqueue_tick) : 0;
          uint32_t latency_ms = latency_ticks * portTICK_PERIOD_MS;
          uint32_t qdepth = (hid_queue ? uxQueueMessagesWaiting(hid_queue) : 0);

          static uint32_t send_count = 0;
          static uint32_t kb_count = 0, m_count = 0, kbmouse_count = 0;
          send_count++;
          if (msg.type == HID_MSG_KEYBOARD) kb_count++;
          else if (msg.type == HID_MSG_MOUSE) m_count++;
          else if (msg.type == HID_MSG_KB_MOUSE) kbmouse_count++;

          if ((send_count & 0xFF) == 0) {
            KM_LOGI("hid stats: total=%u kb=%u mouse=%u kbmouse=%u q=%u", (unsigned)send_count, (unsigned)kb_count, (unsigned)m_count, (unsigned)kbmouse_count, (unsigned)qdepth);
          }

          if (latency_ms > 5) {
            KM_LOGW("hid_sender: type=%d latency=%u ms qdepth=%u usb_bl=%u ready=%d",
                     msg.type, (unsigned)latency_ms, (unsigned)qdepth, (unsigned)usb_bl_state, (int)tud_hid_ready());
          }
        }

        xSemaphoreGive(hid_report_mutex);
      } else {
        //ESP_LOGW(KM_TAG, "hid_sender: mutex busy, dropping HID message of type %d", msg.type);
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

/* Scan timing aggregation for diagnostics (only when there is keyboard or mouse activity) */
static uint64_t total_full_scan_us = 0;
static uint32_t full_scan_count = 0;
static uint64_t total_partial_scan_us = 0;
static uint32_t partial_scan_count = 0;
static uint32_t last_scan_report_ms = 0;
static const uint32_t SCAN_REPORT_INTERVAL_MS = 60000; // report every 60s (reduce noise)
/* track recent mouse events so scans can be measured when mouse movement occurs */
static volatile TickType_t last_mouse_event_tick = 0;
static const TickType_t MOUSE_EVENT_WINDOW = pdMS_TO_TICKS(100); // 100 ms window to attribute scan to mouse movement

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
      //ESP_LOGI(KM_TAG, "internal func: %d", keypress_internal_function);

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
    uint32_t elapsed = (uint32_t)(ticks_now - last_key_enqueue_tick);
    if (elapsed < (uint32_t)pdMS_TO_TICKS(KEY_ENQUEUE_MIN_MS)) {
      /* Too soon and identical — drop to avoid queue flood */
      return;
    }
  }

  hid_msg_t msg;
  msg.type = HID_MSG_KEYBOARD;
  msg.enqueue_tick = xTaskGetTickCount();
  memcpy(msg.payload.keycodes, keycodes, sizeof(msg.payload.keycodes));

  if (hid_queue != NULL) {
    // If there is a pending mouse msg, combine into a single KB+MOUSE message to avoid mouse starvation
    if (uxQueueMessagesWaiting(hid_queue) > 0) {
      hid_msg_t peek_msg;
      if (xQueueReceive(hid_queue, &peek_msg, 0) == pdTRUE) {
        if (peek_msg.type == HID_MSG_MOUSE) {
          // create combined message and push to front so it is handled next
          hid_msg_t combined;
          combined.type = HID_MSG_KB_MOUSE;
          combined.enqueue_tick = xTaskGetTickCount();
          memcpy(combined.payload.kb_mouse.keycodes, keycodes, sizeof(combined.payload.kb_mouse.keycodes));
          combined.payload.kb_mouse.buttons = peek_msg.payload.mouse.buttons;
          combined.payload.kb_mouse.x = peek_msg.payload.mouse.x;
          combined.payload.kb_mouse.y = peek_msg.payload.mouse.y;
          combined.payload.kb_mouse.wheel = peek_msg.payload.mouse.wheel;

          if (xQueueSendToFront(hid_queue, &combined, pdMS_TO_TICKS(5)) != pdTRUE) {
            KM_LOGW("send_hid_key: failed to enqueue combined kb_mouse (q=%u)", (unsigned)uxQueueMessagesWaiting(hid_queue));
            // put the mouse back if combined enqueue failed
            if (xQueueSendToFront(hid_queue, &peek_msg, pdMS_TO_TICKS(5)) != pdTRUE) {
              // give up
            }
            return;
          }

          memcpy(last_enqueued_keycodes, keycodes, sizeof(keycodes));
          last_key_enqueue_tick = xTaskGetTickCount();
          return;
        } else {
          // not a mouse, put it back
          if (xQueueSendToFront(hid_queue, &peek_msg, 0) != pdTRUE) {
            // ignore
          }
        }
      }
    }

    if (xQueueSend(hid_queue, &msg, pdMS_TO_TICKS(5)) != pdTRUE) {
      KM_LOGW("send_hid_key: hid_queue full, dropping keyboard report (qdepth=%u)", (unsigned)uxQueueMessagesWaiting(hid_queue));
      return;
    }
    //ESP_LOGD(KM_TAG, "send_hid_key: enqueued (qdepth=%u)", (unsigned)uxQueueMessagesWaiting(hid_queue));
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
  /* note the mouse activity time so scan timings can attribute scans to recent mouse movement */
  last_mouse_event_tick = xTaskGetTickCount();

  /* Throttle logs: only log every Nth silent sample, or always log when buttons pressed or large movement */
  static uint32_t mouse_report_cnt = 0;
  bool heavy = (buttons != 0) || (abs(x) > 8) || (abs(y) > 8);
  if (heavy || ((mouse_report_cnt++ & 0x3F) == 0)) { /* log 1/64th of normal reports */
      //ESP_LOGI(KM_TAG, "send_mouse_report: btn=0x%02X x=%d y=%d w=%d usb_bl_state=%d", buttons, x, y, wheel, usb_bl_state);
  }

  hid_msg_t msg;
  msg.type = HID_MSG_MOUSE;
  msg.enqueue_tick = xTaskGetTickCount();
  msg.payload.mouse.buttons = buttons;
  msg.payload.mouse.x = x;
  msg.payload.mouse.y = y;
  msg.payload.mouse.wheel = wheel;

  if (hid_queue != NULL) {
    /* Mouse has priority: send to front if possible. Short wait to avoid blocking */
    if (xQueueSendToFront(hid_queue, &msg, pdMS_TO_TICKS(5)) != pdTRUE) {
      /* If queue full, attempt a short-block send (we prefer mouse) */
      if (xQueueSendToFront(hid_queue, &msg, pdMS_TO_TICKS(50)) != pdTRUE) {
        KM_LOGW("send_mouse_report: hid_queue full, dropping mouse report (qdepth=%u)", (unsigned)uxQueueMessagesWaiting(hid_queue));
      }
    } else {
      //ESP_LOGD(KM_TAG, "send_mouse_report: enqueued to front (qdepth=%u)", (unsigned)uxQueueMessagesWaiting(hid_queue));
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

void send_hid_kb_mouse(uint8_t modifier, const uint8_t keycodes[6], uint8_t buttons, int8_t x, int8_t y, int8_t wheel) {
  hid_msg_t msg;
  msg.type = HID_MSG_KB_MOUSE;
  msg.enqueue_tick = xTaskGetTickCount();
  if (keycodes) memcpy(msg.payload.kb_mouse.keycodes, keycodes, sizeof(msg.payload.kb_mouse.keycodes));
  else memset(msg.payload.kb_mouse.keycodes, 0, sizeof(msg.payload.kb_mouse.keycodes));
  msg.payload.kb_mouse.modifier = modifier;
  msg.payload.kb_mouse.buttons = buttons;
  msg.payload.kb_mouse.x = x;
  msg.payload.kb_mouse.y = y;
  msg.payload.kb_mouse.wheel = wheel;

  if (hid_queue != NULL) {
    // put combined message to front to prioritize
    if (xQueueSendToFront(hid_queue, &msg, pdMS_TO_TICKS(5)) != pdTRUE) {
      // try short-block
      if (xQueueSendToFront(hid_queue, &msg, pdMS_TO_TICKS(50)) != pdTRUE) {
        //ESP_LOGW(KM_TAG, "send_hid_kb_mouse: hid_queue full, dropping combined report");
      }
    }
  } else {
    /* fallback: direct send if queue not initialized yet */
    if (usb_bl_state == 0) {
      if (tud_hid_ready()) {
        tud_hid_kb_mouse_report(REPORT_ID_KEYBOARD, REPORT_ID_MOUSE, modifier, msg.payload.kb_mouse.keycodes,
                                msg.payload.kb_mouse.buttons, msg.payload.kb_mouse.x, msg.payload.kb_mouse.y,
                                msg.payload.kb_mouse.wheel, 0);
      }
    } else {
      // fallback to separate BL sends
      send_hid_bl_key(msg.payload.kb_mouse.keycodes);
      send_hid_bl_mouse(msg.payload.kb_mouse.buttons, msg.payload.kb_mouse.x, msg.payload.kb_mouse.y, msg.payload.kb_mouse.wheel);
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
#if KEYBOARD_SCAN_DEBUG
    KM_LOGI("%d.", keycodeTMP);
#endif
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
    KM_LOGI("last_layer: %d current : %d\n", last_layer,
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
      KM_LOGI("layer: 0 %d %d %d", new_layer, keycodeTMP, TO_L0);
      // write_txt("Layer %d", n), 0, -30);
      //  gpio_set_level(CURSOR_LED_WHT_PIN, 0);
    } else {
      current_layout = new_layer;
      last_layer = current_layout;
      layer_changed();
      KM_LOGI("layer: pp %d %d %d", new_layer, keycodeTMP, TO_L0);

      // write_txt("Layer %d", n), 0, -30);

      // gpio_set_level(CURSOR_LED_WHT_PIN, 1);
    }
  }
}

void is_macro(uint16_t keycodeTMP) {
  if ((keycodeTMP >= MACRO_1) && (keycodeTMP <= MACRO_20)) {
    int16_t macro_i = (keycodeTMP - MACRO_1) / 256;
    KM_LOGI("macro: %d ", macro_i);
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

    /* Per-iteration scan timing state (used to attribute scan durations only when activity occurs) */
    uint64_t __scan_start_us = 0;
    bool __did_full_scan = false;
    bool __did_partial_scan = false;
    bool __scan_detected_change = false;  

    /* Quick heap sanity check to detect corruption early */
    if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
      size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      size_t free32 = heap_caps_get_free_size(MALLOC_CAP_32BIT);
      size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      KM_LOGE("Heap integrity FAILED in vTaskKeyboard - free8=%u free32=%u largest8=%u", (unsigned)free8, (unsigned)free32, (unsigned)largest8);
    }

    /* Wait for notification (from ISR) or timeout to do periodic/partial scan */
    /* Increased from 10ms to reduce idle CPU when using event-driven keyboard_button */
    const TickType_t wait_ticks = pdMS_TO_TICKS(50);
    uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);
    if (notified) {
      /* ISR triggered: enter burst-scan mode */
      uint32_t burst_start_ms = esp_timer_get_time() / 1000;
      uint32_t now_ms = burst_start_ms;

      while ((now_ms - burst_start_ms) < BURST_MS) {
        /* start and mark as full scan */
        __scan_start_us = esp_timer_get_time();
        __did_full_scan = true;

        /* keyboard_button reports asynchronously via callback; no full scan */
        /* capture if callback already set the change flag (will be processed below) */
        __scan_detected_change = (stat_matrix_changed == 1);

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
          KM_LOGI("%s", buf);
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
      __scan_start_us = esp_timer_get_time();
      __did_full_scan = true;
      /* final scan omitted: keyboard_button will deliver state via callback */
      __scan_detected_change = __scan_detected_change || (stat_matrix_changed == 1);
      taskYIELD(); /* hint to scheduler that others can run */

    } else {
      /* mark partial scan start and capture initial change detection */
      __scan_start_us = esp_timer_get_time();
      __did_partial_scan = true;
      /* partial scan omitted: rely on keyboard_button callback */
      __scan_detected_change = __scan_detected_change || (stat_matrix_changed == 1);
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
          // KM_LOGI("change 1\n");
          changer = 1;
          break;
        }
      }

      if (changer == 0) {
        // KM_LOGI("change 0\n");
        current_layout = last_layer;
        layer_changed();
        current_col_layer_changer = 255;
        current_row_layer_changer = 255;
      }
    }
    /* Process any matrix changes that may have been set by scans above */
    process_matrix_changes();

    /* If we performed a scan this iteration compute duration and only count it if it had activity */
    if (__did_full_scan || __did_partial_scan) {
      uint64_t scan_end_us = esp_timer_get_time();
      uint64_t scan_dur = (scan_end_us > __scan_start_us) ? (scan_end_us - __scan_start_us) : 0;
      if (scan_dur > 2000) {
        KM_LOGW("scan took %llu us", (unsigned long long)scan_dur);
      }

      bool scan_event = __scan_detected_change;
      /* check for keycodes (non-zero) */
      if (!scan_event) {
        for (int k = 0; k < 6; k++) {
          if (keycodes[k] != 0) { scan_event = true; break; }
        }
      }
      /* check recent mouse activity */
      if (!scan_event && last_mouse_event_tick != 0 && (xTaskGetTickCount() - last_mouse_event_tick) <= MOUSE_EVENT_WINDOW) {
        scan_event = true;
      }

      if (scan_event) {
        if (__did_full_scan) {
          total_full_scan_us += scan_dur;
          full_scan_count++;
        } else {
          total_partial_scan_us += scan_dur;
          partial_scan_count++;
        }

        /* rate-limited visible log: log only 1/16 events to reduce noise, always log if scan longer than 1ms */
        static uint32_t scan_event_log_count = 0;
        scan_event_log_count++;
        bool mouse_recent = (last_mouse_event_tick != 0 && (xTaskGetTickCount() - last_mouse_event_tick) <= MOUSE_EVENT_WINDOW);
        bool keycodes_nonzero = false;
        for (int k = 0; k < 6; k++) { if (keycodes[k] != 0) { keycodes_nonzero = true; break; } }
        if ((scan_event_log_count & 0xF) == 0 || scan_dur > 1000) { /* 1/16 events or long scan */
          KM_LOGI("SCAN EVENT: %s dur=%llu us keys=%d mouse_recent=%d change=%d",
                   (__did_full_scan ? "FULL" : "PARTIAL"), (unsigned long long)scan_dur, (int)keycodes_nonzero, (int)mouse_recent, (int)__scan_detected_change);
        }
      }

      /* reset local flags */
      __did_full_scan = __did_partial_scan = false;
      __scan_detected_change = false;
      __scan_start_us = 0;
    }

    /* Periodic scan timing report */
    {
      uint32_t now_ms = esp_timer_get_time() / 1000;
      if (last_scan_report_ms == 0) last_scan_report_ms = now_ms;
      if ((now_ms - last_scan_report_ms) >= SCAN_REPORT_INTERVAL_MS) {
        if (full_scan_count > 0) {
          uint64_t avg_full = total_full_scan_us / full_scan_count;
          KM_LOGI("Scan avg FULL (events only): %llu us over %u samples", (unsigned long long)avg_full, full_scan_count);
        }

        if (partial_scan_count > 0) {
          uint64_t avg_partial = total_partial_scan_us / partial_scan_count;
          KM_LOGI("Scan avg PARTIAL (events only): %llu us over %u samples", (unsigned long long)avg_partial, partial_scan_count);
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
  KM_LOGI("Keyboard manager initialized");

  /* Create hid queue + mutex + hid sender task to serialize TinyUSB calls */
  if (hid_queue == NULL) {
    hid_queue = xQueueCreate(32, sizeof(hid_msg_t));
    if (hid_queue == NULL) {
      KM_LOGW("Failed to create hid_queue");
    }
  }
  if (hid_report_mutex == NULL) {
    hid_report_mutex = xSemaphoreCreateMutex();
    if (hid_report_mutex == NULL) {
      KM_LOGW("Failed to create hid_report_mutex");
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

