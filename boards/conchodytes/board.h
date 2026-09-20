#ifndef BOARD_H
#define BOARD_H

/* Conchodytes — the mouse of the KaSe set.
 *
 * Replacement PCB for a Logitech M100 shell: ESP32-S3-WROOM-1U, PMW3389DM-T3QU
 * sensor, nRF24L01+ to the dongle's slot 2.
 * Hardware: ~/Documents/GitHub/Conchodytes
 * Design  : docs/superpowers/specs/2026-08-25-conchodytes-firmware-design.md
 *
 * Pinout read from the netlist via `kicad-cli export netlist` on 2026-08-25, and
 * verified on the board the same day. DO NOT copy it from the hardware repo's
 * README: it had left and right swapped on the NC contacts.
 * Locked by test/test_conchodytes_pins.c.
 */

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/spi_master.h"
#endif

/* ── Identité produit ── */
#define GATTS_TAG           "Conchodytes"
#define MANUFACTURER_NAME   "Mae"
#define PRODUCT_NAME        "Conchodytes"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0xC0   /* mouse — distinct from the dongle 0xD0 and the halves 0x01/0x02 */

/* ── No matrix, no display, no trackpad ─────────────────────────
 *
 * A mouse has no matrix keys: its three clicks are read
 * individually, in NO+NC pairs (see below). No MATRIX_ROWS,
 * MATRIX_COLS, ROWSn or COLSn here — the keymap engine is not compiled for this
 * role (main/CMakeLists.txt), so nothing needs them, and a board.h that
 * declared an imaginary keyboard would eventually mislead someone.
 *
 * The absence of BOARD_DISPLAY_BACKEND_* is also deliberate: the
 * root CMakeLists detects this absence and skips the display sources.
 */

/* ── SPI bus, SHARED between the sensor and the radio ────────────
 *
 * SCK/MOSI/MISO are common to the PMW3389 and the nRF24, each behind a 100 Ω
 * series resistor (R30/R18/R16 on the board — no effect at these frequencies).
 *
 * The two devices do NOT share the same SPI mode: mode 0 for the nRF24,
 * mode 3 for the PMW3389. ESP-IDF reconfigures the mode per device, but
 * mutual exclusion remains our responsibility: only one CS low at a time, both
 * pulled high at startup before any initialization, and the bus held
 * during the SROM upload (~61 ms) or it gets corrupted.
 */
#define BOARD_NRF_SPI_HOST   SPI2_HOST
#define BOARD_NRF_SCK        GPIO_NUM_38
#define BOARD_NRF_MISO       GPIO_NUM_39
#define BOARD_NRF_MOSI       GPIO_NUM_40

/* ── nRF24L01+ radio ────────────────────────────────────────────
 * Dongle's slot 2. boards/kase_dongle/board.h annotates BOARD_NRF2_* as
 * "mouse slot (Conchodytes), channel 0x52 by default": both sides
 * must agree or the link never comes up. */
#define BOARD_NRF_CSN        GPIO_NUM_2
#define BOARD_NRF_CE         GPIO_NUM_1
#define BOARD_NRF_IRQ        GPIO_NUM_41
#define BOARD_NRF_CLOCK_HZ   (8 * 1000 * 1000)
#define BOARD_NRF_CHANNEL    0x52
#define BOARD_NRF_ADDR_SUFFIX 0x02   /* 0x01 = keyboard, 0x02 = mouse (rf_slot.h) */

/* ── Pinout aliases: TWO conventions coexist in this repo ─────────────────
 *
 * The Niphargus board.h files name the lines `BOARD_NRF_SCK`, `BOARD_NRF_CSN`…
 * while comm/rf/kbd_relay_tx.c expects `BOARD_NRF_SPI_SCK`,
 * `BOARD_NRF_CSN_GPIO`… and falls back to default values when it doesn't
 * find them.
 *
 * ⚠ These fallback values are GPIO35/37/36/34/33/38 — chosen for the KaSe V2
 * PCB, with no relation to this board. A board.h that declares only
 * the first convention therefore compiles perfectly and drives the wrong
 * pins, silently. Hence these aliases: they duplicate nothing, they translate.
 */
#define BOARD_NRF_SPI_SCK       BOARD_NRF_SCK
#define BOARD_NRF_SPI_MISO      BOARD_NRF_MISO
#define BOARD_NRF_SPI_MOSI      BOARD_NRF_MOSI
#define BOARD_NRF_SPI_CLOCK_HZ  BOARD_NRF_CLOCK_HZ
#define BOARD_NRF_CSN_GPIO      BOARD_NRF_CSN
#define BOARD_NRF_CE_GPIO       BOARD_NRF_CE
#define BOARD_NRF_IRQ_GPIO      BOARD_NRF_IRQ

