#include "keyboard_worker.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "status_display.h"
#include "hid_bluetooth_manager.h"
#include "esp_log.h"

static const char *KW_TAG = "KM_WORKER";

typedef enum {
    KM_EVENT_NONE = 0,
    KM_EVENT_DISPLAY_UPDATE = 1,
    KM_EVENT_BT_TOGGLE = 2,
} km_event_t;

static QueueHandle_t km_queue = NULL;

static void km_worker_task(void *pvParameters) {
    km_event_t ev;
    for (;;) {
        if (km_queue && xQueueReceive(km_queue, &ev, portMAX_DELAY) == pdTRUE) {
            uint64_t t0 = esp_timer_get_time();
            switch (ev) {
                case KM_EVENT_DISPLAY_UPDATE:
                    status_display_update();
                    break;
                case KM_EVENT_BT_TOGGLE:
                    ESP_LOGI(KW_TAG, "Processing KM_EVENT_BT_TOGGLE");
                    if (hid_bluetooth_is_initialized()) {
                        ESP_LOGI(KW_TAG, "Deinitializing Bluetooth...");
                        deinit_hid_bluetooth();
                        save_bt_state(false);
                    } else {
                        ESP_LOGI(KW_TAG, "Initializing Bluetooth...");
                        init_hid_bluetooth();
                        save_bt_state(true);
                    }
                    status_display_update();
                    break;
                default:
                    break;
            }
            uint64_t t1 = esp_timer_get_time();
            if ((t1 - t0) > 2000000) {
                ESP_LOGW(KW_TAG, "KM worker event %d took %llu ms", ev, (unsigned long long)((t1 - t0)/1000));
            }
        }
    }
}

void keyboard_worker_init(void) {
    if (km_queue) return;
    km_queue = xQueueCreate(10, sizeof(km_event_t));
    if (km_queue) {
        xTaskCreate(km_worker_task, "KM_Worker", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGW(KW_TAG, "Failed to create KM queue");
    }
}

void km_post_display_update(void) {
    if (!km_queue) return;
    km_event_t ev = KM_EVENT_DISPLAY_UPDATE;
    xQueueSend(km_queue, &ev, 0);
}

void km_post_bt_toggle(void) {
    if (!km_queue) {
        ESP_LOGE(KW_TAG, "km_queue is NULL, cannot post BT toggle event");
        return;
    }
    km_event_t ev = KM_EVENT_BT_TOGGLE;
    if (xQueueSend(km_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(KW_TAG, "Failed to post BT toggle event to queue (full?)");
    } else {
        ESP_LOGI(KW_TAG, "BT toggle event posted successfully");
    }
}
