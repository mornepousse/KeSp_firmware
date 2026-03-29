# KeSp — Keyboard ESP32 Framework

Open-source keyboard firmware framework for ESP32-S3, designed for custom mechanical keyboards with display, USB HID, Bluetooth HID, advanced keycodes, and a virtual pet.

> KeSp provides the framework. Your board definition provides the hardware specifics.

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

### Statistics & Pet
- **Key statistics** — Per-key press counts and bigram tracking, auto-saved to NVS
- **Tamagotchi** — Virtual pet on both displays, driven by keyboard usage
  - 20 evolution stages (OpenCritter sprites, MIT)
  - Stats: hunger, happiness, energy, health
  - Keyboard interactions: feed, play, sleep, medicine
  - "Tama time" = keypresses (pauses when inactive, no penalty)

### CDC Serial Protocol
- **Pluggable command framework** — Generic CDC dispatch, easy to extend
- **Full configuration** — Keymaps, macros, tap dance, combos, leader, tama
- **Statistics** — Binary heatmap data + text debug output
- **Feature query** — `FEATURES?` command for software capability detection

---

## Project structure

```
boards/
  kase_v1/              # Round SPI display, LED strip, position mapping
  kase_v2/              # I2C OLED
  kase_v2_debug/        # V2 with debug GPIO overrides
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
      cdc_acm_com.c     # Generic CDC framework (reusable)
      cdc_keyboard_cmds.c  # KaSe-specific commands
      cdc_ota.c         # OTA firmware update
    ble/                # Bluetooth HID stack
    usb/                # USB HID (TinyUSB)
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
test/                   # Host-side unit tests (1008 tests)
docs/                   # Protocol documentation
scripts/                # Build automation, sprite conversion
```

---

## Quick start

### Build

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/).

```bash
source ~/esp/esp-idf/export.sh

# Build for a specific board
idf.py -DBOARD=kase_v2_debug build
idf.py -DBOARD=kase_v1 build

# Full flash (required for first flash or partition table change)
esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 write-flash \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/KeSp.bin \
  0x420000 build/storage.bin

# After first flash, use OTA via CDC (no programmer needed)
```

### Tests

```bash
cd test && mkdir -p build && cd build && cmake .. && make && ./test_runner
# 1008 tests, 0 failures
```

---

## Advanced keycodes

All keycodes are 16-bit, configurable via CDC serial or the remapping software.

| Feature | Keycode | Behavior |
|---------|---------|----------|
| Mod-Tap | `K_MT(mod, key)` | Hold = modifier, Tap = keycode |
| Layer-Tap | `K_LT(layer, key)` | Hold = layer, Tap = keycode |
| One-Shot Mod | `K_OSM(mod)` | Tap = next key gets modifier |
| One-Shot Layer | `K_OSL(layer)` | Next key uses that layer |
| Caps Word | `K_CAPS_WORD` | Auto-shift letters until space |
| Repeat | `K_REPEAT` | Repeat last keypress |
| Leader | `K_LEADER` | Start key sequence |
| Tap Dance | `K_TD(index)` | 1/2/3 taps + hold = 4 actions |
| Tama Feed | `K_TAMA_FEED` | Feed the virtual pet |

Full encoding spec: [`docs/KEYCODE_MAP.md`](docs/KEYCODE_MAP.md)

---

## CDC serial protocol

The keyboard exposes a USB CDC serial port for configuration. All commands are text-based (`\r\n` terminated).

```bash
# Query version
echo -en "VERSION?\r\n" > /dev/ttyACM0

# Query features
echo -en "FEATURES?\r\n" > /dev/ttyACM0
# → MT,LT,OSM,OSL,CAPS_WORD,REPEAT,TAP_DANCE,COMBO,LEADER

# Set a key to Mod-Tap(Shift, ESC)
echo -en "SETKEY 0,4,0,5229\r\n" > /dev/ttyACM0

# Create a macro: Ctrl+C, 100ms, Ctrl+V
echo -en "MACROSEQ 0;CopyPaste;06:01,FF:0A,19:01\r\n" > /dev/ttyACM0

# Query tama stats
echo -en "TAMA?\r\n" > /dev/ttyACM0
```

Full command reference: [`docs/CDC_COMMANDS_REFERENCE.md`](docs/CDC_COMMANDS_REFERENCE.md)

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
| [`docs/CDC_COMMANDS_REFERENCE.md`](docs/CDC_COMMANDS_REFERENCE.md) | Complete CDC command reference |
| [`docs/KEYCODE_MAP.md`](docs/KEYCODE_MAP.md) | Keycode encoding specification |
| [`docs/CDC_KEYSTATS_PROTOCOL.md`](docs/CDC_KEYSTATS_PROTOCOL.md) | Binary stats/bigrams/OTA protocol |
| [`docs/TAMA_SPRITE_GUIDE.md`](docs/TAMA_SPRITE_GUIDE.md) | Sprite design guide for artists |

---

## Related projects

- **KaSe PCB** — Hardware, schematics, mechanical design: https://github.com/mornepousse/KaSe_PCB
- **KaSe Soft** — Desktop configuration tool: https://github.com/mornepousse/KaSe_soft

---

## License

**GPL-3.0** — See [LICENSE](LICENSE).

Tamagotchi sprites from [OpenCritter](https://github.com/SuperMechaCow/OpenCritter) (MIT License).
