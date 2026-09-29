#!/usr/bin/env bash
# KaSe brick: the chest confirm invariant (spec 2026-09-29 §5).
# A confirmation reaches the chest only from a real key press: chest_gate_press()
# may be called from key_processor.c alone. Sourced by scripts/check.sh.
check_chest_confirm_invariant() {
  local bad
  bad="$(grep -rl --include='*.c' --include='*.h' 'chest_gate_press' main 2>/dev/null \
         | grep -v -e '^main/input/key_processor.c$' -e '^main/comm/chest/chest_gate\.[ch]$' || true)"
  if [ -n "$bad" ]; then
    fail "chest confirm invariant: chest_gate_press() called outside key_processor.c — $bad"
    return 1
  fi
}
TW_PRE_FAST+=(check_chest_confirm_invariant)
