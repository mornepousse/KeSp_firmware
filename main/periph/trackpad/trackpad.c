/*
 * trackpad.c — IQS5xx trackpad driver for KaSe half firmware.
 *
 * Responsibilities: I2C init, RST pulse, RDY ISR + semaphore, I2C probe,
 * IQS5xx register read (9-byte block, GestureEvents0..RelY), raw-frame
 * encode (rf_encode_trackpad) and RF send via half_spi_lock + rf_driver_send.
 *
 * Gesture-to-HID mapping lives in trackpad_map.c (host-testable).
 *
 * The PCB is reversible: both halves compile this module. Runtime probe
 * (I2C ACK check) determines whether the trackpad is mounted.
 *
 * s_radio is exposed as a non-static extern from half_scan_task.c.
 */

/* ── Host-safe includes (no ESP-IDF) ── */
#include "trackpad.h"
#include "rf_packet.h"      /* rf_trackpad_t, rf_encode_trackpad — host-safe */
#include <stdbool.h>
#include <stdint.h>

/* ── IQS5xx-B000 register addresses (16-bit, big-endian on wire) ── */
#define IQS5XX_REG_GESTURE_EVENTS_0   0x000F
#define IQS5XX_REG_GESTURE_EVENTS_1   0x0010
#define IQS5XX_REG_SYSTEM_INFO_0      0x0011
#define IQS5XX_REG_SYSTEM_INFO_1      0x0012
#define IQS5XX_REG_NUMBER_OF_FINGERS  0x0013
#define IQS5XX_REG_RELATIVE_X         0x0014   /* int16, big-endian */
#define IQS5XX_REG_RELATIVE_Y         0x0016   /* int16, big-endian */
#define IQS5XX_REG_SYSTEM_CONTROL_0   0x0431   /* ACK_RESET bit — VERIFY at bench */
#define IQS5XX_REG_END_WINDOW         0xEEEE   /* End Communication Window */

/* ── SystemInfo0 bitmasks ── */
#define IQS5XX_SYSINFO0_SHOW_RESET    0x80     /* bit 7 — set after reset */

/* ── System Control 0 bitmasks ── */
#define IQS5XX_SYSCTRL0_ACK_RESET     0x02     /* bit 1 — VERIFY address at bench */

/* ── Read block ── */
/* IQS5xx-B000 map (confirmed vs QMK azoteq driver): GestureEvents0=0x000D,
 * GE1=0x000E, SystemInfo0=0x000F, SystemInfo1=0x0010, NumberOfFingers=0x0011,
 * RelativeX=0x0012 (2B), RelativeY=0x0014 (2B), AbsoluteX=0x0016.
 * Reading from 0x000D puts GE0 at data[0], RelX at data[5..6], RelY at data[7..8]. */
#define IQS5XX_READ_START_ADDR        0x000D   /* GestureEvents0 */
#define IQS5XX_READ_BYTE_COUNT        9        /* 0x000D..0x0015 → through RelY LSB */

/* ── Driver-only constants (inside TEST_HOST guard below) ── */
#define IQS5XX_EXPLICIT_END_WINDOW    1        /* 1 = write 0xEEEE after read */

/* ── Pure helpers — compiled in both TEST_HOST and firmware builds ── */

void tp_parse_raw(const uint8_t data[9], tp_frame_t *out)
{
    out->ge0       = data[0];
    out->ge1       = data[1];
    out->sysinfo0  = data[2];
    /* data[3] = SystemInfo1 — not used */
    out->n_fingers = data[4];
    out->rel_x     = (int16_t)(((uint16_t)data[5] << 8) | data[6]);
    out->rel_y     = (int16_t)(((uint16_t)data[7] << 8) | data[8]);
}

bool tp_should_skip_idle(uint8_t ge0, uint8_t ge1, uint8_t n_fingers,
                         int16_t rel_x, int16_t rel_y, uint8_t prev_nf)
{
    bool idle = (ge0 == 0 && ge1 == 0 && n_fingers == 0 && rel_x == 0 && rel_y == 0);
    return idle && (prev_nf == 0);
}

bool tp_is_show_reset(uint8_t sysinfo0)
{
    return (sysinfo0 & IQS5XX_SYSINFO0_SHOW_RESET) != 0;
}

#ifndef TEST_HOST

/* ── ESP-IDF-dependent includes ── */
#include "board.h"           /* BOARD_TRACK_* GPIO + I2C defines */
#include "half_spi.h"        /* half_spi_lock / half_spi_unlock */
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

/* ── ShowReset ACK state — cleared after first successful ACK write ── */
static bool s_need_reset_ack = true;

