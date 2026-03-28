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

/* USB/BLE transport state: 0 = USB, 1 = BLE */
uint8_t usb_bl_state = 0;

void vTaskKeyboard(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        if (keyboard_task_handle == NULL)
            keyboard_task_handle = xTaskGetCurrentTaskHandle();

        /* Wait for ISR notification or 50ms timeout */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

        if (stat_matrix_changed == 1) {
            build_keycode_report();
            stat_matrix_changed = 0;
            process_matrix_changes();
            send_hid_key();
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
