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

Flash (full flash required for OTA partition layout):

```bash
esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 write-flash \
  0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin 0x20000 build/KeSp.bin 0x420000 build/storage.bin
```

## Tests

Host-side unit tests (no hardware needed):

```bash
cd test && mkdir -p build && cd build && cmake .. && make && ./test_runner
```

## Architecture

```
main/
├── main.c                              — Entry point, FreeRTOS task creation
├── config/version.h                    — Firmware version
├── sys/
│   ├── cpu_time.c                      — CPU usage monitoring
│   └── nvs_utils.c/h                  — Generic NVS helpers (reusable)
├── input/
│   ├── keyboard_task.c/h              — Main coordinator (43 lines)
│   ├── key_processor.c/h             — Keycode building, layers, advanced features
│   ├── hid_report.c/h                — HID queue + sender task
│   ├── matrix_scan.c/h               — Physical key scanning
│   ├── key_stats.c/h                 — Keystroke stats + bigrams + NVS
│   ├── keymap.c/h                    — Keymap/macro NVS persistence
│   ├── keyboard_actions.c/h          — Async actions (BT, display)
│   ├── tap_hold.c/h                  — Tap/Hold engine (MT, LT, OSM)
│   ├── tap_dance.c/h                 — Tap Dance engine
│   ├── combo.c/h                     — Simultaneous key combos
│   ├── leader.c/h                    — Leader key sequences
│   ├── key_features.c/h             — OSM, OSL, Caps Word, Repeat Key
│   ├── keyboard_config.h             — Matrix/layer constants
│   └── key_definitions.h            — All keycode defines (shared with KaSe_soft)
├── comm/
│   ├── hid_transport.c/h            — USB/BLE routing (reusable)
│   ├── cdc/
│   │   ├── cdc_acm_com.c/h          — CDC core (binary protocol dispatch)
│   │   ├── cdc_binary_protocol.c/h  — KS/KR frame parser, CRC-8
│   │   ├── cdc_binary_cmds.c        — All command handlers
│   │   └── cdc_ota.c                — OTA via binary CDC
│   ├── usb/usb_hid.c                — USB HID
│   └── ble/                          — BLE HID stack
├── display/
│   ├── status_display.c/h           — Display coordinator (backend-agnostic)
│   ├── display_backend.h            — Backend interface (vtable)
│   ├── display_types.h              — Shared types
│   ├── oled/                         — I2C OLED backend (V2)
│   │   ├── i2c_oled_display.c/h
│   │   └── oled_backend.c
│   ├── round/                        — SPI round display backend (V1)
│   │   ├── spi_round_display.c/h
│   │   ├── round_ui.c/h
│   │   ├── round_backend.c
│   │   └── tamagotchi.c/h
│   └── assets/                       — Image assets
├── led/led_strip_anim.c/h           — WS2812 LED animations
└── app/                              — DFU, LittleFS managers
```

### Key flow

```
ISR (matrix_scan) → keyboard_task → key_processor → hid_report → USB/BLE
                                          ↓
                              tap_hold / tap_dance / combo / leader
```

### Core assignment

- **Core 0**: Keyboard scanning, HID sender, CDC processing
- **Core 1**: Display (LVGL), LED strip

### Display backend system

Each display type implements `display_backend_t` (vtable in `display_backend.h`).
To add a new display: create `display/new_type/new_backend.c` and register in `main.c`.

### CDC command system

Binary-only protocol (KS/KR frames with CRC-8). All commands in `cdc_binary_cmds.c`.
See `docs/CDC_BINARY_PROTOCOL.md` for frame format and command reference.

## Conventions

- `ESP_LOGI/LOGE/LOGW` for logging (never `printf`)
- `STORAGE_NAMESPACE` defined in `keyboard_config.h`
- Board macros use `BOARD_` prefix
- Keycode defines use `K_` prefix (not QMK `KC_`)
- `key_definitions.h` is shared with KaSe_soft — don't remove defines
- All 3 board variants must compile after any change
- Comments in English

## Advanced keycodes

See `docs/KEYCODE_MAP.md` for the full keycode encoding spec and CDC command reference.

| Feature | Keycode | CDC Command |
|---------|---------|------------|
| Mod-Tap | `K_MT(mod, key)` | `SETKEY` |
| Layer-Tap | `K_LT(layer, key)` | `SETKEY` |
| One-Shot Modifier | `K_OSM(mod)` | `SETKEY` |
| One-Shot Layer | `K_OSL(layer)` | `SETKEY` |
| Caps Word | `K_CAPS_WORD` | `SETKEY` |
| Repeat Key | `K_REPEAT` | `SETKEY` |
| Leader Key | `K_LEADER` | `LEADERSET` |
| Tap Dance | `K_TD(index)` | `TDSET` |
| Combos | — | `COMBOSET` |
| Macro Sequence | `MACRO_1-20` | `MACROSEQ` |
