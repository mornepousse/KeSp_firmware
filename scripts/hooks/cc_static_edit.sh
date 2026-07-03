#!/usr/bin/env bash
# PostToolUse (Edit|Write|MultiEdit) — analyse statique du fichier .c édité via
# cppcheck. ADVISORY et FAIL-SOFT : ne bloque jamais (exit 0 toujours), remonte
# seulement des findings. DORMANT si cppcheck absent (no-op propre) → s'active
# automatiquement dès que cppcheck est installé (NixOS: ajoute `cppcheck` au profil,
# ou test ponctuel: nix-shell -p cppcheck).
# cppcheck fait son propre parsing : tolérant aux flags GCC xtensa et aux headers
# ESP-IDF manquants, contrairement à clang-tidy (qui exige pyclang/idf.py clang-check).
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

command -v cppcheck >/dev/null 2>&1 || exit 0     # dormant tant que cppcheck absent

FP="$(python3 -c 'import sys,json; print(json.load(sys.stdin).get("tool_input",{}).get("file_path",""))' 2>/dev/null || true)"
case "$FP" in
  *"/main/"*.c|*"/boards/"*.c) ;;                  # sources C uniquement
  *) exit 0 ;;
esac
[ -f "$FP" ] || exit 0

# Analyse mono-fichier, embarqué-friendly. Pas besoin de résoudre tous les headers
# (missingInclude supprimé) → rapide, zéro dépendance au board courant.
OUT="$(timeout 30 cppcheck \
        --enable=warning,performance,portability \
        --language=c --std=c11 \
        --inline-suppr --quiet \
        --suppress=missingInclude --suppress=missingIncludeSystem \
        "$FP" 2>&1)" || exit 0                     # erreur/timeout => fail-soft

if [ -n "$OUT" ]; then
  echo "🔎 cppcheck sur $(basename "$FP") — advisory (non bloquant) :" >&2
  printf '%s\n' "$OUT" | head -12 >&2
fi
exit 0
