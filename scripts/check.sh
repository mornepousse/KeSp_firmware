#!/usr/bin/env bash
# tripwire-template: v0.7.0
# Tripwire anti-régression KaSe — source unique de vérité du "quoi vérifier".
# Généré par /tripwire:init. Adapter ICI ; les hooks ne font qu'appeler ce script.
# Modes:
#   check.sh                  -> full: phase rapide + toutes les variantes (6 boards)
#   check.sh --fast           -> phase rapide uniquement (tests host, ~secondes)
#   check.sh --variant <name> -> phase rapide + build d'un seul board
# Alias historiques KaSe (CI, agents et docs les utilisent — conservés) :
#   --host-only = --fast ; --board <name> = --variant <name>
# Options:
#   --changed <fichier>       -> (passé par les hooks) route la phase rapide sur le
#                                module touché si MODULE_FAST est renseigné
#   --force                   -> ignore le skip-si-déjà-vert (aussi: TRIPWIRE_FORCE=1)
# Env:
#   TRIPWIRE_FAST_BUDGET      -> budget de la phase rapide en secondes (défaut 30) ;
#                                dépassé -> avertissement non fatal
# Sortie non-zéro si au moins un rouge (toutes les variantes sont tentées en mode full).
# Conçu pour hooks git/plateforme + CI.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR" || exit 1

# ccache : les 6 boards partagent la majorité des composants → cache hit croisé.
# ESP-IDF respecte cette variable ; après le 1er board, les suivants réutilisent
# les objets compilés. Gros gain wall-clock sur le build full + pre-push.
export IDF_CCACHE_ENABLE=1

# Variantes de build. Laisser vide pour un projet mono-cible.
ALL_VARIANTS=(kase_v1 kase_v2 kase_v2_debug kase_dongle kase_half_left kase_half_right)

# Modules (monorepo, optionnel) : quand un hook passe --changed <fichier>, la
# phase rapide est routée sur le premier module dont le glob matche. Sans match
# ou table vide : phase rapide globale. Format "<glob>:<commande>". Exemple :
#   MODULE_FAST=( "*/services/api/*:cd services/api && npm test -s" )
MODULE_FAST=()

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YEL=$'\033[1;33m'; NC=$'\033[0m'
fail() { echo "${RED}✗ $*${NC}" >&2; }
ok()   { echo "${GREEN}✓ $*${NC}"; }
info() { echo "${YEL}» $*${NC}"; }

MODE="full"; SINGLE_VARIANT=""; CHANGED=""; FORCE="${TRIPWIRE_FORCE:-0}"
while [ $# -gt 0 ]; do
  case "$1" in
    --fast|--host-only) MODE="fast" ;;
    --variant|--board)  MODE="single"; SINGLE_VARIANT="${2:-}"
               [ -z "$SINGLE_VARIANT" ] && { fail "--variant/--board requires a name"; exit 2; }
               shift ;;
    --changed) CHANGED="${2:-}"; shift ;;
    --force)   FORCE=1 ;;
    *)         fail "unknown arg: $1"; exit 2 ;;
  esac
  shift
done

# ---- Résolution du scope de la phase rapide (module éventuel) ----
FAST_RUN_CMD="cmake -S test -B test/build && cmake --build test/build && ./test/build/test_runner"; FAST_LABEL="Phase rapide"; SCOPE_KEY=""
if [ -n "$CHANGED" ]; then
  for _e in ${MODULE_FAST[@]+"${MODULE_FAST[@]}"}; do
    case "$CHANGED" in
      ${_e%%:*}) FAST_RUN_CMD="${_e#*:}"; FAST_LABEL="Module ${_e%%:*}"
                 SCOPE_KEY="-mod-$(printf '%s' "${_e%%:*}" | git hash-object --stdin 2>/dev/null | cut -c1-8)"
                 break ;;
    esac
  done
fi

# ---- Verrou : un seul check à la fois (hooks concurrents) ----
GITDIR="$(git rev-parse --git-dir 2>/dev/null || echo .git)"
mkdir -p "$GITDIR/tripwire" 2>/dev/null || true
if command -v flock >/dev/null 2>&1 && [ -d "$GITDIR/tripwire" ]; then
  exec 9>"$GITDIR/tripwire/lock"
  if ! flock -n 9 2>/dev/null; then
    info "un check est déjà en cours — skip (son verdict fera foi)"
    exit 0
  fi
fi

