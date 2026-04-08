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
#include "key_features.h"
#include "keymap.h"
#include "hid_transport.h"
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
    uint8_t buf[6] = { kc };
    hid_send_keyboard(mod, buf);
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(buf, 0, 6);
    hid_send_keyboard(0, buf);
    vTaskDelay(pdMS_TO_TICKS(20));
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
        auto_shift_tick();

        /* Hold just activated → rebuild and send */
        if (tap_hold_hold_just_activated()) {
            build_keycode_report();
            send_hid_key();
        }

        /* Auto shift resolved → send tap with optional shift */
        if (auto_shift_just_resolved()) {
            uint8_t mod = 0;
            uint8_t kc = auto_shift_consume(&mod);
            if (kc != 0)
                send_tap(kc, mod);
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

        /* Macro sequence pending → play */
        if (key_processor_has_pending_macro()) {
            int16_t idx = key_processor_consume_macro();
            if (idx >= 0 && idx < MAX_MACROS) {
                const macro_step_t *steps = macros_list[idx].steps;

                /* Sequential playback (only macros with delays reach here) */
                for (int s = 0; s < MACRO_MAX_STEPS && steps[s].keycode != 0; s++) {
                    if (steps[s].keycode == MACRO_DELAY_MARKER)
                        vTaskDelay(pdMS_TO_TICKS(steps[s].modifier * 10));
                    else
                        send_tap(steps[s].keycode, steps[s].modifier);
                }
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
    key_override_init();
    hid_report_init();
    keyboard_worker_init();
}
