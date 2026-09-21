# Make your own keyboard with this firmware

From a bare ESP32-S3 board to a typing keyboard, remappable over USB, with an
optional wireless split and dongle. Nothing here needs Nix or the author's
bench: plain ESP-IDF 5.5 and a USB cable.

> **The short way:** use the template repository
> **https://github.com/mornepousse/kesp-keyboard-template** — *Use this
> template*, fill `boards/<name>/board.h`, push: GitHub builds your firmware
> (this repository is a pinned submodule there, its reusable workflow
> `build-board.yml` does the work), and a `v1.0.0` tag makes a release of
> *your* repository. The rest of this document is what the template does and
> why, and how to do it by hand.

## 1. Hardware you need

| Part | Notes |
|---|---|
| **ESP32-S3** module (16 MB flash, PSRAM optional) | The firmware targets `esp32s3` only. Native USB (GPIO19/20) is the keyboard's USB port. |
| **Key matrix** with diodes | Wire **COL → switch → diode → ROW**: the scan drives the columns and reads the rows (`BOARD_MATRIX_COL2ROW`). This direction is what lets sleep hold the columns high and wake on *any* row without an ULP. Avoid strapping pins (0, 3, 45, 46) and, if your module has octal PSRAM, GPIO35-37. |
| nRF24L01+ (optional) | For a wireless keyboard or a split: SPI + CE + CSN + IRQ. The dongle carries two (keyboard slot 1, mouse slot 2). |
| Sharp LS011B7DH03 memory-LCD (optional) | Portrait 68 × 160, shares the nRF24 SPI bus, one active-high CS. OLED SSD1306 (I2C) and the round GC9A01 are the other supported backends. |
| Battery (optional) | 1M/1M divider on an ADC pin for the gauge; the protection IC decides the cut-off, the firmware only reports levels. |
| TRRS link between halves (optional) | UART1 + a 5 V load switch: the half on USB can power the other. |

The Niphargus (`boards/niphar_left`, `boards/niphar_right`) is the reference
split: `docs/NIPHARGUS_V2_HARDWARE.md` is its netlist-checked pinout, a good
place to copy from.

## 2. Toolchain

- ESP-IDF **v5.5** (`git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf`, `./install.sh esp32s3`, `. ./export.sh`).
- `export IDF_COMPONENT_CHECK_NEW_VERSION=0` — the lock file (`dependencies.lock`) is authoritative; a registry check breaks the configure step with the current component manager (see `CLAUDE.md`, *Component manager*).
- `python3 -m pip install pyserial` for the bench scripts.

## 3. Your board: one folder

```bash
scripts/new-board.sh my_keyboard              # boards/my_keyboard/ from boards/_template/
#   or, to keep it in your own repository:
scripts/new-board.sh my_keyboard /path/to/my_repo
```

Then edit, in this order:

1. **`board.h`** — the GPIO numbers (`ROWSn`, `COLSn`), `MATRIX_ROWS/COLS`, the
   two tables the core reads (`BOARD_ROW_PINS`, `BOARD_COL_PINS`), and
   **`BOARD_PINS(X)`**: every GPIO the board uses. Keep only the optional
   blocks you have (radio, battery, link, screen). USB VID/PID and product
   strings are at the top.
2. **`sdkconfig.defaults`** — switch on the features the hardware has
   (`CONFIG_KASE_KBD_WIRELESS`, `CONFIG_KASE_VEILLE`, `CONFIG_KASE_BATT_SENSE`,
   `CONFIG_KASE_DISPLAY_MEMLCD`…). A board without a screen keeps
   `CONFIG_KASE_NO_DISPLAY=y`.
3. **`board_keymap.c`** — the compile-time default keymap (one layer is
   enough; the live keymap is edited from the host, §6).
4. **`board_layout.c`** — the physical layout the remapping software draws.

```bash
./scripts/check.sh --fast        # host tests, including your board's pin contract
idf.py -B build_my_keyboard -DBOARD=my_keyboard -DSDKCONFIG=build_my_keyboard/sdkconfig build
#   out of tree:  -DBOARD_DIR=/path/to/my_repo/my_keyboard  instead of -DBOARD=
```

The pin contract (`test/board_contract.inc`) refuses a GPIO used twice, a
strapping pin, a table that does not match the geometry. `boards/README.md`
has the details of every file.

