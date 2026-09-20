# KeSp — Keyboard ESP32 Framework

Open-source firmware framework for ESP32-S3 custom mechanical keyboards —
unibody boards plus a **USB receiver dongle**, with display, USB/Bluetooth HID,
advanced QMK-like keycodes, and an optional **security co-processor** on the
dongle.

> KeSp provides the framework. Your board definition provides the hardware specifics.

**Seven board targets** share the codebase via `boards/<name>/` and per-board
Kconfig gates: `kase_v1` (round display), `kase_v2` (OLED), `kase_v2_debug`
(V2 + debug overrides), `kase_dongle` (USB receiver), `conchodytes` (a PMW3389
mouse on the dongle's second radio slot), and `niphar_left` / `niphar_right`
(the split keyboard — see below).

---

## Work in progress — Niphargus, and what it changed

The split keyboard is being redesigned as
**[Niphargus](https://github.com/mornepousse/Niphargus)**: two ESP32-S3 halves,
an Azoteq TPS43 trackpad on the left, a Sharp Memory LCD on **each** half.
Configuration and updates go over USB; there is no WiFi and no BLE on either
half — the power budget forbids it.

**Hardware status — 2026-09-19.** The keyboard works in its nominal mode: both
halves on battery, no cable anywhere, typing together through the dongle. Pin
tables were verified against the netlist on each half; symbol keys (`!@#$…`)
are one press each (Modified Keys, 0x8000 range), and the physical layout the
remapper draws is generated from the PCB. A wired **TRRS link** also works
(UART transport, probe/ACK handshake, 5 V load switch); charging the far half
through it is still an electrical question, not a firmware one.

**Dongle-side fusion landed (2026-09-13).** Both halves transmit their *raw*
half-matrix to the dongle, which fuses them and runs the keymap engine — so
**neither half listens on battery**, and a sleeping half no longer needs the
other one to wake it. The left keeps working as a standalone USB keyboard with
its own engine; the two engines are kept from diverging by construction: same
code, exactly one engine active at a time by route, and one configuration.
That last point is handled without ever plugging the left in: the dongle carries
the keymap fingerprint check (`KS_CMD_CONFIG_COHERENCE`) and, on divergence,
**streams its keymap to the left inside the nRF24 ACK payloads** (40 chunks of
28 bytes, pulled by the left, done in a few seconds of typing). Should the
dongle vanish, the right half falls back to the left over the direct link after
eight unacknowledged frames, and comes back the same way.

Getting there took three bugs that were all the same bug. *Emit on change* and
*release on silence* are each reasonable, and they do not compose: whoever emits
only when something changes goes quiet while a key is merely **held**, and
whoever releases on silence then drops that key. The cure was the same at every
link: silent at rest, refreshed while something is held. And ESB has a second
trap — about 1 % of frames are refused outright; a change frame that is sent
exactly once has exactly one chance, so every change is now re-emitted a
bounded number of times.

**Screens (2026-09-14).** Both halves drive a Sharp LS011B7DH03 (nice!view
module) mounted upright, 68 × 160, on the SPI bus they share with the radio —
the radio owner lends the bus under a lock, so a refresh never lands in the
middle of a frame. The left shows the current layer name split in four-letter
lines; the right shows the Niphargus logo, generated from the project SVG by
`scripts/gen_logo_memlcd.sh`. Both show route, dongle-seen and their own
battery. The protocol was settled from the datasheet, not by trial: the panel is
**68 lines of 160 pixels** (Sharp's "160 × 68" lists the data direction first),
the command byte goes out raw in MSB-first SPI (M0 is the first clocked bit)
and only the line address is bit-reversed. A write-only panel answers a wrong
guess with silence, never with an error.

**Battery gauge (2026-09-14).** Each half reads its cell on ADC2 (1 M / 1 M
divider, calibrated, plausibility window 2.5–4.5 V); the right reports every
30 s inside its STATUS frame, the dongle caches both halves per side
(`KS_CMD_BATTERY`). The displayed voltage settles for 30 s before changing, so
ADC jitter does not redraw the panel and a slow overnight drift still shows.
Since 2026-09-19 the gauge has two thresholds with hysteresis: **low** (< 3.5 V)
thickens the gauge border and stops the half from offering 5 V over the TRRS
link; **critical** (< 3.3 V) also pulls light sleep down to 5 s. No blinking,
no forced shutdown — the DW01A does that at 2.5 V.

**Sleep** is a hybrid: light sleep after 15 s (~244 µA, state kept, ~1 ms
wake — it was a minute until 2026-09-15, but an idle ESP32-S3 at 160 MHz draws
~28 mA, a hundred times its sleep current, and a day of typing with pauses lost
0.2 V that way), deep sleep beyond four hours (~12 µA, EXT1 wake, a full reboot before
the matrix is scanned again). Deep sleep was **unreachable until 2026-09-15**:
inactivity was only measured while awake, and a light-sleeping half sits in
`esp_light_sleep_start()` until a key — which resets the counter. A timer wake
at the deep threshold now performs the switch. Every wake logs how long the
half actually slept, and both halves carry a heartbeat with `idle=` and
`slept=X s/n`, because a night that loses 0.2 V (~20 mA) and a night at 244 µA
looked identical without that number. The radio is off from the light tier
onward — listening costs 13.1 mA and the nRF24 has no low-power listening mode.

**Idle power (2026-09-16).** The battery was not draining in sleep but
*awake and idle*: 27.6 mA of cores doing nothing at 160 MHz. Both halves now
run dynamic frequency scaling (160 MHz under load, 40 MHz idle, PLL off), stop
scanning the matrix at rest (columns held high, a row-level interrupt restarts
the scan on the first press, first scan under a millisecond), slow every
periodic task to 100 ms at rest (10–20 ms while a key is held, notified on
change), keep no typing statistics at all on the halves (`KASE_KEY_STATS=n`),
leave the left's local keymap engine dormant off USB, and finally **sleep
between keystrokes**: tickless idle plus ESP-IDF's automatic light sleep,
about nine naps a second at rest. The last one only worked once the keyboard
task stopped waking every 10 ms — one free tick at 100 Hz, where the sleeper
needs three; the profiler showed 92 % "idle" time and zero actual sleeps.
Cold USB plug still enumerates under all of this (a "USB does not switch"
scare turned out to be a charge-only cable). One regression from the slower
idle cadences was caught the same day: with the left on USB, its radio tick is
what drains the nRF24 receive FIFO of the right half's re-emitted frames, and
at 100 ms a short press and its release landed in the same tick — the right
half lost keys in USB mode. That tick now stays at 10 ms while the left is
listening (`kbd_relay_cadence_ms`, host-tested). Multimeter figures per half
are the next step.

**Power policy given a home (2026-09-19).** The two regressions above had a
common cause: energy policy was spread across six files, each task picking its
own cadence and its own reasons to stay awake. A one-day restructuring fixed
that. Fusion is now the default configuration of all three boards (the pre-push
check had been guarding a configuration that was no longer flashed); the
pre-fusion "left listens to the right" path is gone (−430 lines). Every cadence
lives in `power/cadence.h`, and each rest cadence carries a `_Static_assert`
against the tickless rule — a 10 ms rest loop no longer compiles. Sleep is
**one task** (`power/veille_task.c`), identical on both halves: modules that
need the board awake post a named **veto** (usb, TRRS link, keymap sync, test
mode), modules that own something to put to sleep register a **hook** (radio,
screen, gauge), and the heartbeat says why the board is up: `HB … vetos=usb+link`.
The right half's screen task went from 100 ms to 1 s, the TRRS link task
blocks on the UART event queue instead of polling. All of it proven on both
halves: wake with the key captured, 100 % ACK, link up and down, screens intact.

**The radio has one owner (2026-09-19).** The three "wrong channel, in silence"
incidents and the receive FIFO flushed on the way back from an excursion were
all ownership bugs: two modules each writing the chip's configuration. Each
half's nRF24 now belongs to `comm/rf/radio_owner.c` alone — one mode at a time
(PTX to a target, PRX listening to a target, off), one lock around every
transaction, an excursion that drains the FIFO into the consumer *before*
leaving, a wake that re-arms the mode. The two halves' modules became thin
policies that never touch `rf_driver` or a mutex. The owner talks to the
hardware through a small operations table, so the invariants are host-tested
against a fake recorder: the sequence of hardware calls is the oracle. The
refactor immediately paid for itself by exposing a listen address that had
only ever been right by accident.

**The dongle engine replays every transition (2026-09-19).** It used to play
only the *current* fused state each 10 ms cycle, so a press and release
landing in the same cycle melted into nothing; a counter added on the 15th
said 536 such overwrites in one evening. The engine now queues each received
state (eight deep) and plays them in order — the same minute of fast
two-handed typing afterwards: 699 frames, zero overwrites. That was the last
behaviour in the contract still marked unguarded.

**The trackpad still has no hardware driver.** Its pure logic — the IQS5xx
frame parser, the gesture→HID mapping, the accel config — exists and is
host-tested; what is missing is the I2C + RDY bring-up on the left half and
wiring its output through the mouse-relay path.

**Open: a lost first keystroke on the left.** After a pause of a couple of
minutes, a light first press on the left half sometimes produces nothing —
no wake, no capture, no driver event — while a firm press or the second press
works; the right half behaves better. Light-sleep wake, sleep-path double
entry (a real bug, removed), row voltage under press, NVS writes and dongle
coalescing have all been instrumented and cleared; the dongle counts
overwritten transitions and USB refusals (`KS_CMD_RF_STATUS[27..42]`) and
they read zero in the failing trials. Latest clue (2026-09-19): a wake fired
by row 2 with all rows already low 13 ms later, key never seen within 156 ms,
then the next press on the same row captured normally — the first contact
lasted under 13 ms. Points at the switch's first contact, not the firmware.

Removed along the way, and not coming back: the first-generation e-ink halves,
their ESP-NOW side channel, and the "left is the only engine" doctrine that
preceded fusion.

| Document | |
|---|---|
| [`niphargus-firmware-design.md`](docs/superpowers/specs/2026-08-19-niphargus-firmware-design.md) | overall design |
| [`dongle-fusion-deux-moteurs-design.md`](docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md) | fusion: two engines, one active by route |
| [`keymap-sync-ack-payload-design.md`](docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md) | keymap sync over the ACK payloads |
| [`batterie-jauge-design.md`](docs/superpowers/specs/2026-09-14-batterie-jauge-design.md) | the battery gauge |
| [`ecrans-memlcd-design.md`](docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md) | the screens |
| [`docs/NIPHARGUS_V2_HARDWARE.md`](docs/NIPHARGUS_V2_HARDWARE.md) | pinout, verified against the netlist |
| [`COMPORTEMENTS.md`](COMPORTEMENTS.md) | the behaviour contract: what the firmware must do, and what guards each line |

---

## Features

### Keyboard
- **Multi-layer keymaps** — Up to 10 layers with per-key mapping, persistent in NVS
- **Mod-Tap / Layer-Tap** — Hold for modifier/layer, tap for keycode
- **One-Shot Modifier/Layer** — Apply modifier or layer for next key only
- **Tap Dance** — Multiple taps on same key trigger different actions (16 slots)
- **Combos** — Press two keys simultaneously for a different keycode (16 slots)
- **Leader Key** — Key sequences that trigger actions with modifiers (16 entries)
- **Caps Word** — Auto-shift letters, deactivate on space/punctuation
- **Repeat Key** — Repeat the last keypress
- **Macros** — Up to 20 macros with sequences, modifiers, and delays

### Hardware
- **Board abstraction** — All hardware config in `boards/<name>/board.h`
- **Key matrix scanning** — Event-driven with configurable debounce
- **USB HID** — Keyboard + mouse composite device via TinyUSB
- **Bluetooth HID** — BLE HID with automatic fallback to USB
- **Display support** — I2C OLED (SSD1306) and SPI round (GC9A01) via LVGL
- **Display backend abstraction** — Add new display types with one file
- **WS2812 LED strip** — Reactive animations (breathe, chase, KPM bar)
- **OTA firmware update** — Flash new firmware over USB CDC, no programmer needed
- **Deep sleep** — Configurable inactivity timeout
- **Trackpad** — gesture/acceleration mapping (pure logic, host-tested). The
  Azoteq TPS43 driver lands with the Niphargus left half; the older IQS5xx
  path went out with the first-generation halves.

### Wireless split & dongle
- **NRF24L01+ RF link** — keyboard (PTX) → USB dongle (PRX), Enhanced ShockBurst,
  carrying *finished HID reports* rather than raw matrix state
- **USB dongle** — presents as a plain keyboard to the host and repeats what it
  receives; two slots (keyboard, mouse) with per-set addressing and pairing, so
  several sets coexist in the same room
- **Link supervision** — a 4-byte idle status frame (battery, link quality),
  sent once a second whenever nothing else has gone out. It exists for one
  reason: a receiver cannot tell *"not typing"* from *"dead"* if both look like
  silence. It was specified, decoded by the dongle, and **never emitted** until
  2026-09-08 — so a held key went quiet and the dongle released it after 2.5 s
- **Fail-safe on link loss** — release what that slot was holding, and only that
- **Inter-half wire link** — length-prefixed frames with CRC-8 over TRRS, plus a
  two-sided handshake before either half enables 5 V on the connector
- **Wireless relay mode** — a full keyboard (e.g. V2D) can process locally and
  relay its final HID report to the dongle over RF

### Security co-processor (dongle, optional)
- **Compile-time personality** (Kconfig): `NONE` / OTP-HID (YubiKey-style CR-HMAC)
  / **OpenPGP smartcard** over USB CCID (gpg sign / decrypt / SSH-auth, touch-gated)
- Touch-gate confirm keycode, NVS-encryption + Secure-Boot V2 options
- *Currently frozen to `NONE`* — the OpenPGP surface compiles out; re-enable via Kconfig

### Statistics
- **Key statistics** — Per-key press counts, auto-saved to NVS
- **Bigram tracking** — Counted in RAM and readable over CDC. Not persisted since
  v4.1.0: the NVS write ran from the display task and stalled the instruction
  cache mid-typing.

### CDC Serial Protocol
- **Binary-only protocol** — KS/KR frames with CRC-8, no ASCII fallback
- **Full configuration** — Keymaps, macros, tap dance, combos, leader
- **Statistics** — Binary heatmap data + text format via binary frames
- **OTA firmware update** — Binary OTA over CDC with chunked transfer

---

## Project structure

```
boards/
  kase_v1/              # Round SPI display (GC9A01), LED strip
  kase_v2/              # I2C OLED (SSD1306)
  kase_v2_debug/        # V2 + debug/wireless GPIO overrides (V2D)
  kase_dongle/          # USB receiver — no matrix, no keymap, no engine
  niphar_left/          # Niphargus master: engine, relay, sleep (trackpad HW driver TODO)
  niphar_right/         # Niphargus scanner: matrix + Sharp LCD (LCD driver TODO)
main/
  input/                # Matrix scan, key processing, HID reports
    keyboard_task.c     # Main coordinator (ISR → process → send)
    key_processor.c     # Keycode building, layers, advanced features
    hid_report.c        # HID queue + sender task
    matrix_scan.c       # Physical key scanning
    key_stats.c         # Keystroke stats + bigrams
    tap_hold.c          # Tap/Hold engine (MT, LT, OSM)
    tap_dance.c         # Tap Dance engine
    combo.c             # Simultaneous key combos
    leader.c            # Leader key sequences
    key_features.c      # OSM, OSL, Caps Word, Repeat Key
  comm/
    hid_transport.c     # USB/BLE routing abstraction
    cdc/
      cdc_acm_com.c     # CDC core (binary protocol dispatch)
      cdc_binary_cmds.c # All command handlers (KS/KR protocol)
      cdc_binary_protocol.c # Frame parser, CRC-8, response helpers
      cdc_ota.c         # OTA firmware update (binary only)
    ble/                # Bluetooth HID stack
    usb/                # USB HID (TinyUSB)
    rf/                 # NRF24 driver, dongle RX / keyboard TX, slots, pairing
    link/               # Niphargus inter-half wire link (frames + 5 V handshake)
  security/             # Dongle co-processor: SEC slots, OTP-HID, OpenPGP/CCID
  periph/               # Trackpad gesture/acceleration mapping
  display/
    status_display.c    # Backend-agnostic coordinator
    display_backend.h   # Backend interface (vtable)
    oled/               # I2C OLED backend
    round/              # SPI round display backend
    memlcd/             # Sharp memory-LCD backend (Niphargus halves, 68×160 portrait)
    assets/             # LVGL images (Niphargus logo generated by scripts/gen_logo_memlcd.sh)
  power/                # Sleep tiers (veille.c) and battery gauge (batt_sense.c)
  led/                  # WS2812 LED strip animations
  sys/                  # NVS helpers, CPU monitoring
test/                   # Host-side unit tests (CMake, link real modules)
docs/                   # Protocol documentation
scripts/                # Build automation, sprite conversion
```

---

## Quick start

### Build

Requires [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/).

```bash
source ~/esp/esp-idf/export.sh

# Per-board build — each board keeps its OWN isolated sdkconfig
# (prevents config leaking between boards)
idf.py -B build_kase_v2_debug -DBOARD=kase_v2_debug \
       -DSDKCONFIG=build_kase_v2_debug/sdkconfig build
idf.py -B build_kase_dongle   -DBOARD=kase_dongle \
       -DSDKCONFIG=build_kase_dongle/sdkconfig   build

# Build all 7 boards + run host tests (anti-regression gate)
./scripts/check.sh

# App-only flash — preserves NVS (keymaps/macros/stats)
idf.py -B build_kase_v2_debug -p /dev/ttyUSB0 app-flash

# Full flash (first flash or partition-table change) — esptool merge_bin
# offsets from build_<board>/flash_args. After first flash: OTA over USB CDC.
```

### Tests

Host-side unit tests (no hardware needed). Tests link the real firmware modules
and are gated by a test-count ratchet + bite-proof discipline.

```bash
./scripts/check.sh --host-only     # host tests only (~seconds)
# or manually:
cmake -S test -B test/build && cmake --build test/build && ./test/build/test_runner
```

---

## Advanced keycodes

All keycodes are 16-bit, configurable via CDC serial or the remapping software.

| Feature | Keycode | Behavior |
|---------|---------|----------|
| Mod-Tap | `K_MT(mod, key)` | Hold = modifier, Tap = keycode |
| Layer-Tap | `K_LT(layer, key)` | Hold = layer, Tap = keycode |
| Layer-Mod | `K_LM(layer, mods)` | Hold = layer + modifiers |
| One-Shot Mod | `K_OSM(mod)` | Tap = next key gets modifier |
| One-Shot Layer | `K_OSL(layer)` | Next key uses that layer |
| Caps Word | `K_CAPS_WORD` | Auto-shift letters until space |
| Repeat | `K_REPEAT` | Repeat last keypress |
| Leader | `K_LEADER` | Start key sequence |
| Tap Dance | `K_TD(index)` | 1/2/3 taps + hold = 4 actions |

Full encoding spec: [`docs/KEYCODE_MAP.md`](docs/KEYCODE_MAP.md)

---

## CDC binary protocol

The keyboard exposes a USB CDC serial port for configuration using a binary frame protocol (KS/KR).

```
Request:  [0x4B][0x53][cmd:u8][len:u16 LE][payload...][crc8]
Response: [0x4B][0x52][cmd:u8][status:u8][len:u16 LE][payload...][crc8]
```

```python
import serial, struct

def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

ser = serial.Serial("/dev/ttyACM0", timeout=2)

# Ping
ser.write(bytes([0x4B, 0x53, 0x04, 0, 0, 0]))

# Get version (cmd 0x01)
ser.write(bytes([0x4B, 0x53, 0x01, 0, 0, 0]))
```

Full protocol reference: [`docs/CDC_BINARY_PROTOCOL.md`](docs/CDC_BINARY_PROTOCOL.md)

---

## Adding a new board

A board is one folder. `scripts/new-board.sh <name>` creates it from
`boards/_template/` (pins, matrix tables, `BOARD_PINS(X)` checked by the host
contract test, keymap, layout, `sdkconfig.defaults`); `check.sh` and the CI
discover it from there, `-DBOARD_DIR=` builds one kept outside the repository.
Walkthrough from bare hardware to a remapped keyboard: **`docs/MAKE_YOUR_OWN.md`**;
file-by-file reference: `boards/README.md`.

---

## Documentation

| Document | Description |
|----------|-------------|
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Development guide, architecture, conventions |
| [`docs/CDC_BINARY_PROTOCOL.md`](docs/CDC_BINARY_PROTOCOL.md) | Binary protocol reference (all commands) |
| [`docs/KEYCODE_MAP.md`](docs/KEYCODE_MAP.md) | Keycode encoding specification |
| [`docs/CDC_KEYSTATS_PROTOCOL.md`](docs/CDC_KEYSTATS_PROTOCOL.md) | Stats/bigrams binary format details |
| [`docs/MAKE_YOUR_OWN.md`](docs/MAKE_YOUR_OWN.md) | From bare hardware to a remapped keyboard: board folder, flash, pairing, diagnostics |
| [`boards/README.md`](boards/README.md) | A board is one folder — the files, the pin tables and the contract |
| [`docs/NIPHARGUS_V2_HARDWARE.md`](docs/NIPHARGUS_V2_HARDWARE.md) | Niphargus pinout — verified against the netlist |
| [`docs/HARDWARE_SMOKE_TEST.md`](docs/HARDWARE_SMOKE_TEST.md) | Bench checklist to run before a merge or release |

---

## Related projects

The firmware lives here; each keyboard's schematics, PCB and case live in their
own repository.

| Repository | What it is |
|---|---|
| [KaSe PCB](https://github.com/mornepousse/KaSe_PCB) | Hardware for the unibody boards — the KiCad project behind `kase_v1` / `kase_v2` / `kase_v2_debug` |
| [Niphargus](https://github.com/mornepousse/Niphargus) | Hardware for the split keyboard — the boards `niphar_left` / `niphar_right` are written for |
| [Conchodytes](https://github.com/mornepousse/Conchodytes) | The wireless mouse that shares this dongle — slot 2 of the RF link |
| [KeSp Controller](https://github.com/mornepousse/KeSp_controller) | Desktop remapping software, speaking the CDC binary protocol |

This repository is developed on [GitLab](https://gitlab.com/harrael/KeSp_firmware)
and mirrored to [GitHub](https://github.com/mornepousse/KeSp_firmware) — commits,
tags and releases alike.

---

## License

**GPL-3.0** — See [LICENSE](LICENSE).

