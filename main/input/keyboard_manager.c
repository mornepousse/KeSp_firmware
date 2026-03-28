#include "keyboard_manager.h"
#include "hid_transport.h"
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

// Report IDs - must match usb_hid.c
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
    struct {
        uint8_t keycodes[6];
        uint8_t modifier;
    } keyboard;
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
static uint8_t current_modifiers = 0;

static void extract_modifiers(uint8_t *keycodes, uint8_t *modifier) {
    *modifier = 0;
    for (int i = 0; i < 6; i++) {
        if (keycodes[i] >= HID_KEY_CONTROL_LEFT && keycodes[i] <= HID_KEY_GUI_RIGHT) {
            *modifier |= (1 << (keycodes[i] - HID_KEY_CONTROL_LEFT));
            keycodes[i] = 0;
        }
    }
}

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
          uint8_t kb_mod = 0;
          uint8_t m_buttons = 0;
          int8_t m_x = 0, m_y = 0, m_wheel = 0;

          switch (msg.type) {
            case HID_MSG_KEYBOARD:
              memcpy(kb_buf, msg.payload.keyboard.keycodes, sizeof(kb_buf));
              kb_mod = msg.payload.keyboard.modifier;
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
              kb_mod = current_modifiers;
              m_buttons = msg.payload.mouse.buttons;
              m_x = msg.payload.mouse.x;
              m_y = msg.payload.mouse.y;
              m_wheel = msg.payload.mouse.wheel;
              break;

            case HID_MSG_KB_MOUSE:
              memcpy(kb_buf, msg.payload.kb_mouse.keycodes, sizeof(kb_buf));
              kb_mod = msg.payload.kb_mouse.modifier;
              m_buttons = msg.payload.kb_mouse.buttons;
              m_x = msg.payload.kb_mouse.x;
              m_y = msg.payload.kb_mouse.y;
              m_wheel = msg.payload.kb_mouse.wheel;
              break;

            default:
              break;
          }

          hid_send_kb_mouse(kb_mod, kb_buf, m_buttons, m_x, m_y, m_wheel);
        }
        // measure latency and log occasionally
        {
          TickType_t now = xTaskGetTickCount();
          uint32_t latency_ticks = (now >= msg.enqueue_tick) ? (now - msg.enqueue_tick) : 0;
          uint32_t latency_ms = latency_ticks * portTICK_PERIOD_MS;
          uint32_t qdepth = (hid_queue ? uxQueueMessagesWaiting(hid_queue) : 0);
          (void)qdepth; // suppress unused warning if logging disabled

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
static void run_internal_funct(void);
static void is_toggle_layer(uint16_t keycodeTMP);
static void is_momentary_layer(int16_t keycodeTMP, uint8_t i);
static bool is_internal_function(int16_t keycodeTMP);
static void is_macro(uint16_t keycodeTMP);

/* Enable verbose debug by default for troubleshooting */
#define KEYBOARD_MANAGER_DEBUG 1

uint8_t usb_bl_state = 0; // 0: USB, 1: BL
uint16_t keypress_internal_function = 0;
uint8_t current_row_layer_changer = INVALID_KEY_POS;
uint8_t current_col_layer_changer = INVALID_KEY_POS;

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
      // DEBUG: print what we found
      ESP_LOGI(KM_TAG, "process_matrix_changes: executing internal func: 0x%04X", keypress_internal_function);

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
    if (current_press_col[i] != INVALID_KEY_POS) {
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
      current_col_layer_changer = INVALID_KEY_POS;
      current_row_layer_changer = INVALID_KEY_POS;
    }
  }
}



#ifndef KEY_ENQUEUE_MIN_MS
#define KEY_ENQUEUE_MIN_MS 8
#endif

void send_hid_key() {
  static uint8_t last_enqueued_keycodes[6] = {0};
  static uint8_t last_enqueued_modifier = 0;
  static TickType_t last_key_enqueue_tick = 0;

  // Extract modifiers from global keycodes
  uint8_t modifier = 0;
  extract_modifiers(keycodes, &modifier);
  current_modifiers = modifier;

  /* Coalesce identical key reports to avoid saturating HID queue during bursts */
  if (memcmp(keycodes, last_enqueued_keycodes, sizeof(keycodes)) == 0 && modifier == last_enqueued_modifier) {
    TickType_t ticks_now = xTaskGetTickCount();
    uint32_t elapsed = (uint32_t)(ticks_now - last_key_enqueue_tick);
    uint32_t min_ticks = (uint32_t)pdMS_TO_TICKS(KEY_ENQUEUE_MIN_MS);
    if (min_ticks > 0 && elapsed < min_ticks) {
      /* Too soon and identical — drop to avoid queue flood */
      return;
    }
  }

  hid_msg_t msg;
  msg.type = HID_MSG_KEYBOARD;
  msg.enqueue_tick = xTaskGetTickCount();
  memcpy(msg.payload.keyboard.keycodes, keycodes, sizeof(msg.payload.keyboard.keycodes));
  msg.payload.keyboard.modifier = modifier;

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
          combined.payload.kb_mouse.modifier = modifier;
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
          last_enqueued_modifier = modifier;
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
    last_enqueued_modifier = modifier;
    last_key_enqueue_tick = xTaskGetTickCount();
  } else {
    /* fallback: direct send if queue not initialized yet */
    hid_send_keyboard(modifier, keycodes);
    memcpy(last_enqueued_keycodes, keycodes, sizeof(keycodes));
    last_enqueued_modifier = modifier;
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
    hid_send_mouse(buttons, x, y, wheel);
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
    hid_send_kb_mouse(modifier, msg.payload.kb_mouse.keycodes,
                      msg.payload.kb_mouse.buttons, msg.payload.kb_mouse.x,
                      msg.payload.kb_mouse.y, msg.payload.kb_mouse.wheel);
  }
}

