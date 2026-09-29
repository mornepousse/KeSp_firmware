#!/usr/bin/env bash
# KaSe brick: main/display/memlcd/memlcd_font_widths.h is the width oracle of
# the chest views on the memory-LCD (host tests measure text with it, LVGL
# draws with the real font). It is generated from the LVGL font files by
# scripts/gen_memlcd_font_widths.py; if the managed LVGL is bumped and its
# Montserrat advances move, the tests would keep proving widths the panel no
# longer draws. This brick regenerates and compares — red on any drift.
# Skipped (with a notice) when managed_components has not been fetched.
# Sourced by scripts/check.sh.
check_font_widths() {
  [ -f scripts/gen_memlcd_font_widths.py ] || return 0
  local out
  if ! out="$(python3 scripts/gen_memlcd_font_widths.py --check 2>&1)"; then
    fail "font width table: $out"
    return 1
  fi
  [ -n "$out" ] && echo "$out"
  return 0
}
TW_PRE_FAST+=(check_font_widths)
