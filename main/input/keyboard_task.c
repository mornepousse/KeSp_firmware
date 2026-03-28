/* Keyboard task: main coordinator loop.
   Waits for matrix ISR notification, delegates to key_processor and hid_report. */
#include "keyboard_task.h"
#include "key_processor.h"
#include "hid_report.h"
#include "keyboard_actions.h"
#include "matrix_scan.h"
#include "tap_hold.h"
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

        /* Short timeout: 10ms for responsive tap/hold timing */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

        /* Always tick tap/hold timer — even without matrix change */
        tap_hold_tick();

        /* If a hold just activated (timeout reached), rebuild and send report */
        if (tap_hold_hold_just_activated()) {
            build_keycode_report();
            send_hid_key();
        }

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
    hid_report_init();
    keyboard_worker_init();
}
