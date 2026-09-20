#ifndef BOARD_H
#define BOARD_H

/* Niphargus — LEFT half (U6), the MASTER.
 *
 * Pinout: docs/NIPHARGUS_V2_HARDWARE.md, verified against the netlist on 2026-08-06.
 * ⚠ The RIGHT half's table is different (routing permutations) —
 * never copy one from the other.
 * Locked by test/test_niphar_left_pins.c.
 */

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/spi_master.h"
#endif

/* ── Identité produit ── */
#define GATTS_TAG           "Niphargus_L"
#define MANUFACTURER_NAME   "Mae"
#define PRODUCT_NAME        "Niphargus Left"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0x10

/* ── Matrix: COL → switch → diode → ROW ────────────────────────
 * The scan DRIVES the columns and READS the rows (BOARD_MATRIX_COL2ROW).
 * Deep sleep wake: EXT1 on the ROWS (all in the RTC domain). */
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


/* 26 keys, rows of 7/7/6/6: two positions of the grid are empty. */
#define MATRIX_ROWS  4
#define MATRIX_COLS  7

/* Pin tables read by the core (matrix_scan.c, veille.c): the firmware has no
 * matrix shape of its own, MATRIX_ROWS/MATRIX_COLS entries each. */
#define BOARD_ROW_PINS { ROWS0, ROWS1, ROWS2, ROWS3 }
#define BOARD_COL_PINS { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6 }

/* The left half is the keyboard's ONLY keymap engine: it carries the keycodes
 * for all 52 keys even though it only scans 26. Columns 0-6 = this half,
 * 7-13 = the right, whose received coordinate is offset by MATRIX_COLS.
 * Locked by test/test_niphar_keymap_span.c. */
#define KEYMAP_COLS  14

/* Both halves are the SAME PCB flipped over. Column 0 of this half is
 * its leftmost key; by symmetry, column 0 of the right is its
 * rightmost key. Its coordinates are therefore stored backwards in the
 * keymap: column 0 → 13, column 6 → 7.
 *
 * Observed on the bench on 2026-09-07: without this, typing the right's home
 * row produces ";lkjh" instead of "hjkl;". The right emits its
 * physical coordinates and doesn't need to know where it's placed — the
 * conversion belongs to the master, see half_col_to_keymap() in comm/rf/half_link.h. */
#define BOARD_REMOTE_COLS_MIRRORED  1

/* ── nRF24L01+ radio (SPI2, shared with the display on the right side) ── */
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
 * The fallback is therefore skipped and these aliases must exist here, or
 * every consumer of the RF stack stops compiling for this half.
 * Locked by test/test_niphar_left_pins.c.
 *
 * Neither BOARD_NRF_CHANNEL nor BOARD_NRF_ADDR_SUFFIX: a half carries TWO links
 * (PRX toward the other half, PTX toward the dongle), a single channel wouldn't
 * make sense. Their choice belongs to B3/B4 — see
 * docs/superpowers/specs/2026-08-19-niphargus-firmware-design.md. */
#define BOARD_NRF_SPI_SCK       BOARD_NRF_SCK
#define BOARD_NRF_SPI_MISO      BOARD_NRF_MISO
#define BOARD_NRF_SPI_MOSI      BOARD_NRF_MOSI
#define BOARD_NRF_CSN_GPIO      BOARD_NRF_CSN
#define BOARD_NRF_CE_GPIO       BOARD_NRF_CE
#define BOARD_NRF_IRQ_GPIO      BOARD_NRF_IRQ
#define BOARD_NRF_CLOCK_HZ      (8 * 1000 * 1000)
#define BOARD_NRF_SPI_CLOCK_HZ  BOARD_NRF_CLOCK_HZ

/* MASTER -> DONGLE link. Must agree with board_rf_radio1_cfg() in
 * boards/kase_dongle/board_rf.h: channel 0x4C (2476 MHz), address "KaSe" +
 * suffix 0x01, the keyboard slot of comm/rf/rf_slot.h.
 *
 * ⚠ This is NOT the right -> left link. The left half carries two
 * (PRX toward the right, PTX toward the dongle); this is the second one. The
 * first will get its own macros once B3 is written. */
#define BOARD_NRF_CHANNEL       0x4C
#define BOARD_NRF_ADDR_SUFFIX   0x01


/* ── Inter-half link (TRRS, UART1) ─────────────────────────────
 * Straight cable: TX arrives on TX. ONE half must swap TXD/RXD via the
 * GPIO matrix — that's the left one. Never drive both TX without this swap. */
