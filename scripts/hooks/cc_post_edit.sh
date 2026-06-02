#!/usr/bin/env bash
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
FP="$(python3 -c 'import sys,json; d=json.load(sys.stdin); print(d.get("tool_input",{}).get("file_path",""))' 2>/dev/null)"
case "$FP" in
  *"/main/"*.c|*"/main/"*.h|*"/boards/"*|*"/test/"*) ;;
  *) exit 0 ;;  # fichier non surveillé → rien
esac
OUT="$("$REPO/scripts/check.sh" --host-only 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "Régression tests host après édition de $FP :" >&2
  echo "$OUT" | tail -8 >&2
  exit 2   # remonte à Claude
fi
exit 0
