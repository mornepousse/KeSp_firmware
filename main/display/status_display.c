/* Status display coordinator — delegates to the active display backend.
   No backend-specific code here. */
#include "status_display.h"
#include "display_backend.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef SPLASH_DURATION_MS
#define SPLASH_DURATION_MS 3000
#endif

static const char *TAG = "STATUS_DISP";

static bool status_display_sleeping = false;
static bool is_showing_splash = false;
static TickType_t splash_start_tick = 0;

/* Backend pointer — set at init by board-specific code */
static const display_backend_t *backend = NULL;

void display_set_backend(const display_backend_t *b) { backend = b; }
const display_backend_t *display_get_backend(void)    { return backend; }

/* Request wake flag for display task context */
volatile bool request_wake_request = false;

/* Expose a controlled way to disable the display from other modules */
extern bool display_available;
void status_display_force_disable(void)
{
    display_available = false;
}

/* ── Public API ──────────────────────────────────────────────────── */

void status_display_start(void)
{
    if (!backend) {
        ESP_LOGE(TAG, "No display backend set");
        return;
    }

    if (!backend->init()) {
        display_available = false;
        return;
    }
    display_available = true;

    esp_log_level_set(TAG, ESP_LOG_WARN);
    status_display_sleeping = false;
    status_display_refresh_all();
}

void status_display_update_layer_name(void)
{
    if (!display_available || status_display_sleeping || !backend) return;
    is_showing_splash = false;
    backend->update_layer();
}

void status_display_update(void)
{
    if (!display_available || status_display_sleeping || !backend) return;

    if (is_showing_splash) {
        if ((xTaskGetTickCount() - splash_start_tick) > pdMS_TO_TICKS(SPLASH_DURATION_MS)) {
            is_showing_splash = false;
            backend->refresh_all();
        }
        return;
    }
    backend->update();
}

void status_display_refresh_all(void)
{
    if (!display_available || !backend) return;
    is_showing_splash = false;
    status_display_sleeping = false;
    backend->refresh_all();
}

void status_display_sleep(void)
{
    if (!display_available || status_display_sleeping || !backend) return;
    backend->sleep();
    status_display_sleeping = true;
}

void status_display_wake(void)
{
    if (!display_available || !status_display_sleeping || !backend) return;
    backend->wake();
    status_display_sleeping = false;
}

void status_display_show_DFU_prog(void)
{
    if (!display_available || !backend) return;
    backend->show_dfu();
}

void status_display_notify_mouse_activity(void)
{
    if (backend) backend->notify_mouse();
}
