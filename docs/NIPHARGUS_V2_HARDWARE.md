# Hardware contract — Niphargus v2, S3 halves

Source of truth: netlist of the schematic `Niphargus/rili/pcb/niphar.kicad_sch`, verified
pin by pin during the 2026-08-06 review (commits `faf3e11`→`c1f9cf6`). In case of
doubt, the netlist takes precedence over this document.

## MCU: ESP32-S3-WROOM-1(U)-N16R8 — one per half

> **Corrected at bring-up on 2026-09-01.** This document previously stated an
> **N8R2** (8 MB flash / 2 MB PSRAM). The first left half powered up reports
> `16 MB` of flash and `8 MB` of onboard PSRAM to esptool (ESP32-S3 rev v0.2,
> MAC `d0:cf:13:21:92:60`): the module actually fitted is an **N16R8**. The
> right half has not been read yet. PSRAM remains disabled on the firmware
> side (no `CONFIG_SPIRAM=y`).

U6 = left (sheet `s3`), U5 = right (sheet `right`). Right-side nets suffixed `_d`/`_D`.

## Matrix — ⚠ TWO distinct tables (routing permutations)

| row | LEFT (U6) | RIGHT (U5) |
|---|---|---|
| row0 | GPIO1 | GPIO2 |
| row1 | GPIO2 | GPIO12 |
| row2 | GPIO8 | GPIO4 |
| row3 | GPIO6 | GPIO5 |
| col0 | GPIO4 | GPIO6 |
| col1 | GPIO5 | GPIO7 |
| col2 | GPIO7 | GPIO8 |
| col3 | GPIO9 | GPIO9 |
| col4 | GPIO10 | GPIO11 |
| col5 | GPIO11 | GPIO10 |
| col6 | GPIO12 | GPIO1 |

- Electrical chain: **COL → switch → anode-diode-cathode → ROW** (1N4148W).
  → the scan DRIVES the columns and READS the rows.
- **Deep sleep wake-up: EXT1 on the ROWS** (all on RTC-capable GPIOs ✓).
- 100 Ω series resistor on each row on the MCU side; TVS SD05C on the switch side (transparent to firmware).
- 26 keys per half (rows of 7/7/6/6).

## Identifying the halves — MAC address

Both halves are programmed with the **same FTDI adapter**, moved from one to
the other, and the port stays `/dev/ttyUSB2` in both cases: nothing in
`idf.py flash` tells you which one it is writing to.

| Half | MAC (recorded 2026-09-07) |
|---|---|
| left (master) | `d0:cf:13:21:92:60` |
| right (scanner) | `80:b5:4e:eb:5e:08` |

On 2026-09-07, the master's firmware was written to the scanner: the right
half stopped scanning and started listening instead, and **nothing
complained** — the keyboard had silently lost a half. Flash via
`./scripts/flash-niphar.sh <left|right>`, which reads the MAC and refuses if
it does not match.

## Pins common to both MCUs

| Function | GPIO | Notes |
|---|---|---|
| VBAT_SENSE (gauge) | 13 | ADC2_CH2, 1M/1M divider + 100nF — **ADC2 forbidden while WiFi is active** (nRF24-only: OK). Full battery ≈ 4.15 V ÷ 2 |
| LCD_CS (`CS_DPL`) | 14 | Sharp display, **active HIGH** |
| nRF24 CE / CSN | 15 / 16 | |
| LINK_TX / LINK_RX (TRRS) | 17 / 18 | UART1. **Straight cable: TX lands on TX** → ONE half must swap TXD/RXD via the GPIO matrix. Never drive both TX lines without this swap |
| USB D− / D+ | 19 / 20 | native |
| LINK_5V_EN | 21 | ON of the SiP32431 (100k pull-down = 5 V off by default). Handshake: BOTH the sender AND the receiver must enable their switch to transfer 5 V; a half with a dead battery cannot be woken via the TRRS (assumed) |
| Shared SPI SCK / MISO / MOSI | 38 / 39 / 40 | nRF24 + display (write-only display) + **ESP32-P4 (Niphar_chest)** — see below |
| nRF24 IRQ | 41 | |
| TP_RDY | 42 | trackpad, left only (labels pending on the right) |
| I2C trackpad SDA / SCL | 47 / 48 | 4.7k pull-ups; left only. Trackpad NRST = hardware RC, no GPIO |
| Prog | 0, 43 (TX0), 44 (RX0) | 6-pin ESP-Prog-style connector per half (EN/3V3/TX/GND/RX/IO0) |
| Forbidden | 3, 45, 46, 35-37 | strapping / octal PSRAM — not wired |

