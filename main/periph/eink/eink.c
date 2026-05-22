/*
 * eink.c — SSD1681 e-ink driver for KaSe half firmware.
 * See eink.h for API contract.
 *
 * CRITICAL SPI lock contract (do not change):
 *   eink_push():
 *     half_spi_lock()
 *       [SPI transactions — short, ~10 ms]
 *     half_spi_unlock()
 *     [BUSY wait — long, ~1-2 s — bus FREE, NRF can TX key events]
 *
 * This ensures keyboard latency is not affected by e-ink refresh cycles.
 *
 * TEST_HOST guard: all ESP-IDF / FreeRTOS / GPIO / SPI code is inside
 * #ifndef TEST_HOST blocks. eink_fb_set_px() is outside any guard — it is
 * pure byte manipulation with no ESP-IDF dependencies, and must compile
 * in both firmware and host test builds.
 */

#include "eink.h"
#include <string.h>   /* memset — used by both host and firmware builds */

/* ── 1bpp pixel packing: set one pixel in the SSD1681 B&W RAM image ── */
/*
 * Packs a single pixel into the packed 1bpp framebuffer.
 *
 * Layout (row-major, MSB-first):
 *   fb[row * (EINK_WIDTH/8) + col/8]  bit (7 - col%8) = is_white
 *
 * SSD1681 convention: bit=1 → white, bit=0 → black.
 * LVGL v8 at LV_COLOR_DEPTH=1: color.full==1 = white, color.full==0 = black.
 * Polarity is direct — no inversion needed.
 *
 * No FreeRTOS / GPIO / SPI deps. Called from eink_lvgl_set_px_cb (firmware)
 * and directly from host tests. Must remain outside TEST_HOST guards.
 */
void eink_fb_set_px(uint8_t *fb, int col, int row, int is_white)
{
    int byte_idx = row * (EINK_WIDTH / 8) + col / 8;
    int bit_idx  = 7 - (col % 8);   /* MSB = leftmost pixel in each byte */

    if (is_white) {
        fb[byte_idx] |=  (uint8_t)(1 << bit_idx);   /* white: set bit */
    } else {
        fb[byte_idx] &= ~(uint8_t)(1 << bit_idx);   /* black: clear bit */
    }
}

#ifndef TEST_HOST
/* All ESP-IDF / FreeRTOS / GPIO / SPI code is guarded here.
 * The host test runner compiles eink.c with -DTEST_HOST and only
 * sees eink_fb_set_px above. */

#include "board.h"         /* BOARD_EINK_* GPIO + SPI defines */
#include "half_spi.h"      /* half_spi_lock / half_spi_unlock */
#include "eink_lvgl.h"     /* eink_lvgl_init, eink_lvgl_start */

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eink";

/* ── SPI device handle for SSD1681 ─────────────────────────── */
static spi_device_handle_t s_eink_dev = NULL;

/* ── Framebuffer: 200×200 px, 1bpp (5000 bytes), static BSS ── */
/* White = 0xFF (SSD1681 convention: bit=1 → white, bit=0 → black) */
static uint8_t s_fb[EINK_FB_SIZE];

