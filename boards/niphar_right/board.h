#ifndef BOARD_H
#define BOARD_H

/* Niphargus — RIGHT half (U5), the SLAVE.
 *
 * Pinout: docs/NIPHARGUS_V2_HARDWARE.md, verified against the netlist on 2026-08-06.
 * ⚠ The LEFT half's table is different (routing permutations) —
 * never copy one from the other.
 * Locked by test/test_niphar_right_pins.c.
 */

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/spi_master.h"
#endif

/* ── Product identity ── */
#define GATTS_TAG           "Niphargus_R"
#define MANUFACTURER_NAME   "Mae"
#define PRODUCT_NAME        "Niphargus Right"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0x11

/* ── Matrix: COL → switch → diode → ROW ──────────────────────
 * The scan DRIVES the columns and READS the rows (BOARD_MATRIX_COL2ROW).
 * RIGHT table — NOT a mirror of the left half: of the 11 matrix
 * pins, only one coincides between the two halves (col3 = GPIO9,
 * common routing); the other 10 (row0..row3, col0..col2, col4..col6)
 * differ. Never copy one from the other. */
#define ROWS0  GPIO_NUM_2
#define ROWS1  GPIO_NUM_12
#define ROWS2  GPIO_NUM_4
#define ROWS3  GPIO_NUM_5

#define COLS0  GPIO_NUM_6
#define COLS1  GPIO_NUM_7
#define COLS2  GPIO_NUM_8
#define COLS3  GPIO_NUM_9
#define COLS4  GPIO_NUM_11
#define COLS5  GPIO_NUM_10
#define COLS6  GPIO_NUM_1


/* 26 keys, rows of 7/7/6/6: two grid positions are empty. */
#define MATRIX_ROWS  4
#define MATRIX_COLS  7

/* Pin tables read by the core (matrix_scan.c, veille.c): the firmware has no
 * matrix shape of its own, MATRIX_ROWS/MATRIX_COLS entries each. */
#define BOARD_ROW_PINS { ROWS0, ROWS1, ROWS2, ROWS3 }
#define BOARD_COL_PINS { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6 }

/* ── nRF24L01+ radio (SPI2, shared with the screen) ──────────────── */
#define BOARD_NRF_SPI_HOST   SPI2_HOST
#define BOARD_NRF_SCK        GPIO_NUM_38
#define BOARD_NRF_MISO       GPIO_NUM_39
#define BOARD_NRF_MOSI       GPIO_NUM_40
#define BOARD_NRF_CE         GPIO_NUM_15
#define BOARD_NRF_CSN        GPIO_NUM_16
#define BOARD_NRF_IRQ        GPIO_NUM_41
/* Aliases expected by the RF stack. comm/rf/kbd_relay_tx.c builds its config
 * from BOARD_NRF_SPI_SCK, BOARD_NRF_CSN_GPIO... and its fallback block is
 * guarded by `#ifndef BOARD_NRF_SPI_HOST` — which we define above.
 * The fallback is thus skipped and these aliases must exist here, without
 * which any consumer of the RF stack stops compiling for this half.
 * Locked by test/test_niphar_right_pins.c.
 *
 * Neither BOARD_NRF_CHANNEL nor BOARD_NRF_ADDR_SUFFIX: a half carries TWO links
 * (PRX toward the other half, PTX toward the dongle), a single channel would
 * make no sense. Their choice belongs to B3/B4 — cf.
 * docs/superpowers/specs/2026-08-19-niphargus-firmware-design.md. */
#define BOARD_NRF_SPI_SCK       BOARD_NRF_SCK
#define BOARD_NRF_SPI_MISO      BOARD_NRF_MISO
#define BOARD_NRF_SPI_MOSI      BOARD_NRF_MOSI
#define BOARD_NRF_CSN_GPIO      BOARD_NRF_CSN
#define BOARD_NRF_CE_GPIO       BOARD_NRF_CE
#define BOARD_NRF_IRQ_GPIO      BOARD_NRF_IRQ


/* ── Inter-half link (TRRS, UART1) ──────────────────────────
 * Straight cable: TX arrives on TX. ONE half must swap TXD/RXD via the
 * GPIO matrix — that is the LEFT half. The right half does NOT swap: driving
 * both TX without a swap on one of the two sides would put two outputs in
 * conflict on the same wire. */
#define BOARD_LINK_UART_NUM    1
#define BOARD_LINK_TX          GPIO_NUM_17
#define BOARD_LINK_RX          GPIO_NUM_18
#define BOARD_LINK_SWAP_TX_RX  0
/* ON of the SiP32431, 100 k pull-down: the 5 V is DEAD by default. Only raise
 * it after a successful handshake (link_handshake.h). */
#define BOARD_LINK_5V_EN       GPIO_NUM_21

/* ── Battery gauge ────────────────────────────────────────────
 * ADC2_CH2, 1M/1M divider + 100 nF. ADC2 is usable because there is
 * no WiFi; full battery ≈ 4.15 V ÷ 2. */
#define BOARD_VBAT_SENSE_GPIO  GPIO_NUM_13

/* ── No trackpad on the right half (left only) ── */

/* ── Sharp LS011B7DH03 screen (nice!view-type module, J4) ──────
 * Same module as the left half (J12), same bus: shares the nRF24's SPI
 * (write-only, no MISO), CS ACTIVE HIGH on GPIO14, mounted in PORTRAIT
 * (68 wide × 160 tall). Driver: display/memlcd, proven on the bench on
 * 2026-09-14. CS is held LOW from boot and pulled low during sleep so that
 * the screen never listens to the shared bus's radio traffic. */
#define BOARD_DISPLAY_BACKEND_MEMLCD
#define BOARD_LCD_CS_GPIO         GPIO_NUM_14
#define BOARD_LCD_CS_ACTIVE_HIGH  1
#define BOARD_LCD_ROTATE_180      0
#define BOARD_DISPLAY_WIDTH       68    /* PORTRAIT: 68 wide × 160 tall */
#define BOARD_DISPLAY_HEIGHT      160
#define BOARD_DISPLAY_SLEEP_MS    60000

#define BOARD_HAS_LED_STRIP  0

/* ── Matrix scan ──────────────────────────────────────────────
 * SETTLING/RECOVERY at 0 reuses the KaSe boards' setting. Suspected of
 * contributing to missed keystrokes (docs/DONGLE_ARCHI_ET_HALF_TYPING_2026-07-13.md,
 * bug #2) — to be measured on the bench, do not tune blindly. */
#define BOARD_MATRIX_COL2ROW
#define BOARD_MATRIX_SCAN_INTERVAL_US  1000
#define BOARD_MATRIX_SETTLING_US       0
#define BOARD_MATRIX_RECOVERY_US       0
#define BOARD_DEBOUNCE_TICKS           5   /* 5 ms: the dongle replays every transition, it no longer masks a 3-5 ms bounce (2026-09-20) */

/* ── USB ──
 * No useful USB HID in production on the right half (it has neither a keymap
 * engine nor HID output); VID/PID kept for consistency if the programming port
 * is enumerated at bring-up. */
#define BOARD_USB_VID  0xCafe
#define BOARD_USB_PID  0x4004

#endif /* BOARD_H */