## ⚠ The SPI bus carries a third participant: the ESP32-P4

> **Added on 2026-09-05, after an hour-long diagnosis.**

An **ESP32-P4** board (Niphar_chest project) is connected to the shared SPI
bus of the left half — SCK/MISO/MOSI on GPIO 38/39/40. This document did not
mention it anywhere, and its absence was costly: an **unprogrammed** P4 holds
these three lines, the nRF24 goes silent, and the diagnosis blames the radio
module, then a mechanical short, then the octal PSRAM in turn before finding
the actual cause.

Fault signature, should it recur: the **three shared lines** read
`PINNED LOW` in the line test (`CONFIG_KASE_NRF_PROBE`), while **CSN, CE and
IRQ remain free** — those three belong to the radio alone, the P4 does not
touch them. No bridge between them: each is pulled separately, by the same
component at the other end.

Design consequences, not addressed as of today:

- **Three slaves on one bus, each with its own CS.** The P4 absolutely must
  release MISO outside of selection. A slave that keeps MISO driven as an
  output holds the bus even while working perfectly.
- **Arbitration.** Nothing currently orders access between the nRF24, the
  display and the P4.
- The P4 must be flashed **before** any radio test on the left half.

## Peripherals

- **Displays (BOTH halves)**: nice!view-style module (Sharp LS011B7DH03) on
  J4 (right) and J12 (left, populated 2026-09-14), 5 pins:
  MOSI/SCK/3V3/GND/CS. Mounted UPRIGHT (portrait 68 × 160). CS active high, held
  LOW from boot and pulled low during sleep (pins isolated by light sleep); software
  VCOM toggling (EXTCOMIN handled by the module). Protocol settled from the
  datasheet (lemia docs 6844/6845): 68-line × 160 px panel, raw command word
  in MSB-first (M0 = first bit), row address in LSB-first (rev8).
  Driver: `main/display/memlcd/`.
- **Trackpad (left)**: Azoteq TPS43 (IQS572) over I2C + mandatory RDY (handshake).
- **nRF24L01+**: 2×4 breakout modules, 3.3 V supply, 100 Ω series resistor on the 6 signals.

## Power (per half)

- Li-ion 16340 → DW01A+FS8205 → switch (battery-only path) → **HT7833** (500 mA,
  4 µA IQ) → 3.3 V. USB 5 V → SS14 → same node (USB bypasses the switch).
- TP4056 charging ~500 mA; CHRG = LED; STDBY not wired → end-of-charge detected via ADC.
  **Gauge implemented on 2026-09-14** (`power/batt_sense`): calibrated ADC2_CH2,
  8 readings / 10 s + on wake, 4.10 V read against 4.08 V on the multimeter.
  "End-of-charge via ADC" = plateau ≥ 4.15 V held for 2 min (inferred, for lack
  of STDBY/VBUS).
- AO3407 load-sharing: USB present = battery isolated.
- CH334R hub (left): crystal-less mode, powered from 5 V USB only —
  **it does not exist on battery** (and port 4 is not wired).

## Firmware: non-negotiable requirements

1. **Swap TX/RX** on one half before any TRRS UART.
2. **Watchdog** + brownout recovery: clean reset ≤ 200 ms (legacy: the v1s
   crashed on ESD — the hardware is hardened, the firmware must finish the job).
3. Sleep budget: target < 50 µA per half, radio in power-down, RTC scan.
4. Two pin profiles (tables above) selected at compile time or by
   detection (e.g. trackpad presence on the I2C bus = left half).

---

## KeSp_firmware integration (to do)

Create the board definitions `boards/niphargus_half_left` and `boards/niphargus_half_right`
on the model of `kase_half_left`/`kase_half_right`, with the two pin tables above.
Notable differences vs KaSe: 4×7 matrix (26 keys) in the RTC domain with EXT1 wake-up
on the rows, nRF24L01+ radio on SPI shared with the display (display CS GPIO14 active high),
TRRS wired link UART1 (TX/RX swap on one half), ADC2_CH2 gauge.
