#!/usr/bin/env bash
# Harnais footprint KaSe — taille app par board + marge partition OTA, trackées
# dans perf/sizes.json (versionnable), avec détection de régression vs baseline.
# Statique (lit les .bin buildés) — ne build pas, ne touche pas au matériel.
#
# Modes:
#   perf-size.sh            -> rapport (taille, marge OTA, delta vs baseline)
#   perf-size.sh --update   -> rapport + écrit la nouvelle baseline (perf/sizes.json)
#   perf-size.sh --check     -> rapport + exit != 0 si un board grossit > seuil (hooks/CI)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$SCRIPT_DIR")"
cd "$REPO"

ALL_BOARDS=(kase_v1 kase_v2 kase_v2_debug kase_dongle kase_half_left kase_half_right)
DB="perf/sizes.json"
GROWTH_PCT="${PERF_GROWTH_PCT:-5}"   # warn si +X% vs baseline
TIGHT_PCT="${PERF_TIGHT_PCT:-15}"    # warn si marge OTA < X%
MODE="report"; [ "${1:-}" = "--update" ] && MODE="update"; [ "${1:-}" = "--check" ] && MODE="check"

# Taille de la partition app (factory) depuis partitions.csv (fallback 2 MiB).
FACTORY_HEX="$(awk -F',' 'tolower($1) ~ /factory/ && tolower($3) ~ /factory/ {gsub(/[ \t]/,"",$5); print $5}' partitions.csv 2>/dev/null | head -1)"
FACTORY=$(( ${FACTORY_HEX:-0x200000} ))
[ "$FACTORY" -gt 0 ] || FACTORY=$((0x200000))

# Collecte des tailles courantes (boards buildés uniquement).
SIZES=""
for b in "${ALL_BOARDS[@]}"; do
  bin="build_$b/KeSp.bin"
  [ -f "$bin" ] && SIZES="$SIZES$b $(stat -c%s "$bin")"$'\n'
done

mkdir -p perf
FACTORY="$FACTORY" GROWTH_PCT="$GROWTH_PCT" TIGHT_PCT="$TIGHT_PCT" MODE="$MODE" DB="$DB" \
python3 - "$SIZES" <<'PY'
import json, os, sys
factory = int(os.environ["FACTORY"]); growth = float(os.environ["GROWTH_PCT"])
tight = float(os.environ["TIGHT_PCT"]); mode = os.environ["MODE"]; db = os.environ["DB"]
cur = {}
for line in sys.argv[1].splitlines():
    if line.strip():
        b, s = line.split(); cur[b] = int(s)
base = {}
try:
    base = json.load(open(db)).get("boards", {})
except Exception:
    pass
if not cur:
    print("Aucun board buildé (pas de build_*/KeSp.bin). Build d'abord."); sys.exit(0)

def hum(n): return f"{n/1024:.1f} KiB"
print(f"{'board':16} {'app':>11} {'marge OTA':>10} {'Δ baseline':>14}  flags")
regressed = False
for b in sorted(cur):
    sz = cur[b]; free_pct = 100*(factory-sz)/factory
    flags = []
    if free_pct < tight: flags.append("TIGHT")
    d = ""
    if b in base:
        delta = sz - base[b]; dpct = 100*delta/base[b] if base[b] else 0
        sign = "+" if delta >= 0 else ""
        d = f"{sign}{delta} ({sign}{dpct:.1f}%)"
        if dpct > growth: flags.append("GROWTH"); regressed = True
    else:
        d = "(nouveau)"
    print(f"{b:16} {hum(sz):>11} {free_pct:>9.1f}% {d:>14}  {' '.join(flags)}")

if mode == "update":
    json.dump({"factory": factory, "boards": cur}, open(db, "w"), indent=2, sort_keys=True)
    open(db, "a").write("\n")
    print(f"\n→ baseline écrite dans {db}")
elif mode == "check" and regressed:
    print(f"\n✗ régression footprint (> {growth:.0f}% vs baseline). Investigue ou --update si voulu.")
    sys.exit(1)
elif mode == "report":
    print(f"\n(baseline: {db} ; --update pour l'enregistrer, --check pour CI/hooks)")
PY
