# __BOARD_NAME__

Created from `boards/_template` by `scripts/new-board.sh`. The folder is the
whole registration: nothing else in the repository needs to know this board.

| File | Owns |
|---|---|
| `board.h` | pins, matrix geometry and tables, `BOARD_PINS(X)`, optional feature blocks |
| `board_keymap.c` | compile-time default keymap and layer names |
| `board_layout.c` | physical layout JSON for KeSp_controller |
| `sdkconfig.defaults` | Kconfig features of this board, on top of the root defaults |

Build and flash:

```bash
idf.py -B build___BOARD_NAME__ -DBOARD=__BOARD_NAME__ -DSDKCONFIG=build___BOARD_NAME__/sdkconfig build
esptool --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x20000 build___BOARD_NAME__/KeSp.bin
```

`./scripts/check.sh --fast` runs the host tests, including this board's pin
contract (`test/test_board_contract___BOARD_NAME__.c`); `./scripts/check.sh`
builds it with the others.
