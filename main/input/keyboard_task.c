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

static const char *TAG = "KB_TASK";

TaskHandle_t keyboard_task_handle = NULL;
uint8_t usb_bl_state = 0;

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
        if (leader_tick()) {
            uint8_t mod = 0;
            uint8_t kc = leader_consume(&mod);
            if (kc != 0) {
                /* Inject leader result as a tap */
                keycodes[0] = kc;
                if (mod) {
                    for (uint8_t b = 0; b < 8; b++) {
                        if (mod & (1 << b)) {
                            for (uint8_t s = 1; s < 6; s++) {
                                if (keycodes[s] == 0) { keycodes[s] = 0xE0 + b; break; }
                            }
                        }
                    }
                }
                send_hid_key();
                vTaskDelay(pdMS_TO_TICKS(10));
                memset(keycodes, 0, sizeof(keycodes));
                send_hid_key();
            }
        }

        /* Hold just activated → rebuild and send */
        if (tap_hold_hold_just_activated()) {
            build_keycode_report();
            send_hid_key();
        }

        /* Tap dance resolved → inject and send tap press+release */
        if (tap_dance_just_resolved()) {
            uint8_t td_kc = tap_dance_consume();
            if (td_kc != 0) {
                keycodes[0] = td_kc;
                send_hid_key();
                vTaskDelay(pdMS_TO_TICKS(10));
                keycodes[0] = 0;
                send_hid_key();
            }
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