# ---- Capture d'échec : la sortie du dernier rouge reste lisible sans re-run ----
OUTBUF="$GITDIR/tripwire/.out.$$"
trap 'rm -f "$OUTBUF"' EXIT
capture_fail() { # $1=label $2=commande affichée ; la sortie est déjà dans $OUTBUF
  {
    printf '# cmd: %s\n# mode: %s\n' "$2" "$1"
    tail -200 "$OUTBUF" 2>/dev/null
  } > "$GITDIR/tripwire/last-fail.log" 2>/dev/null || true
}

# ---- Skip-si-déjà-vert : même état que le dernier vert -> rien à refaire ----
fingerprint() {
  {
    git rev-parse HEAD 2>/dev/null || echo no-head
    git diff HEAD 2>/dev/null || git diff 2>/dev/null || true
    git ls-files -o --exclude-standard 2>/dev/null | LC_ALL=C sort \
      | git hash-object --stdin-paths 2>/dev/null || true
  } | git hash-object --stdin 2>/dev/null || date +%s.%N
}
KEY="$MODE${SINGLE_VARIANT:+-$SINGLE_VARIANT}$SCOPE_KEY"
STAMP="$GITDIR/tripwire/green-$KEY"
FP="$(fingerprint)"
if [ "$FORCE" != "1" ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$FP" ]; then
  ok "déjà vert (état inchangé depuis le dernier passage) — skip (--force pour relancer)"
  exit 0
fi

T_START=$SECONDS

# ---- Phase rapide (boucle courte, budget TRIPWIRE_FAST_BUDGET s) ----
run_fast() {
  info "${FAST_LABEL}…"
  local t0=$SECONDS rc=0
  if ( eval "$FAST_RUN_CMD" ) >"$OUTBUF" 2>&1; then
    ok "$FAST_LABEL OK"
  else
    capture_fail "$FAST_LABEL" "$FAST_RUN_CMD"
    fail "$FAST_LABEL: échec — détail: $GITDIR/tripwire/last-fail.log (ou relance: $FAST_RUN_CMD)"
    rc=1
  fi
  local dt=$((SECONDS - t0)) budget="${TRIPWIRE_FAST_BUDGET:-30}"
  if [ "$dt" -gt "$budget" ]; then
    info "⚠ phase rapide: ${dt}s > budget ${budget}s — déplacer des tests vers le check complet ou scoper par module (MODULE_FAST)"
  fi
  return "$rc"
}

# ---- Phase complète ----
# Multi-variantes: appelée une fois par variante ($v = nom).
# Mono-cible: appelée une fois avec $v vide.
build_variant() {
  local v="$1"
  info "Build ${v:-complet}…"
  if ( idf.py -B "build_$v" -DBOARD="$v" -DSDKCONFIG="build_$v/sdkconfig" build ) >"$OUTBUF" 2>&1; then
    ok "Build ${v:-complet} OK"
    return 0
  else
    capture_fail "Build ${v:-complet}" "idf.py -B "build_$v" -DBOARD="$v" -DSDKCONFIG="build_$v/sdkconfig" build"
    fail "Build ${v:-complet}: échec — détail: $GITDIR/tripwire/last-fail.log (ou relance: idf.py -B "build_$v" -DBOARD="$v" -DSDKCONFIG="build_$v/sdkconfig" build)"
    return 1
  fi
}

rc=0
run_fast || rc=1

if [ "$MODE" = "single" ]; then
  build_variant "$SINGLE_VARIANT" || rc=1
elif [ "$MODE" = "full" ]; then
  if [ "${#ALL_VARIANTS[@]}" -eq 0 ]; then
    build_variant "" || rc=1
  else
    for v in "${ALL_VARIANTS[@]}"; do
      build_variant "$v" || rc=1
    done
  fi
fi

echo "========================================"
if [ "$rc" -eq 0 ]; then
  printf '%s\n' "$FP" > "$STAMP" 2>/dev/null || true
  ok "check.sh: tout vert"
else
  fail "check.sh: ROUGE"
fi
# Historique des durées (jamais bloquant) — les skips sortent avant ce point.
{
  HIST="$GITDIR/tripwire/history.tsv"
  printf '%s\t%s\t%s\t%s\n' "$(date +%s)" "$KEY" "$((SECONDS - T_START))" "$rc" >> "$HIST"
  if [ "$(wc -l < "$HIST")" -gt 500 ]; then
    { tail -500 "$HIST" > "$HIST.$$" && mv "$HIST.$$" "$HIST"; } || rm -f "$HIST.$$"
  fi
} 2>/dev/null || true

echo "========================================"
exit "$rc"
