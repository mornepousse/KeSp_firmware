#!/usr/bin/env bash
# Hook Claude Code Stop — check du variant courant avant de conclure.
# Dégradation: env de build absent -> --fast au lieu de bloquer chaque Stop.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO" || exit 1
# python3 requis pour la garde anti-boucle ; sans lui le hook est inactif (signalé).
command -v python3 >/dev/null 2>&1 || { echo "tripwire: python3 absent, hook Stop inactif" >&2; exit 0; }
# Anti-boucle : si on est déjà dans une continuation de Stop hook, ne pas re-bloquer.
IN="$(cat 2>/dev/null || true)"
printf '%s' "$IN" | python3 -c 'import sys,json; sys.exit(0 if json.load(sys.stdin).get("stop_hook_active") else 1)' 2>/dev/null && exit 0
VARIANT="$(cat .kase-board 2>/dev/null || true)"
VARIANT="${VARIANT:-kase_v2_debug}"
if [ -z "${IDF_PATH:-}" ] && [ -f "$HOME/esp/esp-idf/export.sh" ]; then
  source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1 || true
fi
if [ -n "${IDF_PATH:-}" ]; then
  OUT="$("$REPO/scripts/check.sh" --variant "$VARIANT" 2>&1)"
  rc=$?
  MSG="check.sh (board $VARIANT) est ROUGE avant de conclure :"
else
  OUT="$("$REPO/scripts/check.sh" --fast 2>&1)"
  rc=$?
  MSG="check.sh --fast est ROUGE avant de conclure (env de build absent, phase complète sautée) :"
fi
if [ "$rc" -ne 0 ]; then
  echo "$MSG" >&2
  echo "$OUT" | tail -10 >&2
  exit 2
fi
exit 0
