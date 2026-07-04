# KeSp — Keyboard ESP32 Framework

Open-source firmware framework for ESP32-S3 custom mechanical keyboards — from
a single unibody board to a **wireless split** (two halves + a USB dongle),
with display, USB/Bluetooth HID, advanced QMK-like keycodes, a virtual pet, and
an optional **security co-processor** on the dongle.

> KeSp provides the framework. Your board definition provides the hardware specifics.

**Six board targets** share the codebase via `boards/<name>/` and per-board
Kconfig gates: `kase_v1` (round display), `kase_v2` (OLED), `kase_v2_debug`
(V2 + debug/wireless overrides), `kase_dongle` (USB receiver), `kase_half_left`
and `kase_half_right` (wireless e-ink halves).

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
- **Trackpad** — Cirque/IQS5xx pointing device on a half (gestures, accel curve)
- **E-ink status display** — SSD1681 monochrome dashboard on the halves (link/USB/battery)

### Wireless split & dongle
- **NRF24L01+ RF link** — Halves (PTX) → USB dongle (PRX), Enhanced ShockBurst
- **ESP-NOW side channel** — Pairing, config bridge (KS/KR tunnelled), e-ink status push
- **USB dongle** — Presents as a plain keyboard to the host; runs the key engine
  and relays HID; per-set addressing + pairing so multiple keyboards coexist
- **Wireless relay mode** — A full keyboard (e.g. V2D) can process locally and
  relay its final HID report to the dongle over RF
- **Half power management** — Heartbeat throttle + light-sleep with keypress wake

### Security co-processor (dongle, optional)
- **Compile-time personality** (Kconfig): `NONE` / OTP-HID (YubiKey-style CR-HMAC)
  / **OpenPGP smartcard** over USB CCID (gpg sign / decrypt / SSH-auth, touch-gated)
- Touch-gate confirm keycode, NVS-encryption + Secure-Boot V2 options
- *Currently frozen to `NONE`* — the OpenPGP surface compiles out; re-enable via Kconfig

### Statistics & Pet
- **Key statistics** — Per-key press counts and bigram tracking, auto-saved to NVS
- **Tamagotchi** — Virtual pet on both displays, driven by keyboard usage
  - 20 evolution stages (OpenCritter sprites, MIT)
  - Stats: hunger, happiness, energy, health
  - Keyboard interactions: feed, play, sleep, medicine
  - "Tama time" = keypresses (pauses when inactive, no penalty)

### CDC Serial Protocol
- **Binary-only protocol** — KS/KR frames with CRC-8, no ASCII fallback
- **Full configuration** — Keymaps, macros, tap dance, combos, leader, tama
- **Statistics** — Binary heatmap data + text format via binary frames
- **OTA firmware update** — Binary OTA over CDC with chunked transfer

---

## Project structure

```
boards/
  kase_v1/              # Round SPI display (GC9A01), LED strip
  kase_v2/              # I2C OLED (SSD1306)
  kase_v2_debug/        # V2 + debug/wireless GPIO overrides (V2D)
  kase_dongle/          # USB receiver (NRF24 RX), plain-keyboard to host
  kase_half_left/       # Wireless half: matrix TX + e-ink + trackpad
  kase_half_right/      # Wireless half (mirror)
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
    rf/                 # NRF24 driver, dongle RX / half TX, pairing, cfg bridge
    espnow/             # ESP-NOW pairing + info channel
  security/             # Dongle co-processor: SEC slots, OTP-HID, OpenPGP/CCID
  periph/               # E-ink (SSD1681), trackpad (IQS5xx)
  display/
    status_display.c    # Backend-agnostic coordinator
    display_backend.h   # Backend interface (vtable)
    oled/               # I2C OLED backend
    round/              # SPI round display + tamagotchi
  tama/
    tama_engine.c       # Game logic (display-agnostic)
    tama_render.c       # LVGL sprite renderer
    tama_sprites.h      # OpenCritter sprite data (MIT)
  led/                  # WS2812 LED strip animations
  sys/                  # NVS helpers, CPU monitoring
  config/               # Version
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

# Build all 6 boards + run host tests (anti-regression gate)
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
| Tama Feed | `K_TAMA_FEED` | Feed the virtual pet |

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
2. Create `boards/<name>/board_keymap.c` with default keymaps
3. Build: `idf.py -DBOARD=<name> build`

See `boards/kase_v2/board.h` for a minimal example, `CONTRIBUTING.md` for conventions.

---

## Documentation

| Document | Description |
|----------|-------------|
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Development guide, architecture, conventions |
| [`docs/CDC_BINARY_PROTOCOL.md`](docs/CDC_BINARY_PROTOCOL.md) | Binary protocol reference (all commands) |
| [`docs/KEYCODE_MAP.md`](docs/KEYCODE_MAP.md) | Keycode encoding specification |
| [`docs/CDC_KEYSTATS_PROTOCOL.md`](docs/CDC_KEYSTATS_PROTOCOL.md) | Stats/bigrams binary format details |
| [`docs/TAMA_SPRITE_GUIDE.md`](docs/TAMA_SPRITE_GUIDE.md) | Sprite design guide for artists |

---

## Related projects

- **KaSe PCB** — Hardware, schematics, mechanical design: https://github.com/mornepousse/KaSe_PCB
- **KeSp Controller** — Desktop remapping software: https://github.com/mornepousse/KeSp_controller

---

## License

**GPL-3.0** — See [LICENSE](LICENSE).

Tamagotchi sprites from [OpenCritter](https://github.com/SuperMechaCow/OpenCritter) (MIT License).