#define BOARD_LINK_UART_NUM    1
#define BOARD_LINK_TX          GPIO_NUM_17
#define BOARD_LINK_RX          GPIO_NUM_18
#define BOARD_LINK_SWAP_TX_RX  1
/* SiP32431 ON pin, 100 k pull-down: 5 V is DEAD by default. Only raise it
 * after a successful handshake (link_handshake.h). */
#define BOARD_LINK_5V_EN       GPIO_NUM_21

/* ── Battery gauge ─────────────────────────────────────────────
 * ADC2_CH2, 1M/1M + 100 nF divider. ADC2 is usable because there's
 * no WiFi; full battery ≈ 4.15 V ÷ 2. */
#define BOARD_VBAT_SENSE_GPIO  GPIO_NUM_13

/* ── Azoteq TPS43 trackpad (IQS572) — left only ────────────────
 * Trackpad's NRST = hardware RC, no GPIO. RDY mandatory (handshake). */
#define BOARD_HAS_TRACKPAD_LOCAL  1
#define BOARD_TRACK_SDA_GPIO      GPIO_NUM_47
#define BOARD_TRACK_SCL_GPIO      GPIO_NUM_48
#define BOARD_TRACK_RDY_GPIO      GPIO_NUM_42

/* ── Sharp LS011B7DH03 display (nice!view-type module, J12) ───
 * Soldered on 2026-09-14 (the user: "the screens are soldered on both").
 * Same module as the right, same bus: shares the nRF24's SPI (write-only,
 * no MISO), CS ACTIVE HIGH on GPIO14, mounted in PORTRAIT (68 wide × 160
 * tall). Driver: display/memlcd. CS is held LOW from boot so that
 * the screen never listens to the shared bus's radio traffic. */
#define BOARD_DISPLAY_BACKEND_MEMLCD
#define BOARD_LCD_CS_GPIO         GPIO_NUM_14
#define BOARD_LCD_CS_ACTIVE_HIGH  1
#define BOARD_LCD_ROTATE_180      0
#define BOARD_DISPLAY_WIDTH       68
#define BOARD_DISPLAY_HEIGHT      160
#define BOARD_DISPLAY_SLEEP_MS    60000
#define BOARD_HAS_LED_STRIP  0

/* ── Matrix scan ───────────────────────────────────────────────
 * SETTLING/RECOVERY at 0 reuses the KaSe boards' setting. Suspected of
 * contributing to missed keystrokes (docs/DONGLE_ARCHI_ET_HALF_TYPING_2026-07-13.md,
 * bug #2) — to be measured on the bench, don't tune it blind. */
#define BOARD_MATRIX_COL2ROW
#define BOARD_MATRIX_SCAN_INTERVAL_US  1000
#define BOARD_MATRIX_SETTLING_US       0
#define BOARD_MATRIX_RECOVERY_US       0
#define BOARD_DEBOUNCE_TICKS           5   /* 5 ms: the dongle replays every transition, it no longer masks a 3-5 ms bounce (2026-09-20) */

/* ── USB ── */
#define BOARD_USB_VID  0xCafe
#define BOARD_USB_PID  0x4003

#endif /* BOARD_H */

/* Every GPIO this board drives or reads — the generic board contract
 * (test/board_contract.inc) checks the list: no GPIO twice, valid numbers,
 * no strapping pin, reserved pins respected. Add here whatever you add above. */
#define BOARD_USES_NATIVE_USB 1
#define BOARD_PINS(X) \
    X(ROWS0) X(ROWS1) X(ROWS2) X(ROWS3) \
    X(COLS0) X(COLS1) X(COLS2) X(COLS3) X(COLS4) X(COLS5) X(COLS6) \
    X(BOARD_NRF_SCK) X(BOARD_NRF_MISO) X(BOARD_NRF_MOSI) X(BOARD_NRF_CE) X(BOARD_NRF_CSN) X(BOARD_NRF_IRQ) \
    X(BOARD_LINK_TX) X(BOARD_LINK_RX) X(BOARD_LINK_5V_EN) X(BOARD_VBAT_SENSE_GPIO) \
    X(BOARD_TRACK_SDA_GPIO) X(BOARD_TRACK_SCL_GPIO) X(BOARD_TRACK_RDY_GPIO) X(BOARD_LCD_CS_GPIO)