/* ── PMW3389DM-T3QU sensor ─────────────────────────────────────
 *
 * PixArt datasheet Version 1.0 | 07 Sep 2017. Validated on the board on 2026-08-25:
 * Product_ID 0x47, Inverse 0xB8, Revision 0x01, SROM_ID 0xE8 after
 * upload, movement read.
 *
 * ⚠ This is NOT a PMW3360, despite what the hardware repo's file names
 * still say. Different SROM blob, partially different register map,
 * longer SPI timings (tSRAD 160 µs vs 35).
 */
#define BOARD_SNS_NCS_GPIO     GPIO_NUM_17
#define BOARD_SNS_MOTION_GPIO  GPIO_NUM_18
#define BOARD_SNS_SPI_MODE     3            /* CPOL=1, CPHA=1 */
#define BOARD_SNS_CLOCK_HZ     (2 * 1000 * 1000)  /* fSCLK max, Table 4 p. 15 */

/* Sensor resolution, in cpi.
 *
 * 1000 cpi is the original M100's value, whose shell this board reuses:
 * keeping it the same avoids the replacement feeling different under the hand.
 * The PMW3389 goes up to 16000 (datasheet p. 1) and it starts near that
 * ceiling if nobody tells it otherwise — the firmware wasn't setting it, hence
 * a far too fast cursor on the bench on 2026-08-26. */
#define BOARD_SNS_CPI          1000

/* Is the sensor mounted rotated 180° relative to the mouse's axis?
 *
 * ⚠ ON v1, YES — AND IT CHANGES ON v2. Measured on 2026-08-25 (NOTES-V2 §1,
 * Conchodytes repo): gesture to the right → negative X, 100.0% bias over 76
 * samples; gesture forward → positive Y, 99.8% bias over 71. BOTH
 * axes flipped and each stayed clean — the signature of a 180° rotation and
 * not a mirror, which would only invert one of them.
 *
 * ⚠ SET TO 0 WHEN THE v2 LAYOUT ROTATES U2 BY 180°. The footprint's
 * rotation and this constant fix THE SAME defect: leaving both
 * active would reintroduce it backwards. v2 is conditioned on whether the
 * LM19-LSI lens accepts the rotation inside the M100 shell — until that's
 * settled, the fix lives here. */
#define BOARD_SNS_ROT_180      1

/* Adaptive smoothing of the movement, in Q8 (256 = 1.0). At 0, no smoothing.
 *
 * ⚠ A WORKAROUND FOR AN OPTICAL DEFECT, not a fix. The sensor jitters because
 * its SQUAL is at 30-55 for ~80 expected — see NOTES-V2 §8 (Conchodytes
 * repo). If the optics get repaired, SET `ALPHA_MIN` BACK to 256 (i.e.
 * no smoothing) rather than leaving a delay that no longer buys anything.
 *
 * ALPHA_MIN governs the smoothing AT REST AND ON SLOW GESTURES: the lower it
 * is, the more it's smoothed and the longer the delay. 38/256 ≈ 0.15, i.e. a
 * time constant of 4 ms / 0.15 ≈ 27 ms at 250 Hz.
 *
 * VITESSE_MAX is the threshold, in counts per sample, beyond which smoothing
 * STOPS ENTIRELY. 8 counts at 250 Hz = 2000 counts/s ≈ 5 cm/s: any sharp
 * gesture therefore passes with no delay at all, only low-amplitude noise is
 * attenuated. */
#define BOARD_SNS_LISSAGE_ALPHA_MIN   38
#define BOARD_SNS_LISSAGE_VITESSE_MAX 8

/* ── Clicks: three SPDT, debounce via the NC contact ────────────
 *
 * COM to ground, NO and NC each pulled to 3.3 V by 10 k (R105-R107 and
 * R108-R110). The firmware reads BOTH contacts:
 *
 *   idle    : NC stuck to COM -> low ; NO open -> high
 *   pressed : NO stuck to COM -> low ; NC open -> high
 *   bounce  : BOTH high — keep the previous state
 *
 * Verified on the board on 2026-08-25: over 24 transitions, NO spurious edge.
 * No double-click without a single time filter or a constant to tune —
 * but this is true ONLY if NO and NC belong to the same button.
 *
 * ⚠ The bounce DURATION is not measured: the campaign believed it was sampling
 * at 1 kHz while CONFIG_FREERTOS_HZ defaults to 100, i.e. 10 ms per sample.
 * See main/input/mouse_buttons.h.
 *
 * ⚠ The hardware repo's README claimed LEFT_NC=GPIO4 and RIGHT_NC=GPIO5.
 * The netlist says the opposite: GPIO4 -> SW2.3 (RIGHT click, R109), GPIO5 -> SW1.3
 * (LEFT click, R108). Verified on the board: pressing only the left click
 * produced 26 edges on L and 0 on R.
 */