/* ── Trackpad task handle — needed for suspend/resume during light-sleep ── */
static TaskHandle_t s_tp_task = NULL;

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
        .intr_type    = GPIO_INTR_POSEDGE,     /* IQS5xx-B000 RDY is ACTIVE-HIGH: rises
                                                * when a comm window opens. Read while high.
                                                * (NEGEDGE fired at window CLOSE → I2C timeout.) */
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

        /* ── Step 1: Read 9-byte block from GestureEvents0 (0x000F) ── */
        /* IQS5xx window-transfer: write 2-byte reg addr, read 9 bytes.
         * Must complete while RDY is asserted. Timeout 50 ms. */
        uint8_t reg_addr[2] = {
            (IQS5XX_READ_START_ADDR >> 8) & 0xFF,   /* 0x00 */
            (IQS5XX_READ_START_ADDR)      & 0xFF,   /* 0x0F */
        };
        uint8_t data[IQS5XX_READ_BYTE_COUNT];
        esp_err_t err = i2c_master_transmit_receive(
            s_i2c_device,
            reg_addr, sizeof(reg_addr),
            data, sizeof(data),
            50   /* timeout ms */
        );
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2C read failed: %d — skip event", err);
            /* Do not end window — bus is already free after failed transaction */
            continue;
        }

        /* ── Step 2: End Communication Window (best-effort) ─────────── */
        /* Some IQS5xx revisions require an explicit 0xEEEE write to close
         * the window. Others close on the I2C STOP from step 1.
         * IQS5XX_EXPLICIT_END_WINDOW=1 by default — disable at bench if
         * bus hangs (see bench checklist item 2). */
#if IQS5XX_EXPLICIT_END_WINDOW
        {
            uint8_t end_cmd[3] = {0xEE, 0xEE, 0x00};
            /* Best-effort: chip may already have closed the window — ignore error */
            i2c_master_transmit(s_i2c_device, end_cmd, sizeof(end_cmd), 10);
        }
#endif

        /* ── Step 3: Extract fields from read block ─────────────────── */
        tp_frame_t frame;
        tp_parse_raw(data, &frame);
        uint8_t  ge0       = frame.ge0;
        uint8_t  ge1       = frame.ge1;
        uint8_t  sysinfo0  = frame.sysinfo0;
        uint8_t  n_fingers = frame.n_fingers;
        int16_t  rel_x     = frame.rel_x;
        int16_t  rel_y     = frame.rel_y;

        /* ── Step 4: ShowReset ACK (once after hardware reset) ───────── */
        /* On first read after RST pulse, ShowReset (sysinfo0 bit7) is set.
         * Acknowledge by writing ACK_RESET bit to System Control 0 (0x0431).
         * Skip the data from this event — RelX/RelY are garbage post-reset.
         * Register address and bit position: VERIFY against IQS5xx-B000
         * datasheet (spec §7 item 6). Expected: addr=0x0431, bit1=0x02. */
        if (s_need_reset_ack) {
            if (tp_is_show_reset(sysinfo0)) {
                uint8_t ack_cmd[3] = {
                    (IQS5XX_REG_SYSTEM_CONTROL_0 >> 8) & 0xFF,   /* 0x04 */
                    (IQS5XX_REG_SYSTEM_CONTROL_0)      & 0xFF,   /* 0x31 */
                    IQS5XX_SYSCTRL0_ACK_RESET                     /* 0x02 */
                };
                esp_err_t ack_err = i2c_master_transmit(s_i2c_device,
                                                         ack_cmd, sizeof(ack_cmd),
                                                         20);
                if (ack_err == ESP_OK) {
                    ESP_LOGI(TAG, "ShowReset ACK sent — entering normal operation");
                } else {
                    ESP_LOGW(TAG, "ShowReset ACK failed: %d", ack_err);
                }
                s_need_reset_ack = false;
                continue;   /* skip this event's movement data */
            } else {
                /* ShowReset not set on first read — chip was already running */
                s_need_reset_ack = false;
                ESP_LOGI(TAG, "ShowReset not set on first read — normal operation");
            }
        }

        /* ── Step 5: Encode raw + transmit (mapping now done on the dongle) ── */
        /* Forward every report except pure idle repeats. A finger-lift frame
         * (all-zero but s_prev_nf > 0) MUST be sent so the dongle's
         * trackpad_map can release a held drag / pending tap. */
        static uint8_t s_prev_nf = 0;
        if (tp_should_skip_idle(ge0, ge1, n_fingers, rel_x, rel_y, s_prev_nf)) continue;
        s_prev_nf = n_fingers;
        rf_trackpad_t tp = { .ge0=ge0, .ge1=ge1, .n_fingers=n_fingers,
                             .rel_x=rel_x, .rel_y=rel_y, .seq=s_seq++ };
        uint8_t buf[9];
        rf_encode_trackpad(buf, &tp);
        half_spi_lock();
        rf_driver_send(&s_radio, buf, 9);
        half_spi_unlock();
    }
}

void trackpad_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        trackpad_task, "trackpad",
        3072,   /* stack: 3 KB (I2C + encoding, no large buffers) */
        NULL, 6, &s_tp_task, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed: %d", (int)ret);
    }
}

void trackpad_suspend(void)
{
    gpio_isr_handler_remove(BOARD_TRACK_RDY_GPIO);   /* no RDY wake while asleep */
    if (s_tp_task) vTaskSuspend(s_tp_task);
}

void trackpad_resume(void)
{
    if (s_tp_task) vTaskResume(s_tp_task);
    gpio_isr_handler_add(BOARD_TRACK_RDY_GPIO, rdy_isr_handler, NULL);
}

#endif /* TEST_HOST */
