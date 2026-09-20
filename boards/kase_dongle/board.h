#ifndef BOARD_H
#define BOARD_H

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "sdkconfig.h"     /* CONFIG_KASE_DONGLE_FUSION — see the dims below */
#endif

/* ── Product info ──────────────────────────────────────────── */
#define GATTS_TAG           "KaSe_Dongle"
#define MANUFACTURER_NAME   "KaSe"
#define PRODUCT_NAME        "KaSe Dongle"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0xD0   /* dongle, distinct from halves 0x01/0x02 */

/* ── No matrix ─────────────────────────────────────────────────
 *
 * This board used to declare a 5x14 matrix, a full pinout in GPIO_NUM_NC and
 * a 70-key keymap. None of that exists: the dongle has no switches at all.
 * It was a trace of architecture A, where it merged the half-matrices of the
 * old halves and applied the keymap itself.
 *
 * The Niphargus sends it HID that is already finished, so the declaration was
 * only describing an imaginary keyboard — and a `board.h` that lies about its
 * hardware ends up misleading someone. The input engine is no longer compiled
 * for this role (main/CMakeLists.txt), nor is the KEYBOARD block of the CDC
 * protocol, so nothing needs it any more.
 */

/* ── Fusion dimensions (KASE_DONGLE_FUSION only) ─────────────────────────────
 *
 * In fusion mode, the dongle GETS BACK the keymap engine: it receives BOTH
 * raw half-matrices from the halves, merges them and outputs HID. It therefore
 * shares the geometry and default keymap of the LEFT half (boards/niphar_left) —
 * same dimensions, a single keymap file redirected (board_keymap.c / .c here
 * only include those of the left half). The replicated NVS remains the
 * runtime authority (phase 3). Design: docs/superpowers/specs/2026-09-12-dongle-fusion-*.
 *
 * These macros do NOT exist outside fusion: without an engine, nobody reads
 * them, and a board.h must not describe a matrix the board does not have.
 * That is also why they are guarded rather than hard-declared. */
#if defined(CONFIG_KASE_DONGLE_FUSION)
#define MATRIX_ROWS  4
#define MATRIX_COLS  7
#define KEYMAP_COLS  14
/* Both halves are the same PCB flipped over: the right half's columns run
 * high, reversed. The conversion belongs to the engine (half_col_to_keymap). */
#define BOARD_REMOTE_COLS_MIRRORED  1
#endif

/* ── NRF24L01+ pinout (extracted from dongle.kicad_sch netlist) ─ */
#define BOARD_NRF_SPI_HOST       SPI2_HOST
#define BOARD_NRF_SPI_MOSI       GPIO_NUM_5
#define BOARD_NRF_SPI_MISO       GPIO_NUM_6
#define BOARD_NRF_SPI_SCK        GPIO_NUM_7
#define BOARD_NRF_SPI_CLOCK_HZ   (10 * 1000 * 1000)   /* 10 MHz, NRF24 datasheet max */

/* NRF#1 = keyboard slot (Niphargus master half), channel 0x4C by default */
#define BOARD_NRF1_CSN_GPIO      GPIO_NUM_13
#define BOARD_NRF1_CE_GPIO       GPIO_NUM_14
#define BOARD_NRF1_IRQ_GPIO      GPIO_NUM_8

/* NRF#2 = mouse slot (Conchodytes), channel 0x52 by default */
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

/* ── USB identification ────────────────────────────────────── */
/* Dev VID/PID until first public release.
 * Migrate to pid.codes (VID 0x1209) before v4.0 release. */
#define BOARD_USB_VID             0x303A
#define BOARD_USB_PID             0x4001

#endif /* BOARD_H */

/* Every GPIO this board drives or reads — the generic board contract
 * (test/board_contract.inc) checks the list: no GPIO twice, valid numbers,
 * no strapping pin, reserved pins respected. Add here whatever you add above. */
#define BOARD_USES_NATIVE_USB 1
#define BOARD_PINS(X) \
    X(BOARD_NRF_SPI_MOSI) X(BOARD_NRF_SPI_MISO) X(BOARD_NRF_SPI_SCK) \
    X(BOARD_NRF1_CSN_GPIO) X(BOARD_NRF1_CE_GPIO) X(BOARD_NRF1_IRQ_GPIO) \
    X(BOARD_NRF2_CSN_GPIO) X(BOARD_NRF2_CE_GPIO) X(BOARD_NRF2_IRQ_GPIO)
