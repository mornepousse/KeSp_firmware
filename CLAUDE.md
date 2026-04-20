# KaSe firmware — Claude Code instructions

Firmware ESP32-S3 pour clavier split-ergo custom (KaSe V1/V2/V2D).
Distribué via binaires GitLab Releases. Build via ESP-IDF 5.5.

## Repo
- **Origin** : https://gitlab.com/harrael/KeSp_firmware
- **Local** : `~/Documents/GitHub/KaSe_firmware/`
- **Related** : https://gitlab.com/harrael/KeSp_controller (remapping software)

## Versioning

Source de vérité : tag git `vX.Y.Z`. Lu par ESP-IDF via `git describe --tags`
au build. Pas de fichier VERSION.

Pour cut une release :
1. `git commit` les changements
2. `git tag vX.Y.Z`
3. `git push && git push --tags`
4. Build les 3 boards + merge full binaries
5. `glab release create vX.Y.Z <files...>`

Entre deux releases : `cheni vX.Y.Z-N-gHASH-dirty` via `git describe`.

## Board variants

- **V1** : round SPI display (GC9A01), LED strip, pinout historique
- **V2** : OLED I2C (SSD1306), pinout production
- **V2D** : V2 + overrides GPIO pour prototype (COLS7/8 sur GPIO21/4 au lieu de UART0)

Chaque variant sous `boards/<name>/` avec `board.h`, `board_keymap.c`,
`board_layout.c`. V2D inherit de V2 via `#include "../kase_v2/board.h"`.

## Build system

```bash
source ~/esp/esp-idf/export.sh
idf.py -B build_v1  -DBOARD=kase_v1       build
idf.py -B build_v2  -DBOARD=kase_v2       build
idf.py -B build_v2d -DBOARD=kase_v2_debug build
```

Paramètre CMake : `-DBOARD=<name>` (pas `-DBOARD_VARIANT`). Chaque board
a son propre dossier build (`build_v1/`, `build_v2/`, `build_v2d/`).

**Important** : `sdkconfig` est partagé entre les 3 builds. Ne pas le
supprimer entre builds sinon `fullclean` régénère depuis `sdkconfig.defaults`
et perd les overrides manuels.

## Flash

**App only** (NVS préservée) :
```bash
idf.py -B build_v<N> -p /dev/ttyUSB0 flash
# ou: esptool.py write_flash 0x20000 build_v<N>/KeSp.bin
```

**Full flash** (erase + bootloader + partition table + app + storage) :
```bash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 erase_flash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x0 kase_<board>_full.bin
```

Requis après changement de partition table (ex: NVS resize).

## Partition table

`partitions.csv` — 16MB flash :
- `nvs`      : 0x9000  + 0x10000 (64KB) — config, keymaps, stats
- `otadata`  : 0x19000 + 0x2000
- `phy_init` : 0x1B000 + 0x1000
- `factory`  : 0x20000 + 0x200000 (2MB)
- `ota_0`    : 0x220000 + 0x200000 (2MB)
- `storage`  : 0x420000 + 0xF0000 (LittleFS)

NVS DOIT rester à 64KB — stocke ~21KB de bigrams + keymaps + macros + etc.
Ne pas réduire sans retirer les bigrams d'abord.

## Architecture

```
main/
├── main.c                # app_main, safe boot, task orchestration
├── comm/
│   ├── cdc/              # Binary protocol only (KS/KR frames, CRC-8)
│   │   ├── cdc_acm_com.c        # USB CDC dispatch
│   │   ├── cdc_binary_protocol.c # Frame parser, CRC
│   │   ├── cdc_binary_cmds.c    # All command handlers
│   │   └── cdc_ota.c            # OTA binary helpers
│   ├── ble/              # Bluetooth LE HID
│   │   └── hid_bluetooth_manager.c
│   ├── usb/              # USB HID + CDC TinyUSB init
│   │   └── usb_hid.c
│   └── hid_transport.c   # USB/BLE routing (usb_bl_state)
├── input/
│   ├── matrix_scan.c     # keyboard_button driver wrapper
│   ├── keyboard_task.c   # Main scan loop
│   ├── key_processor.c   # Keycode decoding, layers, advanced
│   ├── key_features.c    # OSM, OSL, caps_word, repeat, leader, etc.
│   ├── tap_hold.c tap_dance.c combo.c leader.c
│   ├── hid_report.c      # HID queue + sender task
│   └── keymap.c key_stats.c
├── display/
│   ├── display_backend.h # vtable for OLED/round
│   ├── status_display.c  # Coordinator
│   ├── oled/             # I2C OLED (V2/V2D)
│   └── round/            # SPI GC9A01 (V1)
├── tama/                 # Virtual pet (OpenCritter sprites)
└── led/                  # WS2812 strip anim (V1 only)

boards/
├── kase_v1/   kase_v2/   kase_v2_debug/
└── kase_layout.inc  # Layout JSON shared V2/V2D
```

