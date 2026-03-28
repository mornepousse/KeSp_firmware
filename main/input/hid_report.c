/* HID report queue and sender task */
#include "hid_report.h"
#include "hid_transport.h"
#include "matrix_scan.h"
#include "key_definitions.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "HID_REPORT";

/* ── Message types ───────────────────────────────────────────────── */

typedef enum { HID_MSG_KEYBOARD = 1, HID_MSG_MOUSE = 2, HID_MSG_KB_MOUSE = 3 } hid_msg_type_t;

typedef struct {
    hid_msg_type_t type;
    TickType_t enqueue_tick;
    union {
        struct { uint8_t keycodes[6]; uint8_t modifier; } keyboard;
        struct { uint8_t buttons; int8_t x, y, wheel; } mouse;
        struct { uint8_t keycodes[6]; uint8_t modifier; uint8_t buttons; int8_t x, y, wheel; } kb_mouse;
    } payload;
} hid_msg_t;

/* ── State ───────────────────────────────────────────────────────── */

static QueueHandle_t hid_queue = NULL;
static SemaphoreHandle_t hid_report_mutex = NULL;
static TaskHandle_t hid_sender_task_handle = NULL;
static uint8_t current_modifiers = 0;

extern uint8_t usb_bl_state;

/* ── Modifier extraction ─────────────────────────────────────────── */

static void extract_modifiers(uint8_t *kc, uint8_t *modifier)
{
    *modifier = 0;
    for (int i = 0; i < 6; i++) {
        if (kc[i] >= HID_KEY_CONTROL_LEFT && kc[i] <= HID_KEY_GUI_RIGHT) {
            *modifier |= (1 << (kc[i] - HID_KEY_CONTROL_LEFT));
            kc[i] = 0;
        }
    }
}

/* ── Sender task ─────────────────────────────────────────────────── */

