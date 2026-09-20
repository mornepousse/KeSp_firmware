#!/usr/bin/env bash
# Flashes a Niphargus half WHILE VERIFYING THE CHIP'S IDENTITY.
#
# Why this script exists: both halves are programmed through the SAME FTDI
# adapter, moved from one to the other. Nothing in `idf.py flash` says which
# one it's writing to — the port stays /dev/ttyUSB2 in both cases. On
# 2026-09-07, the MASTER's firmware was written onto the SCANNER: the right
# half stopped scanning and started listening instead, and nothing objected.
# The keyboard had silently lost a half.
#
# The MAC address identifies the chip and depends on no wiring. It is read
# before writing, and refused if it doesn't match the requested half.
#
# Usage: ./scripts/flash-niphar.sh <left|right> [port]
set -euo pipefail

MAC_LEFT="d0:cf:13:21:92:60"    # read on 2026-09-07
MAC_RIGHT="80:b5:4e:eb:5e:08"   # read on 2026-09-07

moitie="${1:-}"
port="${2:-/dev/ttyUSB2}"

case "$moitie" in
  left)  attendue="$MAC_LEFT";  board=niphar_left  ;;
  right) attendue="$MAC_RIGHT"; board=niphar_right ;;
  *) echo "usage: $0 <left|right> [port]" >&2; exit 2 ;;
esac

if ! command -v esptool >/dev/null 2>&1; then
  echo "esptool missing from PATH — run from the devshell:" >&2
  echo "  nix develop ~/nixos-config#esp-idf" >&2
  exit 1
fi

lue="$(esptool --chip esp32s3 -p "$port" read_mac 2>/dev/null \
        | sed -n 's/^MAC:[[:space:]]*//p' | head -1)"

if [ -z "$lue" ]; then
  echo "no chip answers on $port" >&2
  exit 1
fi

if [ "$lue" != "$attendue" ]; then
  echo "REFUSED: the chip on $port is not the \"$moitie\" half." >&2
  echo "  expected: $attendue" >&2
  echo "  read     : $lue" >&2
  case "$lue" in
    "$MAC_LEFT")  echo "  -> the LEFT half is plugged in." >&2 ;;
    "$MAC_RIGHT") echo "  -> the RIGHT half is plugged in." >&2 ;;
    *) echo "  -> unknown chip; if the board is new, add its MAC here." >&2 ;;
  esac
  exit 1
fi

echo "chip confirmed: $moitie ($lue) — writing $board"
exec idf.py -B "build_$board" -p "$port" -b 460800 flash
