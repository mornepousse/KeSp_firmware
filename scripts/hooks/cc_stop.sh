#!/usr/bin/env bash
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
BOARD="$(cat .kase-board 2>/dev/null || echo kase_v2_debug)"
if [ -z "${IDF_PATH:-}" ] && [ -f "$HOME/esp/esp-idf/export.sh" ]; then
  source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1 || true
fi
# IDF indisponible : on dégrade vers les tests host (pas de faux rouge au build
# board qui ne pourrait pas tourner), au lieu de bloquer chaque Stop.
if [ -z "${IDF_PATH:-}" ]; then
  OUT="$("$REPO/scripts/check.sh" --host-only 2>&1)"
  rc=$?
  MSG="check.sh --host-only est ROUGE avant de conclure (IDF absent, build board sauté) :"
else
  OUT="$("$REPO/scripts/check.sh" --board "$BOARD" 2>&1)"
  rc=$?
  MSG="check.sh (board $BOARD) est ROUGE avant de conclure :"
fi
if [ "$rc" -ne 0 ]; then
  echo "$MSG" >&2
  echo "$OUT" | tail -10 >&2
  exit 2
fi
exit 0
