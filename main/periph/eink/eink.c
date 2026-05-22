/*
 * eink.c — SSD1681 e-ink skeleton for KaSe half firmware.
 * See eink.h for API contract and skeleton/stub boundary.
 *
 * Skeleton:  SPI device, GPIO, RST, probe, SPI lock ordering.
 * Stub:      SSD1681 command sequences, 1bpp rendering.
 *
 * CRITICAL SPI lock contract (do not change):
 *   eink_push():
 *     half_spi_lock()
 *       [SPI transactions — short, ~10 ms]
 *     half_spi_unlock()
 *     [BUSY wait — long, ~1-2 s — bus FREE, NRF can TX key events]
 *
 * This ensures keyboard latency is not affected by e-ink refresh cycles.
 */

#include "eink.h"
#include "board.h"         /* BOARD_EINK_* GPIO + SPI defines */
#include "half_spi.h"      /* half_spi_lock / half_spi_unlock */

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "eink";

/* ── SPI device handle for SSD1681 ─────────────────────────── */
static spi_device_handle_t s_eink_dev = NULL;

/* ── Framebuffer: 200×200 px, 1bpp (5000 bytes), static BSS ── */
/* White = 0xFF (SSD1681 convention: bit=1 → white, bit=0 → black) */
static uint8_t s_fb[EINK_FB_SIZE];

/* ── Probe result ───────────────────────────────────────────── */
static bool s_present = false;

/* ── Stub: current layer index for display (replaced by g_half_state in Plan Bricks-3) ── */
/* TODO: remove this stub and read from g_half_state.layer_idx once Plan Bricks-3 is done */
static uint8_t s_layer_idx_stub = 0;

/* ── Helper: send one SPI command byte (DC=low) ─────────────── */
/* Caller must hold half_spi_lock(). */
static void eink_send_cmd(uint8_t cmd)
{
    gpio_set_level(BOARD_EINK_DC_GPIO, 0);   /* DC=low → command */
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_eink_dev, &t);
}

/* ── Helper: send data bytes (DC=high) ──────────────────────── */
/* Caller must hold half_spi_lock(). Suppressed in stub — called in eink_push TODO block. */
static void eink_send_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(BOARD_EINK_DC_GPIO, 1);   /* DC=high → data */
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_eink_dev, &t);
}

bool eink_init(void)
{
    /* ── GPIO config: DC, RST (output); BUSY (input) ─────────── */
    /* GPIO1 is a strapping pin (SD_DATA0) — reset bootloader function first */
    gpio_reset_pin(BOARD_EINK_BUSY_GPIO);

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << BOARD_EINK_DC_GPIO) | (1ULL << BOARD_EINK_RST_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);

    gpio_config_t busy_cfg = {
        .pin_bit_mask = (1ULL << BOARD_EINK_BUSY_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&busy_cfg);

    /* ── Reset pulse: RST high 10 ms → low 10 ms → high 10 ms ─ */
    gpio_set_level(BOARD_EINK_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_EINK_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_EINK_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* ── Probe: BUSY should go low within 200 ms after reset ─── */
    /* SSD1681 pulls BUSY high internally during power-on / reset init. */
    int probe_ms = 0;
    while (gpio_get_level(BOARD_EINK_BUSY_GPIO) == 1 && probe_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        probe_ms += 10;
    }
    if (gpio_get_level(BOARD_EINK_BUSY_GPIO) != 0) {
        ESP_LOGI(TAG, "BUSY did not go low within 200 ms — panel not present on this half");
        return false;
    }

    /* ── Register SSD1681 as second device on SPI2_HOST ────────── */
    /* SPI2_HOST bus was initialized by rf_driver_init_tx(). */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = BOARD_EINK_SPI_HZ,   /* 4 MHz */
        .mode           = 0,                    /* CPOL=0, CPHA=0 — same as NRF24 */
        .spics_io_num   = BOARD_EINK_CS_GPIO,   /* GPIO18, active-low */
        .queue_size     = 1,
        .pre_cb         = NULL,
        .post_cb        = NULL,
    };
    esp_err_t err = spi_bus_add_device(BOARD_NRF_SPI_HOST, &dev_cfg, &s_eink_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %d — cannot register SSD1681", err);
        return false;
    }

    /* ── Initialize framebuffer to white ───────────────────────── */
    memset(s_fb, 0xFF, EINK_FB_SIZE);

    s_present = true;
    ESP_LOGI(TAG, "SSD1681 e-ink panel detected — init OK");
    return true;
}

