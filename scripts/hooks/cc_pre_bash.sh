#!/usr/bin/env bash
# PreToolUse guard for Bash — bloque les footguns KaSe/ESP-IDF documentés dans
# CLAUDE.md AVANT exécution. Contrat Claude Code : exit 2 + stderr => l'appel
# outil est bloqué et le message est renvoyé à Claude.
set -uo pipefail

payload="$(cat)"
cmd="$(printf '%s' "$payload" | jq -r '.tool_input.command // empty' 2>/dev/null || true)"
[ -z "$cmd" ] && cmd="$(printf '%s' "$payload" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("tool_input",{}).get("command",""))' 2>/dev/null || true)"
[ -z "$cmd" ] && exit 0

block() { printf '🚫 [pre-bash guard] %s\n' "$1" >&2; exit 2; }

# --- Footgun 1 : build board sans sdkconfig isolé → fuite de config entre boards ---
# CLAUDE.md : « Jamais builder deux boards dans le même build/ avec le sdkconfig racine. »
if printf '%s' "$cmd" | grep -qE 'idf\.py\b' \
   && printf '%s' "$cmd" | grep -qE '(^|[[:space:]])(build|app|reconfigure)([[:space:]]|$)' \
   && printf '%s' "$cmd" | grep -qE '\-DBOARD=' \
   && ! printf '%s' "$cmd" | grep -qE '\-DSDKCONFIG='; then
  block "Build board sans -DSDKCONFIG=build_<board>/sdkconfig → fuite de config entre boards (CLAUDE.md). Utilise: idf.py -B build_<board> -DBOARD=<board> -DSDKCONFIG=build_<board>/sdkconfig build"
fi

# --- Footgun 2 : erase flash / wipe NVS → tue le pairing espnow (e-ink mort) ---
# CLAUDE.md : « Jamais d'erase NVS sans raison » ; mémoire : erase → re-pairing requis.
# Exige un VRAI contexte d'invocation (outil de flash présent) + un verbe erase,
# sinon un simple message de commit / echo contenant "erase_flash" faux-positive.
if printf '%s' "$cmd" | grep -qE '\b(esptool(\.py)?|parttool(\.py)?|idf\.py)\b' \
   && printf '%s' "$cmd" | grep -qE 'erase[_-]?flash|erase[_-]?region|erase[_-]?partition' \
   && ! printf '%s' "$cmd" | grep -qF 'ALLOW_ERASE'; then
  block "erase_flash efface la NVS (config, keymaps, bigrams) ET le pairing espnow → l'e-ink meurt, re-pairing requis. Si c'est voulu : ajoute '# ALLOW_ERASE' à la commande, ou lance-la toi-même via ! dans le prompt."
fi

exit 0