static void hid_sender_task(void *pvParameters)
{
    (void)pvParameters;
    hid_msg_t msg;
    for (;;) {
        if (xQueueReceive(hid_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        if (xSemaphoreTake(hid_report_mutex, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        uint8_t kb_buf[6] = {0};
        uint8_t kb_mod = 0;
        uint8_t m_buttons = 0;
        int8_t m_x = 0, m_y = 0, m_wheel = 0;

        switch (msg.type) {
        case HID_MSG_KEYBOARD:
            memcpy(kb_buf, msg.payload.keyboard.keycodes, sizeof(kb_buf));
            kb_mod = msg.payload.keyboard.modifier;
            /* Try to combine with a pending mouse msg */
            {
                hid_msg_t next;
                if (xQueueReceive(hid_queue, &next, 0) == pdTRUE) {
                    if (next.type == HID_MSG_MOUSE) {
                        m_buttons = next.payload.mouse.buttons;
                        m_x = next.payload.mouse.x;
                        m_y = next.payload.mouse.y;
                        m_wheel = next.payload.mouse.wheel;
                    } else {
                        xQueueSendToFront(hid_queue, &next, 0);
                    }
                }
            }
            break;
        case HID_MSG_MOUSE:
            memcpy(kb_buf, keycodes, sizeof(kb_buf));
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
        xSemaphoreGive(hid_report_mutex);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

#ifndef KEY_ENQUEUE_MIN_MS
#define KEY_ENQUEUE_MIN_MS 8
#endif

void send_hid_key(void)
{
    static uint8_t last_kc[6] = {0};
    static uint8_t last_mod = 0;
    static TickType_t last_tick = 0;

    uint8_t modifier = 0;
    extract_modifiers(keycodes, &modifier);
    current_modifiers = modifier;

    /* Coalesce identical reports */
    if (memcmp(keycodes, last_kc, sizeof(keycodes)) == 0 && modifier == last_mod) {
        uint32_t elapsed = (uint32_t)(xTaskGetTickCount() - last_tick);
        if (elapsed < (uint32_t)pdMS_TO_TICKS(KEY_ENQUEUE_MIN_MS))
            return;
    }

    hid_msg_t msg = { .type = HID_MSG_KEYBOARD, .enqueue_tick = xTaskGetTickCount() };
    memcpy(msg.payload.keyboard.keycodes, keycodes, 6);
    msg.payload.keyboard.modifier = modifier;

    if (hid_queue != NULL) {
        /* Try to combine with pending mouse */
        if (uxQueueMessagesWaiting(hid_queue) > 0) {
            hid_msg_t peek;
            if (xQueueReceive(hid_queue, &peek, 0) == pdTRUE) {
                if (peek.type == HID_MSG_MOUSE) {
                    hid_msg_t combined = {
                        .type = HID_MSG_KB_MOUSE, .enqueue_tick = xTaskGetTickCount()
                    };
                    memcpy(combined.payload.kb_mouse.keycodes, keycodes, 6);
                    combined.payload.kb_mouse.modifier = modifier;
                    combined.payload.kb_mouse.buttons = peek.payload.mouse.buttons;
                    combined.payload.kb_mouse.x = peek.payload.mouse.x;
                    combined.payload.kb_mouse.y = peek.payload.mouse.y;
                    combined.payload.kb_mouse.wheel = peek.payload.mouse.wheel;
                    if (xQueueSendToFront(hid_queue, &combined, pdMS_TO_TICKS(5)) == pdTRUE) {
                        memcpy(last_kc, keycodes, 6); last_mod = modifier; last_tick = xTaskGetTickCount();
                        return;
                    }
                    xQueueSendToFront(hid_queue, &peek, pdMS_TO_TICKS(5));
                    return;
                }
                xQueueSendToFront(hid_queue, &peek, 0);
            }
        }
        xQueueSend(hid_queue, &msg, pdMS_TO_TICKS(5));
    } else {
        hid_send_keyboard(modifier, keycodes);
    }
    memcpy(last_kc, keycodes, 6); last_mod = modifier; last_tick = xTaskGetTickCount();
}

void send_mouse_report(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    hid_msg_t msg = {
        .type = HID_MSG_MOUSE, .enqueue_tick = xTaskGetTickCount(),
        .payload.mouse = { buttons, x, y, wheel }
    };
    if (hid_queue != NULL) {
        if (xQueueSendToFront(hid_queue, &msg, pdMS_TO_TICKS(5)) != pdTRUE)
            xQueueSendToFront(hid_queue, &msg, pdMS_TO_TICKS(50));
    } else {
        hid_send_mouse(buttons, x, y, wheel);
    }
}

void send_hid_kb_mouse(uint8_t modifier, const uint8_t kc[6],
                       uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    hid_msg_t msg = { .type = HID_MSG_KB_MOUSE, .enqueue_tick = xTaskGetTickCount() };
    if (kc) memcpy(msg.payload.kb_mouse.keycodes, kc, 6);
    msg.payload.kb_mouse.modifier = modifier;
    msg.payload.kb_mouse.buttons = buttons;
    msg.payload.kb_mouse.x = x;
    msg.payload.kb_mouse.y = y;
    msg.payload.kb_mouse.wheel = wheel;

    if (hid_queue != NULL) {
        if (xQueueSendToFront(hid_queue, &msg, pdMS_TO_TICKS(5)) != pdTRUE)
            xQueueSendToFront(hid_queue, &msg, pdMS_TO_TICKS(50));
    } else {
        hid_send_kb_mouse(modifier, msg.payload.kb_mouse.keycodes,
                          buttons, x, y, wheel);
    }
}

uint8_t keyboard_get_usb_bl_state(void) { return usb_bl_state; }

void hid_report_init(void)
{
    if (!hid_queue)
        hid_queue = xQueueCreate(32, sizeof(hid_msg_t));
    if (!hid_report_mutex)
        hid_report_mutex = xSemaphoreCreateMutex();
    if (!hid_sender_task_handle)
        xTaskCreatePinnedToCore(hid_sender_task, "hid_sender", 4096, NULL, 4, &hid_sender_task_handle, 0);
    ESP_LOGI(TAG, "HID report queue initialized");
}