void eink_push(const uint8_t *fb)
{
    if (!s_present || s_eink_dev == NULL) return;

    /* STEP 1: Acquire SPI bus (NRF24 must not be transmitting) */
    half_spi_lock();

    /* STEP 2: SSD1681 command sequence — write RAM + trigger full refresh.
     *
     * TODO STUB: Implement the following SSD1681 sequence:
     *   1. SW reset (cmd 0x12), then wait BUSY (inside the lock, short wait ~10 ms).
     *   2. Driver output control (cmd 0x01): set gate/source driver output counts
     *        for 200×200: data = {0xC7, 0x00, 0x00}  (200-1=0xC7 gate lines)
     *   3. Border waveform control (cmd 0x3C): data = {0x05}
     *   4. Set RAM-X address start/end (cmd 0x44): data = {0x00, 0x18}
     *        (0x00=col 0, 0x18=col 24 → 25 bytes × 8 bits = 200 px)
     *   5. Set RAM-Y address start/end (cmd 0x45): data = {0xC7, 0x00, 0x00, 0x00}
     *        (0x00C7 = 199 start, 0x0000 end — Y counts down)
     *   6. Set RAM-X address counter (cmd 0x4E): data = {0x00}
     *   7. Set RAM-Y address counter (cmd 0x4F): data = {0xC7, 0x00}
     *   8. Write B&W RAM (cmd 0x24): send fb[EINK_FB_SIZE] bytes
     *   9. Display update control 2 (cmd 0x22): data = {0xF7} (full update sequence)
     *   10. Master activation (cmd 0x20): no data — triggers the refresh
     *
     * After step 10, BUSY goes HIGH (panel is refreshing). DO NOT poll BUSY inside
     * the lock — release the lock first (step below), then poll.
     *
     * For now, send a no-op SW reset only (demonstrates SPI path works): */
    eink_send_cmd(0x12);   /* SW Reset — BUSY goes high briefly */
    /* eink_send_data is used in the full command sequence (TODO above).
     * Reference it here to suppress -Wunused-function on the stub build. */
    (void)eink_send_data;
    (void)fb;   /* fb is used in the full RAM write sequence (TODO above) */

    /* STEP 3: Release SPI bus — BEFORE BUSY wait.
     * Panel is now refreshing internally. NRF24 can transmit freely. */
    half_spi_unlock();

    /* STEP 4: Wait for BUSY to go low (panel refresh complete).
     * Lock is NOT held during this wait. */
    int wait_ms = 0;
    while (gpio_get_level(BOARD_EINK_BUSY_GPIO) == 1 && wait_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (wait_ms >= 3000) {
        ESP_LOGW(TAG, "eink_push: BUSY timeout after 3 s (panel hung?)");
    }
}

void eink_clear(void)
{
    memset(s_fb, 0xFF, EINK_FB_SIZE);   /* all white */
    eink_push(s_fb);
    /* eink_push already polls BUSY to completion — panel is white when this returns */
}

/* ── Refresh task ───────────────────────────────────────────── */
static void eink_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "eink_task started (1 Hz refresh)");

    /* Initial clear on first run */
    eink_clear();

    for (;;) {
        /* 1 Hz refresh rate — e-ink content changes slowly (layer name, modifiers).
         * Future optimization: wake on ESP-NOW layer-change notification instead. */
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* TODO STUB: Render half_state into s_fb (1bpp, MSB-first).
         *
         * Plan Bricks-3 will wire g_half_state (espnow_info.h). Until then,
         * s_layer_idx_stub is always 0 and rendering draws "Layer 0".
         *
         * Rendering decision (deferred — see spec Section 9.3):
         *   Option A: Direct 1bpp font (bitmap font, no dependencies, minimal RAM).
         *   Option B: LVGL with a custom 1bpp display driver (more flexible, ~150 KB flash).
         *
         * Suggested minimal direct approach:
         *   1. memset(s_fb, 0xFF, EINK_FB_SIZE);  // white background
         *   2. Draw layer index as ASCII digits using a 1bpp bitmap font.
         *   3. Draw modifier state icons (Shift, Ctrl, etc.) as 16×16 bitmaps.
         *   4. Draw BT/USB status indicators.
         *
         * For now: do nothing (panel stays white from eink_clear() above). */
        (void)s_layer_idx_stub;   /* suppress unused warning until rendering is implemented */

        /* Only push if content has changed (future: compare previous state) */
        /* eink_push(s_fb); */   /* commented out — no rendering yet, avoid refresh noise */
    }
}

void eink_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        eink_task, "eink_task",
        4096,   /* 4 KB stack: rendering may need temporary local buffers */
        NULL, 3, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed: %d", (int)ret);
    }
}