#define BOARD_SW_LEFT_GPIO      GPIO_NUM_10
#define BOARD_SW_LEFT_NC_GPIO   GPIO_NUM_5
#define BOARD_SW_RIGHT_GPIO     GPIO_NUM_11
#define BOARD_SW_RIGHT_NC_GPIO  GPIO_NUM_4
#define BOARD_SW_MID_GPIO       GPIO_NUM_12
#define BOARD_SW_MID_NC_GPIO    GPIO_NUM_6

/* ── Wheel: NOT FUNCTIONAL ON v1 — a design defect ──────────────
 *
 * ⚠⚠ LQ1 IS WIRED BACKWARDS ON v1. Established on 2026-08-26 by comparing the
 * M100's original schematic (`USB-Mouse-main/Mouse.sch`) against the actual wiring.
 *
 * LQ1 is NOT a dual phototransistor A/COM/B — it's a three-wire
 * VCC/GND/DATA sensor (marking `H6Y07`):
 *
 *   pin    | original M100          | Conchodytes v1
 *   -------|-----------------------|------------------------
 *      1   | Pin_4 = DATA output   | ENC_A + 10 k pull-up   ✅
 *      2   | Pin_2J = VCC          | GROUND                 ❌
 *      3   | GNDREF = GND          | ENC_B + 10 k pull-up   ❌
 *
 * The component is therefore powered backwards. Current flows through its
 * internal substrate diode and clamps both nets to 0.65 V, regardless of
 * the light. Verified on TWO separate components: it's the wiring, not them.
 *
 * The mistake comes from the `Optical_Mouse:LQ` symbol inherited from the fork,
 * whose pins carry no function name — only "1, 2, 3".
 *
 * ⚠ AND A SINGLE DATA OUTPUT = A SINGLE CHANNEL. Quadrature requires two. The
 * very principle of reading the wheel needs rethinking in v2, not just its
 * wiring. Open question: see Conchodytes/NOTES-V2.md §1bis.
 *
 * Consequence for this firmware: quadrature decoding
 * (input/mouse_wheel.c) is verified on host only, and cannot
 * be exercised on this board. The macros below describe the CURRENT
 * wiring, which is wrong — they will change with v2.
 */
#define BOARD_ENC_A_GPIO     GPIO_NUM_7
#define BOARD_ENC_B_GPIO     GPIO_NUM_9

/* ── Battery gauge ─────────────────────────────────────────────
 * 1M/1M + 100 nF divider bridge (R64/R67/C21), ÷2 ratio: 4.2 V full
 * charge -> 2.1 V at the input. The 1 MΩ is a power-consumption choice — 2.1 µA
 * of permanent leakage against 21 µA with 100 k — viable because C21 supplies
 * the ADC's sampling charge.
 *
 * ⚠ GPIO13 = ADC2_CH2, and on ESP32-S3 ADC2 is shared with the WiFi driver:
 * as long as this measurement is here, WiFi is forbidden on this board. No
 * consequence today — the radio is an nRF24 — but it's a mortgage.
 * NOTES-V2.md point 7 plans GPIO8 / ADC1_CH7 for v2.
 */
#define BOARD_VBAT_SENSE_GPIO   GPIO_NUM_13

/* ── No display, no LED strip ─────────────────────────────── */
#define BOARD_DISPLAY_SLEEP_MS    0
#define BOARD_SLEEP_MINS          0
#define BOARD_HAS_LED_STRIP       0

/* ── USB identification ────────────────────────────────────────
 * Same development VID/PID as the rest of the set until the first
 * publication. To migrate to pid.codes (VID 0x1209) before v4.0. */
#define BOARD_USB_VID             0x303A
#define BOARD_USB_PID             0x4002

#endif /* BOARD_H */

/* Every GPIO this board drives or reads — the generic board contract
 * (test/board_contract.inc) checks the list: no GPIO twice, valid numbers,
 * no strapping pin, reserved pins respected. Add here whatever you add above. */
#define BOARD_USES_NATIVE_USB 1
#define BOARD_PINS(X) \
    X(BOARD_NRF_SCK) X(BOARD_NRF_MISO) X(BOARD_NRF_MOSI) X(BOARD_NRF_CSN) X(BOARD_NRF_CE) X(BOARD_NRF_IRQ) \
    X(BOARD_SNS_NCS_GPIO) X(BOARD_SNS_MOTION_GPIO) \
    X(BOARD_SW_LEFT_GPIO) X(BOARD_SW_LEFT_NC_GPIO) X(BOARD_SW_RIGHT_GPIO) X(BOARD_SW_RIGHT_NC_GPIO) \
    X(BOARD_SW_MID_GPIO) X(BOARD_SW_MID_NC_GPIO) X(BOARD_ENC_A_GPIO) X(BOARD_ENC_B_GPIO) X(BOARD_VBAT_SENSE_GPIO)
