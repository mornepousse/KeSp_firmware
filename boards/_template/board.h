/* __BOARD_NAME__ — board definition. Created from boards/_template by
 * scripts/new-board.sh. This folder is the whole registration of the board:
 * CMake, check.sh and the host contract test discover it from here.
 *
 * Fill the pins first, then BOARD_PINS(X) (every GPIO you use, the contract
 * test checks it), then the optional blocks you need. Delete what you don't
 * have — a macro that describes hardware you don't have is a bug waiting.
 *
 * Wiring rule for key wake-up from sleep: COL → switch → diode → ROW, the
 * scan drives the columns and reads the rows (BOARD_MATRIX_COL2ROW). */
#ifndef BOARD_H
#define BOARD_H

/* ── Product info (USB strings, BLE name) ───────────────────── */
#define GATTS_TAG           "__BOARD_NAME__"
#define MANUFACTURER_NAME   "Me"
#define PRODUCT_NAME        "__BOARD_NAME__"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0x10          /* any unique byte per board */

/* ── Matrix pins — one macro per row and column, ESP32-S3 GPIO numbers ── */
#define ROWS0  GPIO_NUM_1
#define ROWS1  GPIO_NUM_2
#define ROWS2  GPIO_NUM_8
#define ROWS3  GPIO_NUM_6
#define COLS0  GPIO_NUM_4
#define COLS1  GPIO_NUM_5
#define COLS2  GPIO_NUM_7
#define COLS3  GPIO_NUM_9
#define COLS4  GPIO_NUM_10
#define COLS5  GPIO_NUM_11
#define COLS6  GPIO_NUM_12

/* ── Matrix geometry and the tables the core reads ───────────
 * BOARD_ROW_PINS / BOARD_COL_PINS: exactly MATRIX_ROWS / MATRIX_COLS entries.
 * KEYMAP_COLS defaults to MATRIX_COLS; a split master sets it to the total. */
#define MATRIX_ROWS  4
#define MATRIX_COLS  7
#define BOARD_ROW_PINS { ROWS0, ROWS1, ROWS2, ROWS3 }
#define BOARD_COL_PINS { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6 }

/* ── Every GPIO this board drives or reads ──────────────────
 * test/board_contract.inc checks: no GPIO twice, 0..48, no strapping pin
 * (0/3/45/46), no native-USB pin (19/20) when BOARD_USES_NATIVE_USB, no
 * octal-PSRAM pin (35-37) when BOARD_PSRAM_OCTAL. Add here whatever you add
 * below. */
#define BOARD_USES_NATIVE_USB 1
#define BOARD_PINS(X) \
    X(ROWS0) X(ROWS1) X(ROWS2) X(ROWS3) \
    X(COLS0) X(COLS1) X(COLS2) X(COLS3) X(COLS4) X(COLS5) X(COLS6)

/* ── Matrix scanning ───────────────────────────────────────── */
#define BOARD_MATRIX_COL2ROW                  /* drive columns, read rows */
#define BOARD_MATRIX_SCAN_INTERVAL_US  1000
#define BOARD_MATRIX_SETTLING_US       0
#define BOARD_MATRIX_RECOVERY_US       0
#define BOARD_DEBOUNCE_TICKS           5

/* ── USB identification ────────────────────────────────────── */
#define BOARD_USB_VID   0xCafe
#define BOARD_USB_PID   0x4010                /* unique per board */

/* ── Display: none by default. Pick ONE backend or leave all out. ──
 * OLED I2C (V2):      BOARD_DISPLAY_BACKEND_OLED  + BOARD_DISPLAY_I2C_* (see boards/kase_v2/board.h)
 * Round SPI (V1):     BOARD_DISPLAY_BACKEND_ROUND + BOARD_DISPLAY_SPI_*  (see boards/kase_v1/board.h)
 * Sharp memory-LCD:   BOARD_DISPLAY_BACKEND_MEMLCD + BOARD_LCD_CS_GPIO, CONFIG_KASE_DISPLAY_MEMLCD=y
 *                     (see boards/niphar_right/board.h) — shares the nRF24 SPI bus.
 * CMakeLists.txt reads the TEXT of this file for the backend macro. */
#define BOARD_DISPLAY_SLEEP_MS    60000

/* ── LED strip (V1 only today) ─────────────────────────────── */
#define BOARD_HAS_LED_STRIP       0

/* ── Optional: nRF24L01+ radio (CONFIG_KASE_KBD_WIRELESS / split) ──
 * #define BOARD_NRF_SPI_HOST  SPI2_HOST
 * #define BOARD_NRF_SCK   GPIO_NUM_38
 * #define BOARD_NRF_MISO  GPIO_NUM_39
 * #define BOARD_NRF_MOSI  GPIO_NUM_40
 * #define BOARD_NRF_CE    GPIO_NUM_15
 * #define BOARD_NRF_CSN   GPIO_NUM_16
 * #define BOARD_NRF_IRQ   GPIO_NUM_41
 * #define BOARD_NRF_CHANNEL      RF_CH_KBD_DONGLE   (comm/rf/rf_slot.h owns the channel plan)
 * #define BOARD_NRF_ADDR_SUFFIX  0x01
 * → add the six pins to BOARD_PINS(X). Reference: boards/niphar_left/board.h */

/* ── Optional: battery gauge (CONFIG_KASE_BATT_SENSE, ADC, 1M/1M divider) ──
 * #define BOARD_VBAT_SENSE_GPIO  GPIO_NUM_13     → add to BOARD_PINS(X) */

/* ── Optional: wired TRRS link between halves (CONFIG_KASE_LINK_WIRE) ──
 * #define BOARD_LINK_UART_NUM  UART_NUM_1
 * #define BOARD_LINK_TX        GPIO_NUM_17
 * #define BOARD_LINK_RX        GPIO_NUM_18
 * #define BOARD_LINK_5V_EN     GPIO_NUM_21       → add the three to BOARD_PINS(X) */

/* ── Sleep (CONFIG_KASE_VEILLE: light sleep after 15 s, deep after 4 h) ── */
#define BOARD_SLEEP_MINS          45              /* legacy V1/V2 deep-sleep path when KASE_VEILLE=n */

#endif /* BOARD_H */
