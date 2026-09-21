#!/usr/bin/env bash
# tripwire-template: v0.16.0
# Tripwire anti-régression KaSe — source unique de vérité du "quoi vérifier".
# Généré par /tripwire:init. Adapter ICI ; les hooks ne font qu'appeler ce script.
# Modes:
#   check.sh                  -> full: phase rapide + toutes les variantes
#   Alias de dialecte KaSe : --host-only = --fast ; --board <name> = --variant <name>
#   check.sh --fast           -> phase rapide uniquement (~secondes)
#   check.sh --variant <name> -> phase rapide + une seule variante
# Options:
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

# ccache : les 7 boards partagent la majorité des composants → cache hit croisé.
export IDF_CCACHE_ENABLE=1
# Pas de « check for new versions » du component manager (2.2.2, 2026-09-18) :
# il interroge le registre et évalue les manifestes de LVGL 9 contre notre
# sdkconfig LVGL 8 → MissingKconfigError: LV_USE_LIBJPEG_TURBO, fatal à la
# configuration. Le lock (dependencies.lock) fait foi ; un build ne dépend pas
# du réseau.
export IDF_COMPONENT_CHECK_NEW_VERSION=0

# Variantes de build. Laisser vide pour un projet mono-cible.
# KaSe : les cartes sont DÉCOUVERTES depuis boards/*/sdkconfig.defaults — un
# nouveau dossier de carte est sa propre inscription (plan 2026-09-20
# « a board is one folder »). _template est le gabarit de scripts/new-board.sh.
mapfile -t ALL_VARIANTS < <(for d in boards/*/sdkconfig.defaults; do d="${d#boards/}"; d="${d%/sdkconfig.defaults}"; [ "$d" != "_template" ] && echo "$d"; done)


# Avis TDD (optionnel) : formes grep -E des chemins source et test. Vides -> inerte.
SRC_GREP="^main/|^boards/"
TEST_GREP="^test/"

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YEL=$'\033[1;33m'; NC=$'\033[0m'
fail() { echo "${RED}✗ $*${NC}" >&2; }
ok()   { echo "${GREEN}✓ $*${NC}"; }
info() { echo "${YEL}» $*${NC}"; }

MODE="full"; SINGLE_VARIANT=""; FORCE="${TRIPWIRE_FORCE:-0}"
while [ $# -gt 0 ]; do
  case "$1" in
    --fast|--host-only) MODE="fast" ;;
    --variant|--board)  MODE="single"; SINGLE_VARIANT="${2:-}"
               [ -z "$SINGLE_VARIANT" ] && { fail "--variant/--board requires a name"; exit 2; }
               shift ;;
    --force)   FORCE=1 ;;
    *)         fail "unknown arg: $1"; exit 2 ;;
  esac
  shift
done

# ---- Résolution du scope de la phase rapide (module éventuel) ----
FAST_RUN_CMD="cmake -S test -B test/build && cmake --build test/build && ./test/build/test_runner"; FAST_LABEL="Phase rapide"

# ---- Briques : les mécanismes optionnels vivent dans scripts/tripwire.d/ ----
# Chaque fichier est autonome et s'enregistre dans une liste ; le noyau appelle.
# Installer = copier la brique depuis le plugin. Retirer = supprimer le fichier.
TW_PRE_FAST=(); TW_POST_FULL=()
for _b in "$SCRIPT_DIR"/tripwire.d/*.sh; do
  [ -f "$_b" ] && . "$_b"
done

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
KEY="$MODE${SINGLE_VARIANT:+-$SINGLE_VARIANT}"
[ "${TRIPWIRE_RATCHET_STRICT:-0}" = "1" ] && KEY="$KEY-strict"   # un run strict ne skippe que contre un vert strict
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
    info "⚠ phase rapide: ${dt}s > budget ${budget}s — déplacer des tests vers le check complet"
  fi
  return "$rc"
}

# ---- Phase complète ----
# Multi-variantes: appelée une fois par variante ($v = nom).
# Mono-cible: appelée une fois avec $v vide.
build_variant() {
  local v="$1"
  # Un outil absent n'est PAS une régression. Sans idf.py — hook lancé hors du
  # devshell Nix, CI sans toolchain — on saute la phase de build en l'annonçant,
  # au lieu de rendre un rouge indiscernable d'un vrai code cassé. Un rouge qui
  # veut dire "toolchain absente" finit par ne plus être lu.
  # Déclaré dans .tripwire-divergences.
  if ! command -v idf.py >/dev/null 2>&1; then
    info "Build ${v:-complet} SAUTÉ (idf.py absent — lancer dans le devshell pour un check complet)"
    return 0
  fi
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
for _fn in ${TW_PRE_FAST[@]+"${TW_PRE_FAST[@]}"}; do "$_fn" || rc=1; done
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

for _fn in ${TW_POST_FULL[@]+"${TW_POST_FULL[@]}"}; do "$_fn" || rc=1; done

# ---- Source modifiée sans test : un avis, jamais un rouge ----
# Avec COMPORTEMENTS.md, l'avis demande quel comportement est touché. Il a été
# bloquant au Stop pendant une semaine sur un vrai projet : 17 blocages, 66
# commits taxés d'une ligne au contrat. Le rouge du contrat est réservé à une
# garde [test:X] qui disparaît.
if [ -n "$SRC_GREP" ] && [ -n "$TEST_GREP" ]; then
  CH="$( { git diff --name-only HEAD; git ls-files -o --exclude-standard; } 2>/dev/null | sort -u)"
  if [ -n "$CH" ]; then
    NSRC="$(printf '%s\n' "$CH" | grep -cE "$SRC_GREP" || true)"
    NTST="$(printf '%s\n' "$CH" | grep -cE "$TEST_GREP" || true)"
    if [ "$NSRC" -gt 0 ] 2>/dev/null && [ "$NTST" -eq 0 ] 2>/dev/null; then
      if [ -f COMPORTEMENTS.md ] && ! printf '%s\n' "$CH" | grep -qx 'COMPORTEMENTS.md'; then
        info "⚠ $NSRC source(s) sans test — quel comportement de COMPORTEMENTS.md ? (avis)"
      elif [ ! -f COMPORTEMENTS.md ]; then
        info "⚠ TDD: $NSRC fichier(s) source modifié(s) sans test modifié — test d'abord ?"
      fi
    fi
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
