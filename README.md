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
an Azoteq TPS43 trackpad on the left, a Sharp Memory LCD on the right.
Configuration and updates go over USB; there is no WiFi and no BLE on either
half — the power budget forbids it.

The halves talk over **nRF24 radio** (channel 0x4F), and the left half relays
finished HID reports to the dongle on its own channel. A wired TRRS link is the
planned alternative; its framing and 5 V handshake are written and host-tested,
but no cable has been received.

**Hardware status — 2026-09-08.** The keyboard works in its nominal mode: both
halves on battery, no cable anywhere, typing together through the dongle. Pin
tables were verified against the netlist on each half; the left runs the keymap
engine over all 14 columns and relays finished HID to the dongle by a
PRX→PTX→PRX excursion on its single radio.

Getting there took three bugs that were all the same bug. *Emit on change* and
*release on silence* are each reasonable, and they do not compose: whoever emits
only when something changes goes quiet while a key is merely **held**, and
whoever releases on silence then drops that key. The pattern bit at three links
in a row — right half to left, and left half to dongle — and each time the cure
was the same: silent at rest, refreshed while something is held. The constants
that bind an emitter to a listener's patience now live together in
`main/comm/rf/rf_slot.h`, because they are a contract between two firmwares
rather than a number each side picks alone.

Two things are still open. **The trackpad has no driver.** **Sleep is
unwritten** — and the measurement rules out the obvious route: light sleep costs
240 µA and the ULP coprocessor 170 µA (ESP32-S3 datasheet v2.2, table 5-10,
p. 68), so a sub-50 µA target admits only deep sleep with EXT1 wake, at the cost
of a full reboot on the first keypress.

Two decisions shape the whole codebase, and they are worth stating plainly
because both replaced an earlier design that is still visible in the git history.

**The left half is the master, in all circumstances.** It carries the only
keymap engine and the trackpad; the right half is a scanner that sends its
half-matrix over the wire. The left half has to run the engine anyway — it must
work over USB with no dongle in sight — so putting a second engine anywhere else
would mean two sources of truth for the same keystroke. That question came up
three times in two months; it is now closed by construction.

**The dongle is a repeater, not a brain.** It receives *finished HID reports*
and pushes them to the host. The input engine is not compiled for that role, the
keyboard half of the CDC protocol is not compiled either, and its `board.h` no
longer declares a matrix — because it has no switches. Keymap commands sent to a
dongle answer `KS_STATUS_ERR_UNKNOWN` rather than pretending to work: a silent
no-op would be worse than the missing feature.

Its two radio slots are no longer two halves of one keyboard. Slot 1 is the
keyboard, slot 2 is the
**[Conchodytes](https://github.com/mornepousse/Conchodytes)** mouse — two
unrelated devices, which is
why losing one slot releases only what that slot was holding. A mouse going out
of range must not wipe the keystroke in progress.

Removed along the way, and not coming back: the first-generation e-ink halves,
their ESP-NOW side channel, and the dongle's keymap engine.

| Document | |
|---|---|
| [`niphargus-firmware-design.md`](docs/superpowers/specs/2026-08-19-niphargus-firmware-design.md) | overall design |
| [`dongle-role-niphargus-design.md`](docs/superpowers/specs/2026-08-19-dongle-role-niphargus-design.md) | the dongle's role, and the RF budget behind it |
| [`docs/NIPHARGUS_V2_HARDWARE.md`](docs/NIPHARGUS_V2_HARDWARE.md) | pinout, verified against the netlist |

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
  niphar_left/          # Niphargus master: engine + trackpad (trackpad driver TODO)
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

1. Create `boards/<name>/board.h` with hardware macros (GPIOs, display, USB IDs, etc.)
2. Create `boards/<name>/board_keymap.c` and `board_layout.c` — **keyboard roles
   only.** A board with no switches must not declare a matrix to make the build
   pass; see `boards/kase_dongle/` for what a non-keyboard role looks like.
3. Build with an isolated sdkconfig:
   `idf.py -B build_<name> -DBOARD=<name> -DSDKCONFIG=build_<name>/sdkconfig build`

See `boards/kase_v2/board.h` for a minimal example, `CONTRIBUTING.md` for conventions.

---

## Documentation

| Document | Description |
|----------|-------------|
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Development guide, architecture, conventions |
| [`docs/CDC_BINARY_PROTOCOL.md`](docs/CDC_BINARY_PROTOCOL.md) | Binary protocol reference (all commands) |
| [`docs/KEYCODE_MAP.md`](docs/KEYCODE_MAP.md) | Keycode encoding specification |
| [`docs/CDC_KEYSTATS_PROTOCOL.md`](docs/CDC_KEYSTATS_PROTOCOL.md) | Stats/bigrams binary format details |
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

