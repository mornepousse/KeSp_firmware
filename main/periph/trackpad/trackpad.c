/*
 * trackpad.c — IQS5xx trackpad skeleton for KaSe half firmware.
 *
 * Skeleton status:
 *   REAL:  I2C init, RST pulse, RDY ISR + semaphore, I2C probe.
 *   STUB:  IQS5xx register read (protocol proprietary — see TODO block).
 *   REAL:  rf_encode_trackpad + half_spi_lock + rf_driver_send (send path).
 *
 * The PCB is reversible: both halves compile this module. Runtime probe
 * (I2C ACK check) determines whether the trackpad is mounted.
 *
 * s_radio is exposed as a non-static extern from half_scan_task.c.
 */

#include "trackpad.h"
#include "board.h"           /* BOARD_TRACK_* GPIO + I2C defines */
#include "half_spi.h"        /* half_spi_lock / half_spi_unlock */
#include "rf_packet.h"       /* rf_trackpad_t, rf_encode_trackpad, PKT_TYPE_TRACKPAD */
#include "rf_driver.h"       /* rf_driver_send, rf_radio_t */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "trackpad";

/* ── I2C bus + device handles ──────────────────────────────── */
static i2c_master_bus_handle_t s_i2c_bus    = NULL;
static i2c_master_dev_handle_t s_i2c_device = NULL;

/* ── RDY interrupt semaphore ───────────────────────────────── */
static SemaphoreHandle_t s_rdy_sem = NULL;

/* ── Packet sequence counter (shared trackpad seq space) ───── */
static volatile uint8_t s_seq = 0;

/* ── Reference to the half's NRF radio (owned by half_scan_task) ── */
/* half_scan_task.c defines s_radio as non-static (extern linkage). */
extern rf_radio_t s_radio;

/* ── RDY GPIO ISR handler — signals the trackpad task ──────── */
static void IRAM_ATTR rdy_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_rdy_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

bool trackpad_init(void)
{
    /* ── I2C master bus init ──────────────────────────────── */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = BOARD_TRACK_I2C_PORT,
        .scl_io_num        = BOARD_TRACK_SCL_GPIO,
        .sda_io_num        = BOARD_TRACK_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   /* external 4.7kΩ pull-ups on PCB */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %d", err);
        return false;
    }

    /* ── RST pulse: 10 ms low, then high, settle 50 ms ─────── */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << BOARD_TRACK_RST_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(BOARD_TRACK_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TRACK_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));   /* IQS5xx startup time after reset */

    /* ── RDY interrupt semaphore + ISR ─────────────────────── */
    s_rdy_sem = xSemaphoreCreateBinary();
    if (s_rdy_sem == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed");
        return false;
    }

    gpio_config_t rdy_cfg = {
        .pin_bit_mask = (1ULL << BOARD_TRACK_RDY_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   /* external pull-up on PCB */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,     /* RDY goes low when data ready */
    };
    gpio_config(&rdy_cfg);

    esp_err_t isr_err = gpio_install_isr_service(0);
    /* ESP_ERR_INVALID_STATE means ISR service is already installed — that is OK */
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service returned %d (non-fatal)", isr_err);
    }
    gpio_isr_handler_add(BOARD_TRACK_RDY_GPIO, rdy_isr_handler, NULL);

    /* ── I2C probe: check device ACKs at BOARD_TRACK_I2C_ADDR ── */
    err = i2c_master_probe(s_i2c_bus, BOARD_TRACK_I2C_ADDR, 50 /* timeout ms */);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "I2C probe: no ACK at 0x%02X — trackpad not present on this half",
                 BOARD_TRACK_I2C_ADDR);
        /* Clean up ISR and bus */
        gpio_isr_handler_remove(BOARD_TRACK_RDY_GPIO);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return false;
    }

    /* ── Add I2C device handle for subsequent transactions ───── */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_TRACK_I2C_ADDR,
        .scl_speed_hz    = BOARD_TRACK_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %d", err);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return false;
    }

    ESP_LOGI(TAG, "trackpad present at I2C 0x%02X — init OK", BOARD_TRACK_I2C_ADDR);
    return true;
}

/* ── Trackpad FreeRTOS task ─────────────────────────────────── */
static void trackpad_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "trackpad_task started");

    for (;;) {
        /* Wait for RDY pin to go low (data-ready from IQS5xx) */
        xSemaphoreTake(s_rdy_sem, portMAX_DELAY);

        /* ----------------------------------------------------------------
         * TODO STUB: Read IQS5xx touch report over I2C.
         *
         * The IQS5xx uses a Window Transfer protocol:
         *   1. Wait for RDY low (done above).
         *   2. I2C read starting at register 0x0000 (InfoFlags, 2 bytes):
         *        - InfoFlags[1:0] = finger count, InfoFlags[7] = ShowReset
         *   3. Continue reading XY data (register 0x0004..0x0014 for 5 fingers):
         *        - Each finger: AbsX (16-bit), AbsY (16-bit), TouchStrength, Area
         *   4. For relative movement: use the relative XY fields (register 0x0012..0x0013).
         *   5. Read button state from InfoFlags.
         *   6. End read with a STOP or an end-of-communication byte if required by IQS5xx.
         *
         * Suggested implementation sequence:
         *   uint8_t reg_addr[2] = {0x00, 0x00};
         *   uint8_t data[10];
         *   i2c_master_transmit_receive(s_i2c_device, reg_addr, 2, data, 10, 50);
         *   int8_t dx = (int8_t)data[...];  // extract from relative XY
         *   int8_t dy = (int8_t)data[...];
         *   uint8_t buttons = data[...] & 0x03;  // button bits
         *
         * Reference: Azoteq IQS550 / IQS572 Application Note AN171.
         * ---------------------------------------------------------------- */

        /* Stub: send zero movement (trackpad present, no actual data yet) */
        rf_trackpad_t tp = {
            .dx       = 0,    /* TODO STUB: replace with actual dx from IQS5xx */
            .dy       = 0,    /* TODO STUB: replace with actual dy */
            .buttons  = 0,    /* TODO STUB: replace with button state */
            .scroll_v = 0,    /* TODO STUB: replace with vertical scroll delta */
            .scroll_h = 0,    /* TODO STUB: replace with horizontal scroll delta */
            .seq      = s_seq++,
        };

        /* Encode and transmit over NRF24 (PKT_TYPE_TRACKPAD = 0x3) */
        uint8_t buf[7];
        rf_encode_trackpad(buf, &tp);
        half_spi_lock();
        rf_driver_send(&s_radio, buf, 7);
        half_spi_unlock();
    }
}

void trackpad_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        trackpad_task, "trackpad",
        3072,   /* stack: 3 KB (I2C + encoding, no large buffers) */
        NULL, 6, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed: %d", (int)ret);
    }
}
