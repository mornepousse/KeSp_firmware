#!/usr/bin/env bash
# KaSe brick: docs/KEYCODE_MAP.md is the contract with KeSp_controller.
# Every keycode defined with a literal value >= 0x0100 in
# main/input/key_definitions.h must appear in the map — as its own `0xHHHH`
# or inside a documented `0xAAAA-0xBBBB` range. Three keycodes (0x3B00 of
# 2026-03-30, 0x3E00 of 2026-06-09, 0x3F00 of 2026-07-04) entered the header
# without the map, and the controller ignored them for months (found by the
# controller session, 2026-09-29). Sourced by scripts/check.sh.
check_keycode_map() {
  [ -f main/input/key_definitions.h ] && [ -f docs/KEYCODE_MAP.md ] || return 0
  local missing
  missing="$(python3 - <<'PY'
import re
hdr = open("main/input/key_definitions.h").read()
doc = open("docs/KEYCODE_MAP.md").read()
defs = {}
for name, val in re.findall(r"^#define\s+(K_\w+|MO_\w+|TO_\w+)\s+(0x[0-9A-Fa-f]{4})\b", hdr, re.M):
    v = int(val, 16)
    if v >= 0x0100:
        defs.setdefault(v, name)
singles = {int(h, 16) for h in re.findall(r"`0x([0-9A-Fa-f]{4})`", doc)}
ranges = [(int(a, 16), int(b, 16)) for a, b in re.findall(r"`0x([0-9A-Fa-f]{4})-0x([0-9A-Fa-f]{4})`", doc)]
for v, name in sorted(defs.items()):
    if v in singles or any(a <= v <= b for a, b in ranges):
        continue
    print(f"{name}=0x{v:04X}")
PY
)"
  if [ -n "$missing" ]; then
    fail "docs/KEYCODE_MAP.md is missing keycodes defined in key_definitions.h (KeSp_controller cannot know them): $(echo $missing)"
    return 1
  fi
}
TW_PRE_FAST+=(check_keycode_map)