## 4. Flash

First time (empty flash) — the full image, at 0x0:

```bash
cd build_my_keyboard && esptool.py --chip esp32s3 merge_bin -o full.bin --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x19000 ota_data_initial.bin 0x20000 KeSp.bin 0x420000 storage.bin
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 full.bin      # (release assets ship this as *_full.bin)
```

Afterwards, the app only, at 0x20000 — settings and pairing in NVS survive:

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x20000 build_my_keyboard/KeSp.bin
```

A board running this firmware can reboot into the ROM download mode over its
own USB: `scripts/kesp_cdc.py /dev/ttyACM0 dfu`, then the `esptool.py`
command above with `--before no_reset --after hard_reset`. Halves with a
programming header (ESP-Prog / FTDI) are flashed through it, MAC-checked
(`esptool.py read_mac`) when several boards are on the bench.

## 5. Wireless split and dongle

- Build and flash `kase_dongle` on the receiver. Plug it in: it is the USB
  keyboard the computer sees.
- Open its pairing window: `scripts/kesp_cdc.py /dev/ttyACM0 pair` (30 s;
  `pair 1` forgets previous pairs). Power an **unpaired** half: it requests a
  pairing on the rendezvous channel, gets a `set_id`, stores it in NVS and
  reboots paired. Do the other half. `scripts/kesp_cdc.py /dev/ttyACM0 pairs`
  lists them.
- Both halves send their raw half-matrix to the dongle, which fuses them and
  runs the keymap engine — one engine in the system. The left half on USB
  takes over (the dongle goes quiet and re-emits the right half to it); unplug
  it and the dongle types again.
- Channel plan and slot contract: `main/comm/rf/rf_slot.h`. Two keyboards in
  the same room are told apart by their `set_id`.

⚠ A full flash (0x0) of a half erases its pairing: pair again.

## 6. Remap from the host

The keyboard speaks a binary protocol over its USB CDC port
(`docs/CDC_BINARY_PROTOCOL.md`): keymaps, layers, macros, tap-dance, combos,
leader sequences, statistics, OTA. The desktop application is
**KeSp_controller** (https://gitlab.com/harrael/KeSp_controller). In a wireless
split the keymap lives in the dongle; the left half pulls it over the radio
(ACK payload) so that both engines agree — a mismatch is reported as a
"config divergence".

Keycodes: `main/input/key_definitions.h` (HID, `MO`/`TO` layers, `LT`/`MT`,
tap-dance, combos, *Modified Key* 0x8000-0x8FFF = "this key sends Shift+1").
A key keeps the layer it was pressed on until it is released.

## 7. Reading the board

- **Console** (UART header, 115200): `scripts/console-capture.py /dev/ttyUSB2 log.txt`
  — never `cat` the port, DTR/RTS would hold the chip in reset. The
  heartbeat line every 10 s says everything about power:
  `HB up=751s idle=12s slept=702s/1 vetos=- link=0 batt=40 dV` — `up` is
  real time (RTC), `slept` the light-sleep total, `vetos` who keeps the board
  awake (`usb`, `link`, `sync`, `test`, `pair`). Light sleep after 15 s of
  inactivity, deep sleep after 4 h, wake on any key.
- **Dongle diagnostics** (no console — CDC only):
  `scripts/kesp_cdc.py /dev/ttyACM0 rfstat` → frames received, overwritten
  transitions (should stay 0), engine gap, USB reports sent/refused, re-press
  detector (a key re-pressed < 30 ms after its release: a bounce or a stale
  radio repeat).
- **Screens**: route (USB/RF), ▲ when the dongle acknowledged, battery gauge
  (thick border = low), layer on the left.

## 8. Contributing a board or a fix

- `./scripts/check.sh --fast` before every commit, `./scripts/check.sh` (all in-tree boards)
  before a push — the pre-push hook does it (`./scripts/install-hooks.sh`).
- Pure logic gets a host test first (`test/`), behaviour gets a line in
  `COMPORTEMENTS.md` (the contract `check.sh` verifies).
- The CI builds every board and an out-of-tree template board on each push;
  a `vX.Y.Z` tag publishes the release (pre-release when the tag has a
  suffix). `CLAUDE.md` is the long-form map of the project and its hard-won
  rules — worth reading before touching the radio or the sleep code.
