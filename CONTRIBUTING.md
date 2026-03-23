# KeSp Firmware — Development Guide

## Build

Requires ESP-IDF v5.x. Source the environment before building:

```bash
source ~/esp/esp-idf/export.sh
```

Build for a specific board (default: kase_v2_debug):

```bash
idf.py -DBOARD=kase_v2_debug build
idf.py -DBOARD=kase_v2 build
idf.py -DBOARD=kase_v1 build
```

Build all variants for release:

```bash
./scripts/build_release.sh v3.3
```

## Tests

Host-side unit tests (no hardware needed):

```bash
cd test && mkdir -p build && cd build && cmake .. && make && ./test_runner
```

## Architecture

- **Board abstraction**: All hardware config in `boards/<name>/board.h` (compile-time macros, not runtime structs)
- **Entry point**: `main/main.c` — creates FreeRTOS tasks on two cores
  - Core 0: keyboard matrix scanning, USB HID, CDC, BLE
  - Core 1: display (LVGL), LED strip animations
- **Key flow**: matrix scan → ghost filter → keymap lookup → HID report → USB/BLE send
- **NVS persistence**: keymaps, key stats, bigrams, macros, BLE state, layout names
- **CDC protocol**: text commands over USB serial for configuration (see `docs/CDC_KEYSTATS_PROTOCOL.md`)

## Key conventions

- Use `ESP_LOGI/LOGE/LOGW` (never `printf`) for logging
- `STORAGE_NAMESPACE` is defined once in `keyboard_config.h`
- Board-specific values use `BOARD_` prefix macros
- Feature flags: `BOARD_HAS_LED_STRIP`, `BOARD_HAS_POSITION_MAP`
- Display backends: `BOARD_DISPLAY_BACKEND_ROUND` (SPI GC9A01) or `BOARD_DISPLAY_BACKEND_OLED` (I2C SSD1306)
- Comments in English
- All 3 board variants must compile after any change

## File layout

```
boards/             # Board definitions (board.h, board_keymap.c, board_layout.c)
main/input/         # Matrix scanning, keymaps, keyboard config
main/display/       # Display drivers, LVGL UI, LED strip, tamagotchi
main/comm/usb/      # USB HID via TinyUSB
main/comm/cdc/      # CDC ACM command protocol
main/comm/ble/      # Bluetooth HID
main/config/        # version.h (just includes board.h)
test/               # Host-side unit tests
scripts/            # Build automation
docs/               # Protocol documentation
```
