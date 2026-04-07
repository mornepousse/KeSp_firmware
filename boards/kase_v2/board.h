#ifndef BOARD_H
#define BOARD_H

#ifdef ESP_PLATFORM
#include "driver/i2c.h"
#else
#define I2C_NUM_0 0
#endif

/* ── Product info ──────────────────────────────────────────── */
#define GATTS_TAG           "KaSe_V2"
#define MANUFACTURER_NAME   "Mae"
#define PRODUCT_NAME        "KaSe V2"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0x02

/* ── Matrix GPIO pins ──────────────────────────────────────── */
#define ROWS0  GPIO_NUM_10
#define ROWS1  GPIO_NUM_11
#define ROWS2  GPIO_NUM_12
#define ROWS3  GPIO_NUM_13
#define ROWS4  GPIO_NUM_14

#define COLS0  GPIO_NUM_9
#define COLS1  GPIO_NUM_46
#define COLS2  GPIO_NUM_37
#define COLS3  GPIO_NUM_8
#define COLS4  GPIO_NUM_18
#define COLS5  GPIO_NUM_17
#define COLS6  GPIO_NUM_16
#define COLS7  GPIO_NUM_43
#define COLS8  GPIO_NUM_44
#define COLS9  GPIO_NUM_1
#define COLS10 GPIO_NUM_2
#define COLS11 GPIO_NUM_42
#define COLS12 GPIO_NUM_41

/* ── Matrix dimensions ─────────────────────────────────────── */
#define MATRIX_ROWS  5
#define MATRIX_COLS  13

/* ── Display configuration ─────────────────────────────────── */
#define BOARD_DISPLAY_BACKEND_OLED
#define BOARD_DISPLAY_BUS           DISPLAY_BUS_I2C
#define BOARD_DISPLAY_WIDTH         128
#define BOARD_DISPLAY_HEIGHT        64
#define BOARD_DISPLAY_CLK_HZ        (400 * 1000)
#define BOARD_DISPLAY_RESET         GPIO_NUM_NC
#define BOARD_DISPLAY_I2C_HOST      I2C_NUM_0
#define BOARD_DISPLAY_I2C_SDA       GPIO_NUM_15
#define BOARD_DISPLAY_I2C_SCL       GPIO_NUM_7
#define BOARD_DISPLAY_I2C_ADDR      0x3C
#define BOARD_DISPLAY_I2C_PULLUPS   true

/* ── UI scale & font ───────────────────────────────────────── */
#define UI_SCALE  1
#define UI_FONT   &lv_font_montserrat_14

/* ── LED strip ─────────────────────────────────────────────── */
#define BOARD_HAS_LED_STRIP       0

/* ── Matrix scanning ───────────────────────────────────────── */
#define BOARD_MATRIX_COL2ROW
#define BOARD_MATRIX_SCAN_INTERVAL_US  1000
#define BOARD_MATRIX_SETTLING_US       0
#define BOARD_MATRIX_RECOVERY_US       0

/* ── USB identification ────────────────────────────────────── */
#define BOARD_USB_VID             0xCafe
#define BOARD_USB_PID             0x4001

/* ── Debounce ──────────────────────────────────────────────── */
#define BOARD_DEBOUNCE_TICKS      3

/* ── Display sleep timeout (ms of inactivity) ──────────────── */
#define BOARD_DISPLAY_SLEEP_MS    60000

/* ── Deep sleep (minutes of inactivity, 0 to disable) ──────── */
#define BOARD_SLEEP_MINS          45

#endif /* BOARD_H */
