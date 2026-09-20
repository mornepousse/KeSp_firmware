# Boards — a board is one folder

Every board lives in `boards/<name>/` and nothing else in the repository
names it: CMake builds it from `-DBOARD=<name>`, `scripts/check.sh` discovers
it from `boards/*/sdkconfig.defaults`, the host contract test checks its pins,
`scripts/build_release.sh` ships it. To add one:

```bash
scripts/new-board.sh my_keyboard      # copies boards/_template/, writes its contract test
$EDITOR boards/my_keyboard/board.h    # pins, BOARD_PINS(X), the features you have
./scripts/check.sh --fast             # host tests, including the board's pin contract
idf.py -B build_my_keyboard -DBOARD=my_keyboard -DSDKCONFIG=build_my_keyboard/sdkconfig build
```

## The files

| File | Owns |
|---|---|
| `board.h` | GPIO numbers (`ROWSn`/`COLSn`, radio, screen, battery, link…), `MATRIX_ROWS/COLS`, the tables the core reads (`BOARD_ROW_PINS`, `BOARD_COL_PINS`), the list of every GPIO used (`BOARD_PINS(X)`), USB VID/PID, product strings |
| `board_keymap.c` | compile-time default keymap and layer names (the live keymap is in NVS, edited with KeSp_controller) |
| `board_layout.c` | physical layout JSON served to KeSp_controller |
| `sdkconfig.defaults` | the board's Kconfig features, loaded on top of the root `sdkconfig.defaults` (mandatory, may be empty) |

A board may `#include` another board's `board.h` and override macros
(`kase_v2_debug` does): the tables name the macros, so an override before use
is seen by the core — but `BOARD_PINS(X)` must be redefined in full
(`#undef` first).

## What the core reads

- `BOARD_ROW_PINS` / `BOARD_COL_PINS`: exactly `MATRIX_ROWS` / `MATRIX_COLS`
  entries; `matrix_scan.c` and `veille.c` have no matrix shape of their own.
- `BOARD_MATRIX_COL2ROW`: the scan drives the columns and reads the rows;
  with COL → switch → diode → ROW wiring, sleep holds the columns high and any
  row wakes the board.
- Display backend: a real `#define BOARD_DISPLAY_BACKEND_OLED` /
  `_ROUND` in `board.h` (CMake reads the file — a mention in a comment is
  ignored), or `CONFIG_KASE_DISPLAY_MEMLCD=y` for the Sharp memory-LCD, or
  `CONFIG_KASE_NO_DISPLAY=y` for none.
- Feature blocks (radio, battery, TRRS link, trackpad) each come with their
  Kconfig symbol; `boards/_template/board.h` lists the macros of each.

## The contract (`test/board_contract.inc`)

One test unit per board (`test/test_board_contract_<name>.c`, written by
`new-board.sh`) expands `BOARD_PINS(X)` and checks: no GPIO used twice,
numbers in 0..48, no strapping pin (0/3/45/46 — the legacy V1/V2 pinouts waive
it with `BOARD_PINS_WAIVE_STRAPPING`), no GPIO19/20 when
`BOARD_USES_NATIVE_USB`, no GPIO35-37 when `BOARD_PSRAM_OCTAL`, and the pin
tables against `MATRIX_ROWS/COLS` and `KEYMAP_COLS`. Hardware-specific facts
(which GPIO is which, checked against a netlist) stay in a board's own test
(`test/test_niphar_left_pins.c` is the model).

## Roles

Kconfig roles are features, not board names: `KASE_DEVICE_ROLE_KEYBOARD`
(a keyboard with its own matrix and keymap engine), `KASE_SPLIT_MASTER` (the
half of a split that carries the engine), `KASE_DEVICE_ROLE_SPLIT_SCANNER`
(the half that only sends its matrix), `KASE_DEVICE_ROLE_DONGLE`,
`KASE_DEVICE_ROLE_MOUSE`.

## Boards in the tree

| Folder | What |
|---|---|
| `kase_v1` | KaSe V1 — round SPI display, LED strip, legacy pinout (strapping pins in the matrix) |
| `kase_v2` | KaSe V2 — I2C OLED, production pinout |
| `kase_v2_debug` | V2 with GPIO overrides for the prototype + a bodged nRF24 (bench board) |
| `kase_dongle` | USB receiver, two nRF24 (keyboard slot 1, mouse slot 2), runs the fusion engine |
| `niphar_left` | Niphargus left half — split master, memory-LCD, trackpad, TRRS link |
| `niphar_right` | Niphargus right half — split scanner, memory-LCD, TRRS link |
| `conchodytes` | Conchodytes mouse — PMW3389, clicks, wheel |
| `_template` | the template (`new-board.sh` copies it; CMake refuses to build it) |

## A board outside the repository

Keep your board in your own repository and this firmware as a pinned
dependency (submodule, or a checkout at a tag):

```bash
scripts/new-board.sh my_board /path/to/my_repo        # creates /path/to/my_repo/my_board/
idf.py -B build_my_board -DBOARD_DIR=/path/to/my_repo/my_board -DSDKCONFIG=build_my_board/sdkconfig build
```

`BOARD` is the folder's name unless you pass `-DBOARD=` too. The four files are
the same; the host contract test is not registered for you (it lives here) —
copy a `test/test_board_contract_*.c` into your own test setup if you want it.