/* ── Probe result ───────────────────────────────────────────── */
static bool s_present = false;

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
/* Caller must hold half_spi_lock(). */
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

    /* ── Step 1: Software Reset (cmd 0x12) ───────────────────────
     * Restores all registers to OTP defaults before each full update.
     * BUSY goes HIGH briefly (~10 ms). Poll inside the lock (short). */
    eink_send_cmd(0x12);
    {
        int sw_wait = 0;
        while (gpio_get_level(BOARD_EINK_BUSY_GPIO) == 1 && sw_wait < 50) {
            vTaskDelay(pdMS_TO_TICKS(1));
            sw_wait++;
        }
    }

    /* ── Step 2: Driver Output Control (cmd 0x01) ────────────────
     * Gate lines: 200 rows → 200-1 = 0xC7. Scanning dir: 0 (default). */
    eink_send_cmd(0x01);
    {
        static const uint8_t d01[] = {0xC7, 0x00, 0x00};
        eink_send_data(d01, sizeof(d01));
    }

    /* ── Step 3: Border Waveform Control (cmd 0x3C) ─────────────
     * 0x05 = fix border at VSS (black). Prevents border flicker. */
    eink_send_cmd(0x3C);
    {
        static const uint8_t d3c[] = {0x05};
        eink_send_data(d3c, sizeof(d3c));
    }

    /* ── Step 4: Set RAM-X Address Start/End (cmd 0x44) ─────────
     * Col 0 to col 24 → 25 bytes × 8 bits = 200 pixels. */
    eink_send_cmd(0x44);
    {
        static const uint8_t d44[] = {0x00, 0x18};
        eink_send_data(d44, sizeof(d44));
    }

    /* ── Step 5: Set RAM-Y Address Start/End (cmd 0x45) ─────────
     * Y counts downward: start=199 (0x00C7), end=0 (0x0000). */
    eink_send_cmd(0x45);
    {
        static const uint8_t d45[] = {0xC7, 0x00, 0x00, 0x00};
        eink_send_data(d45, sizeof(d45));
    }

    /* ── Step 6: Set RAM-X Address Counter (cmd 0x4E) ───────────
     * Reset X pointer to column 0 before RAM write. */
    eink_send_cmd(0x4E);
    {
        static const uint8_t d4e[] = {0x00};
        eink_send_data(d4e, sizeof(d4e));
    }

    /* ── Step 7: Set RAM-Y Address Counter (cmd 0x4F) ───────────
     * Reset Y pointer to row 199 (top of display, RAM written top-down). */
    eink_send_cmd(0x4F);
    {
        static const uint8_t d4f[] = {0xC7, 0x00};
        eink_send_data(d4f, sizeof(d4f));
    }

    /* ── Step 8: Write B&W RAM (cmd 0x24) ───────────────────────
     * Send 5000-byte framebuffer. bit=1→white, bit=0→black (SSD1681 native).
     * MSB-first: byte[0] bit7 = pixel at (col=0, row=0). */
    eink_send_cmd(0x24);
    eink_send_data(fb, EINK_FB_SIZE);
    ESP_LOGI(TAG, "eink_push: SPI write complete (~10 ms)");

    /* ── Step 9: Display Update Control 2 (cmd 0x22) ────────────
     * 0xF7 = full update using default OTP LUT (no custom waveform table). */
    eink_send_cmd(0x22);
    {
        static const uint8_t d22[] = {0xF7};
        eink_send_data(d22, sizeof(d22));
    }

    /* ── Step 10: Master Activation (cmd 0x20) ───────────────────
     * No data. Triggers the ~2 s full refresh. BUSY goes HIGH immediately.
     * DO NOT poll BUSY here — release the SPI lock first. */
    eink_send_cmd(0x20);

    /* CRITICAL: Release SPI bus BEFORE BUSY wait.
     * Panel is now refreshing internally. NRF24 can transmit freely.
     * BUSY wait (~1-2 s) is performed outside the lock. */
    half_spi_unlock();

    /* Wait for BUSY to go low (panel refresh complete).
     * Lock is NOT held during this wait — NRF24 TX proceeds freely. */
    int wait_ms = 0;
    while (gpio_get_level(BOARD_EINK_BUSY_GPIO) == 1 && wait_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (wait_ms >= 3000) {
        ESP_LOGW(TAG, "eink_push: BUSY timeout after 3 s (panel hung?)");
    } else {
        ESP_LOGI(TAG, "eink_push: BUSY cleared after %d ms", wait_ms);
    }
}

void eink_clear(void)
{
    memset(s_fb, 0xFF, EINK_FB_SIZE);   /* all white */
    eink_push(s_fb);
    /* eink_push already polls BUSY to completion — panel is white when this returns */
}

void eink_start(void)
{
    /* eink_task (1 Hz polling loop) retired — replaced by LVGL handler task.
     * eink_lvgl_init() sets up LVGL, draw buffer, flush callback, tick timer,
     * and creates the static screen. eink_lvgl_start() creates the handler task. */
    eink_lvgl_init();
    eink_lvgl_start();
}

#endif /* TEST_HOST */
