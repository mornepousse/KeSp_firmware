# KeSp — Keyboard ESP32 Framework

Open-source keyboard firmware framework for ESP32-S3, designed for custom mechanical keyboards with display, USB HID, Bluetooth HID and multi-layer keymaps.

> KeSp provides the framework. Your board definition provides the hardware specifics.

---

## Features

- **Board abstraction** — All hardware config (GPIOs, display, LEDs, USB IDs, debounce, etc.) isolated in `boards/<name>/board.h`. Add a new keyboard by creating a new board directory.
- **Key matrix scanning** — Event-driven architecture with configurable debounce, anti-ghosting, and per-board scan timing.
- **Multi-layer keymaps** — Up to 10 layers with per-key mapping, persistent in NVS.
- **USB HID** — Keyboard + mouse composite device via TinyUSB.
- **Bluetooth HID** — BLE HID with configurable connection parameters.
- **CDC ACM** — USB serial port for configuration, key remapping, statistics, and macros.
- **Display support** — I2C OLED (SSD1306) and SPI round (GC9A01) via LVGL. Auto-sleep on inactivity.
- **WS2812 LED strip** — Animations (breathe, chase, reactive, KPM bar) with configurable frame rate.
- **Key statistics** — Per-key press counts and bigram tracking, auto-saved to NVS.
- **Macros** — Up to 20 configurable multi-key macros.
- **Tamagotchi** — Virtual pet on round display, reacts to typing activity.
- **Deep sleep** — Configurable inactivity timeout.

---

## Project structure

```
boards/
  kase_v1/          # KaSe V1: round SPI display, LED strip, position mapping
  kase_v2/          # KaSe V2: I2C OLED, no LEDs
  kase_v2_debug/    # KaSe V2 Debug: inherits V2, overrides 3 GPIOs
main/
  app/              # Filesystem, DFU
  input/            # Matrix, keyboard manager, keymaps
  display/          # Display drivers, LVGL UI, LED strip, tamagotchi
  comm/
    usb/            # USB HID (TinyUSB)
    cdc/            # CDC ACM commands
    ble/            # Bluetooth HID
  config/           # version.h (includes board.h)
  sys/              # CPU time monitoring
scripts/
  build_release.sh  # Multi-variant release builder
test/               # Host-side unit tests (844 tests)
docs/               # Protocol documentation
```

---

## Adding a new board

1. Create `boards/<your_board>/board.h` — define all hardware macros:
   - Product info (`GATTS_TAG`, `PRODUCT_NAME`, `MANUFACTURER_NAME`, etc.)
   - Matrix GPIOs (`ROWS0-4`, `COLS0-12`, `MATRIX_ROWS`, `MATRIX_COLS`)
   - Display config (`BOARD_DISPLAY_BACKEND_ROUND` or `BOARD_DISPLAY_BACKEND_OLED`, bus, pins, dimensions)
   - Feature flags (`BOARD_HAS_LED_STRIP`, `BOARD_HAS_POSITION_MAP`)
   - USB (`BOARD_USB_VID`, `BOARD_USB_PID`)
   - Timing (`BOARD_DEBOUNCE_TICKS`, `BOARD_MATRIX_SCAN_INTERVAL_US`, `BOARD_DISPLAY_SLEEP_MS`, `BOARD_SLEEP_MINS`)

2. Create `boards/<your_board>/board_keymap.c` — define `keymaps[]` and `default_layout_names[]`.

3. Build: `idf.py -DBOARD=<your_board> build`

See `boards/kase_v2/board.h` for a minimal example.

---

## Build

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/).

```bash
# Build for a specific board (default: kase_v2_debug)
idf.py -DBOARD=kase_v2 build

# Flash
idf.py -DBOARD=kase_v2 -p /dev/ttyUSB0 flash

# Monitor
idf.py -p /dev/ttyUSB0 monitor

# Run host tests
cd test && mkdir -p build && cd build && cmake .. && make && ./test_runner
```

### Release build (all variants)

```bash
./scripts/build_release.sh v3.3
# Output: release/KaSe_v3.3_V1.bin, KaSe_v3.3_V2.bin, KaSe_v3.3_V2_Debug.bin
```

---

## Configuration tool

The firmware communicates with [KaSe_soft](https://github.com/mornepousse/KaSe_soft) over USB CDC for key remapping, layer management, and macro configuration.

Protocol documentation: [`docs/CDC_KEYSTATS_PROTOCOL.md`](docs/CDC_KEYSTATS_PROTOCOL.md)

---

## Reference implementation: KaSe keyboard

KeSp was extracted from the [KaSe keyboard](https://github.com/mornepousse/KaSe_PCB) project. The `boards/kase_v1`, `kase_v2` and `kase_v2_debug` directories are the reference board definitions.

- **KaSe_PCB** — Hardware, schematics, mechanical design: https://github.com/mornepousse/KaSe_PCB
- **KaSe_soft** — Desktop configuration tool: https://github.com/mornepousse/KaSe_soft

---

## License

**GPL-3.0** — See [LICENSE](LICENSE).

Anyone can use, study, modify and redistribute. Modified versions must be published under the same license.
