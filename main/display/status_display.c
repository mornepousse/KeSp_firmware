/* Status display coordinator — delegates to the active display backend.
   No backend-specific code here. */
#include "status_display.h"
#include "display_backend.h"
#include "keyboard_config.h"
#include "keymap.h"
#include "matrix_scan.h"
#include "esp_log.h"
#include "esp_timer.h"
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

    /* Build display config from board.h defines */
    display_hw_config_t cfg = {
        .bus_type = BOARD_DISPLAY_BUS,
        .width = BOARD_DISPLAY_WIDTH,
        .height = BOARD_DISPLAY_HEIGHT,
        .pixel_clock_hz = BOARD_DISPLAY_CLK_HZ,
        .reset_pin = BOARD_DISPLAY_RESET,
#ifdef BOARD_DISPLAY_BACKEND_ROUND
        .spi = {
            .host = BOARD_DISPLAY_SPI_HOST,
            .sclk = BOARD_DISPLAY_SPI_SCLK,
            .mosi = BOARD_DISPLAY_SPI_MOSI,
            .cs = BOARD_DISPLAY_SPI_CS,
            .dc = BOARD_DISPLAY_SPI_DC,
            .backlight = BOARD_DISPLAY_SPI_BL,
        },
#else
        .i2c = {
            .host = BOARD_DISPLAY_I2C_HOST,
            .sda = BOARD_DISPLAY_I2C_SDA,
            .scl = BOARD_DISPLAY_I2C_SCL,
            .address = BOARD_DISPLAY_I2C_ADDR,
            .enable_internal_pullups = BOARD_DISPLAY_I2C_PULLUPS,
        },
#endif
    };

    if (!backend->init(&cfg)) {
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
