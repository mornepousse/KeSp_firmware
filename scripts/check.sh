#!/usr/bin/env bash
# Tripwire anti-régression KaSe — source unique de vérité du "quoi vérifier".
# Modes:
#   check.sh                  -> full: tests host + build des 6 boards (sdkconfig isolé)
#   check.sh --host-only      -> tests host uniquement (~secondes)
#   check.sh --board <name>   -> tests host + build d'un seul board
# Sortie non-zéro si au moins un rouge (tous les boards sont tentés en mode full).
# Conçu pour hooks git/Claude Code + CI.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

# ccache : les 6 boards partagent la majorité des composants → cache hit croisé.
# ESP-IDF respecte cette variable ; après le 1er board, les suivants réutilisent
# les objets compilés. Gros gain wall-clock sur le build full + pre-push, sans risque.
export IDF_CCACHE_ENABLE=1

ALL_BOARDS=(kase_v1 kase_v2 kase_v2_debug kase_dongle kase_half_left kase_half_right)

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YEL=$'\033[1;33m'; NC=$'\033[0m'
fail() { echo "${RED}✗ $*${NC}" >&2; }
ok()   { echo "${GREEN}✓ $*${NC}"; }
info() { echo "${YEL}» $*${NC}"; }

MODE="full"
SINGLE_BOARD=""
case "${1:-}" in
  --host-only) MODE="host" ;;
  --board)     MODE="single"; SINGLE_BOARD="${2:-}";
               [ -z "$SINGLE_BOARD" ] && { fail "--board requires a name"; exit 2; } ;;
  "" )         MODE="full" ;;
  *)           fail "unknown arg: $1"; exit 2 ;;
esac

# ---- Phase rapide : tests host ----
run_host_tests() {
  info "Tests host…"
  cmake -S test -B test/build >/dev/null 2>&1 || { fail "cmake (test) failed"; return 1; }
  cmake --build test/build >/dev/null 2>&1 || { fail "build des tests host failed"; return 1; }
  # test_runner sort 1 si au moins un test échoue (test_main.c) — on se fie
  # au code de sortie, plus robuste qu'un grep sur le format de sortie.
  if ./test/build/test_runner >/dev/null 2>&1; then
    ok "Tests host OK"
    return 0
  else
    fail "Tests host: échecs (relance ./test/build/test_runner pour le détail)"
    return 1
  fi
}

# ---- Phase board : build isolé ----
build_board() {
  local board="$1"
  info "Build $board…"
  if idf.py -B "build_$board" -DBOARD="$board" -DSDKCONFIG="build_$board/sdkconfig" build >/dev/null 2>&1; then
    ok "Build $board OK"
    return 0
  else
    fail "Build $board: échec (relance: idf.py -B build_$board -DBOARD=$board -DSDKCONFIG=build_$board/sdkconfig build)"
    return 1
  fi
}

rc=0
run_host_tests || rc=1

if [ "$MODE" = "single" ]; then
  build_board "$SINGLE_BOARD" || rc=1
elif [ "$MODE" = "full" ]; then
  for b in "${ALL_BOARDS[@]}"; do
    build_board "$b" || rc=1
  done
fi

echo "========================================"
if [ "$rc" -eq 0 ]; then ok "check.sh: tout vert"; else fail "check.sh: ROUGE"; fi
echo "========================================"
exit "$rc"
