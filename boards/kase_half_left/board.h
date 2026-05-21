#ifndef BOARD_H
#define BOARD_H

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#endif

/* ── Product info ──────────────────────────────────────────── */
#define GATTS_TAG           "KaSe_Half"
#define MANUFACTURER_NAME   "KaSe"
#define PRODUCT_NAME        "KaSe Half Left"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0x01   /* left half */

/* ── Half side ─────────────────────────────────────────────── */
#define HALF_SIDE_LEFT  1
#define HALF_SIDE       HALF_SIDE_LEFT

/* ── Matrix 7 cols × 5 rows — COL2ROW, DevKitC ESP32-S3 ────── */
/* Note: the half is a physical matrix but NOT managed by matrix_scan.c.
 * MATRIX_ROWS/COLS/MAX_MATRIX_KEYS are still required because engine code
 * (key_processor.c, keymap.c, etc.) references these defines at compile time. */
#define MATRIX_ROWS         5
#define MATRIX_COLS         7
#define MAX_MATRIX_KEYS     (MATRIX_ROWS * MATRIX_COLS)   /* 35 */

/* COL = output (driven), ROW = input (read). COL2ROW topology. */
#define COLS0   GPIO_NUM_8
#define COLS1   GPIO_NUM_7
#define COLS2   GPIO_NUM_39   /* JTAG_TDI — safe: DevKitC uses USB-JTAG, gpio_reset_pin clears */
#define COLS3   GPIO_NUM_5
#define COLS4   GPIO_NUM_2
#define COLS5   GPIO_NUM_42   /* JTAG_TMS — safe: same reason */
#define COLS6   GPIO_NUM_4

#define ROWS0   GPIO_NUM_6
#define ROWS1   GPIO_NUM_37
#define ROWS2   GPIO_NUM_3    /* JTAG_TDI — safe: same reason as COL2 */
#define ROWS3   GPIO_NUM_9
#define ROWS4   GPIO_NUM_10

/* ── Matrix scan timing (consumed by half_scan_task which creates kbd_cfg) ─ */
#define BOARD_MATRIX_COL2ROW
#define BOARD_MATRIX_SCAN_INTERVAL_US   1000   /* 1 ms between scans */
#define BOARD_MATRIX_SETTLING_US        0
#define BOARD_MATRIX_RECOVERY_US        0
#define BOARD_DEBOUNCE_TICKS            3      /* ~3 ms debounce */

/* ── NRF24L01+ PTX ─────────────────────────────────────────── */
#define BOARD_NRF_SPI_HOST        SPI2_HOST
#define BOARD_NRF_SPI_MOSI        GPIO_NUM_48
#define BOARD_NRF_SPI_MISO        GPIO_NUM_47
#define BOARD_NRF_SPI_SCK         GPIO_NUM_45   /* strapping pin VDD_SPI — safe: CPOL=0 */
#define BOARD_NRF_SPI_CLOCK_HZ    (10 * 1000 * 1000)   /* 10 MHz NRF24 max */

#define BOARD_NRF_CSN_GPIO        GPIO_NUM_35
#define BOARD_NRF_CE_GPIO         GPIO_NUM_36
#define BOARD_NRF_IRQ_GPIO        GPIO_NUM_21   /* wired; not used in MVP (polled TX) */

/* RF addressing — left half. Must match dongle board_rf.h defaults. */
#define BOARD_NRF_ADDR_SUFFIX     0x01
#define BOARD_NRF_CHANNEL         0x4C   /* 2476 MHz */

/* ── Unused peripheral GPIO — not initialized in MVP ───────── */
/* E-ink: CS=18, DC=12, RES=17, BUSY=1 */
/* Trackpad I2C: SDA=40, SCL=38, RST=13, RDY=14 */
/* Battery ADC: GPIO15 (ADC2_CH4), switchable GND=GPIO16 */
/* BMS status: GPIO46 (input only) */
/* LED backlight: GPIO11 (TPS61040DBV boost) */
/* These GPIOs are left in reset state (input, no pull). */

/* ── No display, no BLE, no LED strip on half ──────────────── */
#define BOARD_DISPLAY_SLEEP_MS    0
#define BOARD_SLEEP_MINS          0
#define BOARD_HAS_LED_STRIP       0

/* ── USB: flash/console only (no HID on half) ──────────────── */
/* GPIO43/44 = UART console TX/RX — available on half (not used by matrix) */
#define BOARD_USB_VID             0x303A
#define BOARD_USB_PID             0x4002   /* half, distinct from dongle 0x4001 */

#endif /* BOARD_H */
