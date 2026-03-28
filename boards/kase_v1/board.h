#ifndef BOARD_H
#define BOARD_H

/* ── Product info ──────────────────────────────────────────── */
#define GATTS_TAG           "KaSe_V1"
#define MANUFACTURER_NAME   "Mae"
#define PRODUCT_NAME        "KaSe V1"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0x01

/* ── Matrix GPIO pins ──────────────────────────────────────── */
#define ROWS0  GPIO_NUM_3
#define ROWS1  GPIO_NUM_36
#define ROWS2  GPIO_NUM_39
#define ROWS3  GPIO_NUM_40
#define ROWS4  GPIO_NUM_37

#define COLS0  GPIO_NUM_21
#define COLS1  GPIO_NUM_47
#define COLS2  GPIO_NUM_48
#define COLS3  GPIO_NUM_45
#define COLS4  GPIO_NUM_35
#define COLS5  GPIO_NUM_38
#define COLS6  GPIO_NUM_9
#define COLS7  GPIO_NUM_10
#define COLS8  GPIO_NUM_11
#define COLS9  GPIO_NUM_12
#define COLS10 GPIO_NUM_13
#define COLS11 GPIO_NUM_14
#define COLS12 GPIO_NUM_46

/* ── Matrix dimensions ─────────────────────────────────────── */
#define MATRIX_ROWS  5
#define MATRIX_COLS  13

/* ── Display configuration ─────────────────────────────────── */
#define BOARD_DISPLAY_BACKEND_ROUND
#define BOARD_DISPLAY_BUS       DISPLAY_BUS_SPI
#define BOARD_DISPLAY_WIDTH     240
#define BOARD_DISPLAY_HEIGHT    240
#define BOARD_DISPLAY_CLK_HZ    (20 * 1000 * 1000)
#define BOARD_DISPLAY_RESET     GPIO_NUM_44
#define BOARD_DISPLAY_SPI_HOST  SPI2_HOST
#define BOARD_DISPLAY_SPI_SCLK  GPIO_NUM_41
#define BOARD_DISPLAY_SPI_MOSI  GPIO_NUM_42
#define BOARD_DISPLAY_SPI_CS    GPIO_NUM_1
#define BOARD_DISPLAY_SPI_DC    GPIO_NUM_2
#define BOARD_DISPLAY_SPI_BL    GPIO_NUM_NC

/* ── UI scale & font ───────────────────────────────────────── */
#define UI_SCALE  2
#define UI_FONT   &lv_font_montserrat_28

/* ── LED strip ─────────────────────────────────────────────── */
#define BOARD_HAS_LED_STRIP       1
#define BOARD_LED_STRIP_GPIO      4
#define BOARD_LED_STRIP_NUM_LEDS  17

/* ── Matrix scanning ───────────────────────────────────────── */
#define BOARD_MATRIX_COL2ROW
#define BOARD_MATRIX_SCAN_INTERVAL_US  1000
#define BOARD_MATRIX_SETTLING_US       20
#define BOARD_MATRIX_RECOVERY_US       50

/* ── Position mapping (V1 <-> V2 translation for CDC) ──────── */
#define BOARD_HAS_POSITION_MAP    1

/* ── USB identification ────────────────────────────────────── */
#define BOARD_USB_VID             0xCafe
#define BOARD_USB_PID             0x4001

/* ── Debounce ──────────────────────────────────────────────── */
#define BOARD_DEBOUNCE_TICKS      5

/* ── Display sleep timeout (ms of inactivity) ──────────────── */
#define BOARD_DISPLAY_SLEEP_MS    60000

/* ── Display enabled (perf fixed: lower prio + bigger buffer) ── */
#define SKIP_STATUS_DISPLAY       0

/* ── Deep sleep (minutes of inactivity, 0 to disable) ──────── */
#define BOARD_SLEEP_MINS          45

#endif /* BOARD_H */