uint8_t keyboard_get_usb_bl_state(void) {
  return usb_bl_state;
}

/* ── Internal function / layer switching handlers ────────────────── */

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
    ESP_LOGI(KM_TAG, "run_internal_funct: BT_TOGGLE case reached. Posting to worker queue...");
    km_post_bt_toggle();
    break;
  }
}

static bool is_internal_function(int16_t keycodeTMP) {
  if (keycodeTMP >= TO_L0)  { 
#if KEYBOARD_SCAN_DEBUG
    KM_LOGI("%d.", keycodeTMP);
#endif
    // Catch BT_TOGGLE explicitly to debug
    if (keycodeTMP == BT_TOGGLE) {
        ESP_LOGI(KM_TAG, "is_internal_function: BT_TOGGLE detected (0x%04X)", keycodeTMP);
    }

    if (keypress_internal_function == 0) {
      keypress_internal_function = keycodeTMP;
      return true;
    }
  }
  return false;
}

static void is_momentary_layer(int16_t keycodeTMP, uint8_t i) {
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

static void is_toggle_layer(uint16_t keycodeTMP) {
  if ((keycodeTMP >= TO_L0) && (keycodeTMP <= TO_L9)) {

    int16_t new_layer = (keycodeTMP - TO_L0) / 256;

    if (current_layout == new_layer) {
      current_layout = 0;
      last_layer = current_layout;
      layer_changed();
      KM_LOGI("layer: 0 %d %d %d", new_layer, keycodeTMP, TO_L0);
    } else {
      current_layout = new_layer;
      last_layer = current_layout;
      layer_changed();
      KM_LOGI("layer: pp %d %d %d", new_layer, keycodeTMP, TO_L0);
    }
  }
}

void is_macro(uint16_t keycodeTMP) {
  if ((keycodeTMP >= MACRO_1) && (keycodeTMP <= MACRO_20)) {
    int16_t macro_i = (keycodeTMP - MACRO_1) / 256;
    KM_LOGI("macro: %d ", macro_i);
    if (macro_i < MAX_MACROS && macros_list[macro_i].name[0] != '\0') {
      uint8_t j = 0;
      for (uint8_t i = 0; i < 6; i++) {
        if (keycodes[i] == 0) {
          keycodes[i] = macros_list[macro_i].keys[j];
          j++;
        }
      }
    }
    /* send_hid_key() removed: caller sends HID report after full loop */
  }
}

/* Process a matrix change: build report, handle internals, send HID */
static void handle_matrix_change(void)
{
    build_keycode_report();
    stat_matrix_changed = 0;
    process_matrix_changes();
    send_hid_key();
}

/* Log periodic scan timing stats and reset counters */
static void maybe_log_scan_stats(void)
{
    uint32_t now_ms = esp_timer_get_time() / 1000;
    if (last_scan_report_ms == 0) last_scan_report_ms = now_ms;
    if (now_ms - last_scan_report_ms <= SCAN_REPORT_INTERVAL_MS) return;

    last_scan_report_ms = now_ms;
    if (full_scan_count > 0) {
      KM_LOGI("Scan avg FULL: %llu us over %u samples",
              (unsigned long long)(total_full_scan_us / full_scan_count), full_scan_count);
    }
    if (partial_scan_count > 0) {
      KM_LOGI("Scan avg PARTIAL: %llu us over %u samples",
              (unsigned long long)(total_partial_scan_us / partial_scan_count), partial_scan_count);
    }
    total_full_scan_us = total_partial_scan_us = 0;
    full_scan_count = partial_scan_count = 0;
}

/* Record scan duration if there was keyboard/mouse activity */
static void record_scan_timing(bool is_full_scan, uint64_t scan_dur, bool had_change)
{
    bool active = had_change;
    if (!active) {
      for (int k = 0; k < 6; k++)
        if (keycodes[k] != 0) { active = true; break; }
    }
    if (!active && last_mouse_event_tick != 0 &&
        (xTaskGetTickCount() - last_mouse_event_tick) <= MOUSE_EVENT_WINDOW) {
      active = true;
    }
    if (!active) return;

    if (is_full_scan) { total_full_scan_us += scan_dur; full_scan_count++; }
    else              { total_partial_scan_us += scan_dur; partial_scan_count++; }
}

void vTaskKeyboard(void *pvParameters) {
  (void)pvParameters;
  for (;;) {
    if (keyboard_task_handle == NULL)
      keyboard_task_handle = xTaskGetCurrentTaskHandle();

    /* Wait for ISR notification or timeout */
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    bool is_full_scan = (notified != 0);
    uint64_t scan_start = esp_timer_get_time();
    bool had_change = false;

    if (stat_matrix_changed == 1) {
      had_change = true;
      handle_matrix_change();
    }

    if (is_full_scan) taskYIELD();

    /* Record timing */
    uint64_t scan_dur = esp_timer_get_time() - scan_start;
    if (scan_dur > 2000) KM_LOGW("scan took %llu us", (unsigned long long)scan_dur);
    record_scan_timing(is_full_scan, scan_dur, had_change);
    maybe_log_scan_stats();
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
}

