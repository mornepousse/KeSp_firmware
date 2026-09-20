#!/usr/bin/env bash
# Create boards/<name>/ from boards/_template/ and its host contract test.
# The folder is the registration: CMake (-DBOARD=<name>), check.sh (boards are
# discovered from boards/*/sdkconfig.defaults) and the release script find it
# from there. Only the host test needs two lines registered (test/CMakeLists.txt,
# test/test_main.c) — done here.
set -euo pipefail
name="${1:?usage: scripts/new-board.sh <name>   (lowercase letters, digits, underscore)}"
[[ "$name" =~ ^[a-z][a-z0-9_]*$ ]] || { echo "bad board name: $name" >&2; exit 2; }
[ "$name" = "_template" ] && { echo "_template is the template" >&2; exit 2; }
root="$(cd "$(dirname "$0")/.." && pwd)"
dst="$root/boards/$name"
[ -e "$dst" ] && { echo "$dst already exists" >&2; exit 1; }

cp -r "$root/boards/_template" "$dst"
sed -i "s/__BOARD_NAME__/$name/g" "$dst"/*

# Host contract test: one translation unit per board (test/board_contract.inc).
cat > "$root/test/test_board_contract_$name.c" <<EOC
/* Generic board contract for boards/$name — see test/board_contract.inc. */
#include "test_framework.h"
#include "driver/gpio.h"        /* host stub: GPIO_NUM_* */
#ifndef SPI2_HOST
#define SPI2_HOST 1
#define SPI3_HOST 2
#endif
#include "../boards/$name/board.h"
#define BOARD_CONTRACT_NAME "$name"
#define BOARD_CONTRACT_FN   test_board_contract_$name
#include "board_contract.inc"
EOC
sed -i "s|^\(    test_board_contract_conchodytes.c\)$|\1\n    test_board_contract_$name.c|" "$root/test/CMakeLists.txt"
sed -i "s|^\(extern void test_board_contract_conchodytes(void);\)$|\1\nextern void test_board_contract_$name(void);|" "$root/test/test_main.c"
sed -i "s|^\(    test_board_contract_conchodytes();\)$|\1\n    test_board_contract_$name();|" "$root/test/test_main.c"

echo "created boards/$name/ and test/test_board_contract_$name.c"
echo "next: edit boards/$name/board.h (pins, BOARD_PINS), then"
echo "  ./scripts/check.sh --fast"
echo "  idf.py -B build_$name -DBOARD=$name -DSDKCONFIG=build_$name/sdkconfig build"
