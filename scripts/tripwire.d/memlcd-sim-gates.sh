#!/usr/bin/env bash
# KaSe brick: the left screen's simulator gates (tools/memlcd_sim, README).
# The host tests lay the memory-LCD screen out with a width ORACLE
# (memlcd_font_widths.h, kerning included) and trust a 12 px text FLOOR; only
# the real LVGL can prove either. tools/memlcd_sim/build.sh --gates compiles
# the managed LVGL on the host and runs:
#   1. check_widths     — the oracle == lv_txt_get_width, every font;
#   2. check_glyph_ink  — every font of the engine's table keeps its ink
#                         through the panel's 1-bit threshold (the floor);
#   3. test_safe_wrap   — the safe wraps' lines, re-measured by LVGL.
# Red on any gate. Skipped WITH A NOTICE when managed_components has not
# been fetched (no LVGL to compile) or gcc is missing. The first run builds
# liblvgl.a (~20 s, cached under tools/memlcd_sim/build/, gitignored); the
# next ones take a few seconds. Sourced by scripts/check.sh.
check_memlcd_sim_gates() {
  [ -x tools/memlcd_sim/build.sh ] || return 0
  if [ ! -d managed_components/lvgl__lvgl/src ]; then
    info "memlcd_sim gates SKIPPED (managed_components/lvgl__lvgl absent — run any idf.py build or reconfigure once)"
    return 0
  fi
  if ! command -v "${CC:-gcc}" >/dev/null 2>&1; then
    info "memlcd_sim gates SKIPPED (no host C compiler)"
    return 0
  fi
  local out
  if ! out="$(tools/memlcd_sim/build.sh --gates 2>&1)"; then
    printf '%s\n' "$out" | grep -E 'FAIL|MISMATCH|OVER|UNSAFE|error' | grep -v 'montserrat_(8|10)\b' | tail -15 >&2
    fail "memlcd_sim gates: red — detail: tools/memlcd_sim/build.sh --gates"
    return 1
  fi
  ok "memlcd_sim gates (width oracle, glyph ink, safe wrap measured by LVGL)"
  return 0
}
TW_PRE_FAST+=(check_memlcd_sim_gates)
