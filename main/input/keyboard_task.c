/* Keyboard task: main coordinator loop.
   Waits for matrix ISR notification, delegates to key_processor and hid_report. */
#include "keyboard_task.h"
#include "key_processor.h"
#include "hid_report.h"
#include "keyboard_actions.h"
#include "matrix_scan.h"
#include "tap_hold.h"
#include "tap_dance.h"
#include "combo.h"
#include "leader.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "KB_TASK";

TaskHandle_t keyboard_task_handle = NULL;
uint8_t usb_bl_state = 0;

/* Send a keycode as a quick press+release tap */
static void send_tap(uint8_t kc, uint8_t mod)
{
    memset(keycodes, 0, 6);
    keycodes[0] = kc;
    /* Inject modifier keycodes */
    uint8_t slot = 1;
    for (uint8_t b = 0; b < 8 && slot < 6; b++) {
        if (mod & (1 << b))
            keycodes[slot++] = 0xE0 + b;
    }
    send_hid_key();
    vTaskDelay(pdMS_TO_TICKS(10));
    memset(keycodes, 0, 6);
    send_hid_key();
}

void vTaskKeyboard(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        if (keyboard_task_handle == NULL)
            keyboard_task_handle = xTaskGetCurrentTaskHandle();

        /* 10ms loop for responsive tap/hold and tap dance timing */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

        /* Tick timers — even without matrix change */
        tap_hold_tick();
        tap_dance_tick();

        /* Hold just activated → rebuild and send */
        if (tap_hold_hold_just_activated()) {
            build_keycode_report();
            send_hid_key();
        }

        /* Tap dance resolved → send tap */
        if (tap_dance_just_resolved()) {
            uint8_t td_kc = tap_dance_consume();
            if (td_kc != 0)
                send_tap(td_kc, 0);
        }

        /* Leader timeout check + result */
        if (leader_tick()) {
            uint8_t mod = 0;
            uint8_t kc = leader_consume(&mod);
            if (kc != 0)
                send_tap(kc, mod);
        }

        /* Matrix changed → full processing cycle */
        if (stat_matrix_changed == 1) {
            build_keycode_report();
            stat_matrix_changed = 0;
            process_matrix_changes();

            if (key_processor_has_tap()) {
                send_hid_key();
                vTaskDelay(pdMS_TO_TICKS(10));
                key_processor_clear_taps();
                send_hid_key();
            } else {
                send_hid_key();
            }
        }
    }
}

void keyboard_manager_init(void)
{
    ESP_LOGI(TAG, "Keyboard manager initialized");
    tap_hold_init();
    tap_dance_init();
    combo_init();
    leader_init();
    hid_report_init();
    keyboard_worker_init();
}
