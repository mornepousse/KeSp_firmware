#ifndef BOARD_H
#define BOARD_H

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#endif

/* ── Product info ──────────────────────────────────────────── */
#define GATTS_TAG           "KaSe_Dongle"
#define MANUFACTURER_NAME   "KaSe"
#define PRODUCT_NAME        "KaSe Dongle"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0xD0   /* dongle, distinct des halves 0x01/0x02 */

/* ── Matrix dimensions (logical, 2 halves merged) ──────────── */
#define MATRIX_ROWS         5
#define MATRIX_COLS         14    /* 7 cols half_L + 7 cols half_R */
#define MAX_MATRIX_KEYS     (MATRIX_ROWS * MATRIX_COLS)   /* 70 */

/* Half mapping convention used by RF rx_task and to_global_pos() :
 *   half_L → MATRIX_STATE[row][0..6]
 *   half_R → MATRIX_STATE[row][7..13]
 */
#define COLS_PER_HALF       7
#define HALF_R_COL_OFFSET   7

/* ── No local matrix on dongle — pinout is unused but defined for compile ─ */
#define ROWS0  GPIO_NUM_NC
#define ROWS1  GPIO_NUM_NC
#define ROWS2  GPIO_NUM_NC
#define ROWS3  GPIO_NUM_NC
#define ROWS4  GPIO_NUM_NC
#define COLS0  GPIO_NUM_NC
#define COLS1  GPIO_NUM_NC
#define COLS2  GPIO_NUM_NC
#define COLS3  GPIO_NUM_NC
#define COLS4  GPIO_NUM_NC
#define COLS5  GPIO_NUM_NC
#define COLS6  GPIO_NUM_NC
#define COLS7  GPIO_NUM_NC
#define COLS8  GPIO_NUM_NC
#define COLS9  GPIO_NUM_NC
#define COLS10 GPIO_NUM_NC
#define COLS11 GPIO_NUM_NC
#define COLS12 GPIO_NUM_NC
#define COLS13 GPIO_NUM_NC

/* ── NRF24L01+ pinout (extracted from dongle.kicad_sch netlist) ─ */
#define BOARD_NRF_SPI_HOST       SPI2_HOST
#define BOARD_NRF_SPI_MOSI       GPIO_NUM_5
#define BOARD_NRF_SPI_MISO       GPIO_NUM_6
#define BOARD_NRF_SPI_SCK        GPIO_NUM_7
#define BOARD_NRF_SPI_CLOCK_HZ   (10 * 1000 * 1000)   /* 10 MHz, NRF24 datasheet max */

/* NRF#1 = half_L (channel ch_left, default 0x4C) */
#define BOARD_NRF1_CSN_GPIO      GPIO_NUM_13
#define BOARD_NRF1_CE_GPIO       GPIO_NUM_14
#define BOARD_NRF1_IRQ_GPIO      GPIO_NUM_8

/* NRF#2 = half_R (channel ch_right, default 0x52) */
#define BOARD_NRF2_CSN_GPIO      GPIO_NUM_1
#define BOARD_NRF2_CE_GPIO       GPIO_NUM_4
#define BOARD_NRF2_IRQ_GPIO      GPIO_NUM_2

/* ── No display backend on dongle ──────────────────────────── */
/* DELIBERATELY NO #define BOARD_DISPLAY_BACKEND_* here so root CMakeLists
 * detects the absence and skips display sources. */

/* ── Display sleep / deep sleep — no display, no batt → both 0 ─ */
#define BOARD_DISPLAY_SLEEP_MS    0
#define BOARD_SLEEP_MINS          0

/* ── No LED strip on dongle ────────────────────────────────── */
#define BOARD_HAS_LED_STRIP       0

/* ── Matrix scanning placeholders (consumed by code that may compile but
 *    won't actually scan — gated by CONFIG_KASE_HAS_LOCAL_MATRIX=n) ──── */
#define BOARD_MATRIX_COL2ROW
#define BOARD_MATRIX_SCAN_INTERVAL_US  1000
#define BOARD_MATRIX_SETTLING_US       0
#define BOARD_MATRIX_RECOVERY_US       0
#define BOARD_DEBOUNCE_TICKS           3

/* ── USB identification ────────────────────────────────────── */
/* Dev VID/PID until first public release.
 * Migrate to pid.codes (VID 0x1209) before v4.0 release. */
#define BOARD_USB_VID             0x303A
#define BOARD_USB_PID             0x4001

#endif /* BOARD_H */