## CDC protocol — binary only, no ASCII

Frame format KS (request) / KR (response) avec CRC-8 :
```
KS: [0x4B][0x53][cmd:u8][len:u16 LE][payload...][crc8]
KR: [0x4B][0x52][cmd:u8][status:u8][len:u16 LE][payload...][crc8]
```

Voir `docs/CDC_BINARY_PROTOCOL.md` pour la doc complète et
`main/comm/cdc/cdc_binary_protocol.h` pour les IDs (KS_CMD_*).

**Jamais d'ajout de commande ASCII** — le protocole texte a été retiré en v3.7.

## Keycodes (16-bit)

Encoding dans `main/input/key_definitions.h`. Ranges :
- `0x00-0xFF` : HID standard
- `0x0100-0x0A00` : MO(layer)
- `0x0B00-0x1400` : TO(layer)
- `0x1500-0x2800` : Macros
- `0x2900-0x2F00` : Bluetooth actions
- `0x3000-0x3DFF` : OSM, OSL, CapsWord, Repeat, Leader, Tama, GESC, etc.
- `0x4000-0x4FFF` : LT(layer, kc)
- `0x5000-0x5FFF` : MT(mod, kc)
- `0x6000-0x6FFF` : TD(index)
- `0x7000-0x7FFF` : LM(layer, mods)

## NVS — données persistées

Namespace : `"storage"` (défini `STORAGE_NAMESPACE`).
Clés :
- `keymaps`, `layout_names`
- `macros`
- `key_stats`, `key_stats_tot`, `bigram_stats`, `bigram_total`
- `td_configs`, `td_count`
- `combo_cfg`, `combo_cnt`
- `leader_cfg`, `leader_cnt`
- `ko_cfg`, `ko_cnt`
- `bt_slots`, `bt_active`, `bt_enabled`
- `tama_stats`, `tama_ver`

**Jamais** d'erase NVS au boot sans raison explicite (safe mode préserve
les données depuis v3.7.8).

## Safe boot

RTC memory tracking du nombre de boots consécutifs (`BOOT_CRASH_MAGIC`).
Si > 3 → `safe_mode = true` : skip display/BLE/NVS config loads. USB HID
basique reste fonctionnel. NVS NON effacée.

## Conventions C

- **Pas de `malloc` dans les hot paths** (scan, HID send, callbacks ISR).
  Buffers statiques ou pile.
- **`IRAM_ATTR`** pour les ISR / callbacks gptimer.
- **Pas de `ESP_LOGI`** dans les callbacks de scan matrice (trop lent).
  Utiliser `ESP_LOGD` avec niveau défini à NONE en prod.
- **Mutex LVGL** : tout accès LVGL doit être entouré de
  `lvgl_port_lock()` / `lvgl_port_unlock()`.
- **`lv_obj_is_valid()`** avant d'accéder à un objet LVGL après un
  potentiel `display_clear_screen()`.
- **NVS writes** via `nvs_save_blob_with_total()` pour éviter les
  corruptions si la struct change.

## Tests

Tests host-side dans `test/` (CMake standalone). Pas de tests embedded
sur le target. Exécution : `cd test/build && ./test_runner`.

Les tests doivent être parallel-safe : pas d'état global muté, pas de
chemins temp partagés. Mocks NVS via fake implementations dans le test.

## Dépendances ESP-IDF

Managed via `main/idf_component.yml` :
- `espressif/esp_tinyusb`
- `lvgl/lvgl: ^8`
- `espressif/esp_lvgl_port`
- `espressif/esp_lcd_gc9a01`
- `joltwallet/littlefs`
- `espressif/keyboard_button` (local dans `components/`)
- `espressif/led_strip`

Lock : `dependencies.lock` (tracké git). Pour mettre à jour :
```bash
rm dependencies.lock && rm -rf managed_components/
idf.py reconfigure
```

## Hardware specifics

**USB** : ESP32-S3 OTG Full-Speed only (12 Mbps, max packet 64 bytes).
Pas de High-Speed possible.

**Console UART désactivée** (`CONFIG_ESP_CONSOLE_NONE=y`) — libère
GPIO43/44/16 pour le scan matrice sur V2 (qui utilise UART0).

**GPIO reset** : `matrix_setup()` fait `gpio_reset_pin()` sur toutes les
pins matrice pour détacher les fonctions bootloader (UART0, SPI flash
secondaire).

## Release workflow

1. Bump version via tag git `vX.Y.Z`
2. Clean build 3 boards (`rm -rf build_<N>/`)
3. Merge binaries avec `esptool.py merge_bin` pour les `_full.bin`
4. `glab release create vX.Y.Z <6 files>` (app + full × 3)

Voir `docs/` pour protocoles et keycodes détaillés.
