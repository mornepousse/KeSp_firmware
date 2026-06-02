# Filet anti-régression KaSe — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rendre les régressions *bruyantes au commit qui les cause* via une seule commande de vérification (`scripts/check.sh`) câblée dans des hooks git et Claude Code et dans la CI, sur les 6 boards.

**Architecture:** `scripts/check.sh` est la source unique de vérité du « quoi vérifier » (tests host + build des boards avec `sdkconfig` isolé par board). Trois consommateurs l'appellent : hook git `pre-push` (full 6 boards), hooks Claude Code (`PostToolUse` host-only, `Stop` board courant), et la CI GitHub Actions (matrice 6 boards, toutes branches). Plus deux compléments humains : checklist smoke-test hardware et normes documentées dans CLAUDE.md.

**Tech Stack:** Bash, ESP-IDF 5.5 (`idf.py`), CMake, GitHub Actions, hooks Claude Code (`.claude/settings.json`), python3 (parsing JSON des hooks).

**Référence spec:** `docs/superpowers/specs/2026-06-02-anti-regression-design.md`

**Pré-requis environnement:** ESP-IDF sourcé (`source ~/esp/esp-idf/export.sh`) pour toute étape qui build un board. Les étapes host-only n'en ont pas besoin.

**Convention clé (anti-fuite sdkconfig):** chaque build de board DOIT utiliser un dossier build dédié ET un sdkconfig dédié :
```
idf.py -B build_<board> -DBOARD=<board> -DSDKCONFIG=build_<board>/sdkconfig build
```
Ne jamais builder deux boards dans le même `build/` avec le `sdkconfig` racine partagé (fuite de config = régression silencieuse).

---

## File Structure

- Create: `scripts/check.sh` — tripwire, source unique de vérité. 3 modes : full / `--host-only` / `--board <name>`.
- Create: `scripts/hooks/pre-push` — hook git, appelle `check.sh` (full).
- Create: `scripts/hooks/cc_post_edit.sh` — dispatcher PostToolUse : parse le JSON stdin, lance `check.sh --host-only` si le fichier édité est du code surveillé.
- Create: `scripts/hooks/cc_stop.sh` — dispatcher Stop : lance `check.sh --board <courant>`.
- Create: `.claude/settings.json` — hooks Claude Code (versionné, partagé). N'existe pas encore (seul `settings.local.json` est présent).
- Create: `.kase-board` — fichier marqueur du board courant (une ligne, ex. `kase_v2_debug`).
- Create: `docs/HARDWARE_SMOKE_TEST.md` — checklist runtime une page.
- Modify: `.github/workflows/ci.yml` — matrice 6 boards, déclenchement toutes branches, isolation sdkconfig.
- Modify: `CLAUDE.md` — section workflow anti-régression (check.sh, hooks, norme TDD §4, déclencheurs agents §6).
- Modify: `.gitignore` — ignorer `build_*/` et `.kase-board` si pas déjà fait (vérifier).

---

## Task 1: `scripts/check.sh` — le tripwire

**Files:**
- Create: `scripts/check.sh`
- Reference: `scripts/build_release.sh` (pattern à NE PAS copier — il partage `build/` + sdkconfig)

- [ ] **Step 1: Écrire `scripts/check.sh`**

```bash
#!/usr/bin/env bash
# Tripwire anti-régression KaSe — source unique de vérité du "quoi vérifier".
# Modes:
#   check.sh                  -> full: tests host + build des 6 boards (sdkconfig isolé)
#   check.sh --host-only      -> tests host uniquement (~secondes)
#   check.sh --board <name>   -> tests host + build d'un seul board
# Sortie non-zéro au premier rouge. Conçu pour hooks git/Claude Code + CI.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

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
  if ./test/build/test_runner | tail -3 | grep -q ", 0 failed"; then
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
```

- [ ] **Step 2: Rendre exécutable et lancer le mode host-only**

Run:
```bash
chmod +x scripts/check.sh && ./scripts/check.sh --host-only
```
Expected: affiche `✓ Tests host OK` puis `✓ check.sh: tout vert`, exit 0.

- [ ] **Step 3: Vérifier la détection d'un rouge (test cassé)**

Introduire un échec temporaire pour prouver que le tripwire mord :
```bash
cp test/test_keycodes.c /tmp/test_keycodes.c.bak
printf '\nvoid _tripwire_check(void){ ASSERT_EQ(1, 2); }\n' >> test/test_keycodes.c
```
Note: si `_tripwire_check` n'est pas appelé, le test passera quand même. Plus fiable : casser une assertion existante. À la place :
```bash
cp test/test_keycodes.c /tmp/test_keycodes.c.bak
sed -i '0,/ASSERT_EQ(/s//ASSERT_EQ(999+/' test/test_keycodes.c
./scripts/check.sh --host-only; echo "exit=$?"
```
Expected: `✗ Tests host: échecs`, `exit=1`.

- [ ] **Step 4: Restaurer**

```bash
mv /tmp/test_keycodes.c.bak test/test_keycodes.c
./scripts/check.sh --host-only && echo "RESTORED OK"
```
Expected: `✓ check.sh: tout vert`, `RESTORED OK`.

- [ ] **Step 5: (si ESP-IDF dispo) Vérifier le build d'un board isolé**

Run:
```bash
source ~/esp/esp-idf/export.sh && ./scripts/check.sh --board kase_v2_debug; echo "exit=$?"
```
Expected: `✓ Tests host OK`, `✓ Build kase_v2_debug OK`, `exit=0`. Si ESP-IDF non dispo, noter et passer (la CI couvrira).

- [ ] **Step 6: Commit**

```bash
git add scripts/check.sh
git commit -m "feat(ci): scripts/check.sh tripwire — tests host + build 6 boards sdkconfig isolé"
```

---

## Task 2: Hook git `pre-push`

**Files:**
- Create: `scripts/hooks/pre-push`
- Modify: `CLAUDE.md` (note d'activation `core.hooksPath` — sera complétée en Task 6, ici juste le hook)

- [ ] **Step 1: Écrire `scripts/hooks/pre-push`**

```bash
#!/usr/bin/env bash
# Hook git pre-push KaSe — bloque le push si le tripwire est rouge.
# Activation: git config core.hooksPath scripts/hooks
# Échappatoire WIP: git push --no-verify
set -uo pipefail
REPO="$(git rev-parse --show-toplevel)"
echo "[pre-push] check.sh complet (6 boards)…"
if [ -z "${IDF_PATH:-}" ] && [ -f "$HOME/esp/esp-idf/export.sh" ]; then
  source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1 || true
fi
"$REPO/scripts/check.sh"
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "[pre-push] ROUGE — push bloqué. (git push --no-verify pour forcer un WIP)" >&2
fi
exit "$rc"
```

- [ ] **Step 2: Activer hooksPath et rendre exécutable**

Run:
```bash
chmod +x scripts/hooks/pre-push && git config core.hooksPath scripts/hooks
git config --get core.hooksPath
```
Expected: affiche `scripts/hooks`.

- [ ] **Step 3: Vérifier que le hook se déclenche (dry, sans vrai push)**

Run:
```bash
git push --dry-run 2>&1 | head -5
```
Expected: la ligne `[pre-push] check.sh complet (6 boards)…` apparaît (le hook s'exécute). Si pas d'ESP-IDF, le build échoue → push bloqué : c'est le comportement attendu. Pour ce test, vérifier seulement que le hook **se lance**.

- [ ] **Step 4: Commit**

```bash
git add scripts/hooks/pre-push
git commit --no-verify -m "feat(ci): hook git pre-push via core.hooksPath — bloque push si check rouge"
```
Note: `--no-verify` ici car on n'a pas forcément ESP-IDF sourcé pendant l'implémentation.

---

## Task 3: Hooks Claude Code (`PostToolUse` + `Stop`)

**Files:**
- Create: `.kase-board`
- Create: `scripts/hooks/cc_post_edit.sh`
- Create: `scripts/hooks/cc_stop.sh`
- Create: `.claude/settings.json`

- [ ] **Step 1: Créer le marqueur de board courant**

```bash
echo "kase_v2_debug" > .kase-board
```

- [ ] **Step 2: Écrire `scripts/hooks/cc_post_edit.sh`**

Lit le JSON du hook sur stdin, extrait `tool_input.file_path`, et lance `check.sh --host-only` seulement si le fichier est du code surveillé (`main/**/*.c`, `boards/**`, `test/**`). Exit 2 + stderr → remonte l'échec à Claude.

```bash
#!/usr/bin/env bash
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
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
```

- [ ] **Step 3: Écrire `scripts/hooks/cc_stop.sh`**

```bash
#!/usr/bin/env bash
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
BOARD="$(cat .kase-board 2>/dev/null || echo kase_v2_debug)"
if [ -z "${IDF_PATH:-}" ] && [ -f "$HOME/esp/esp-idf/export.sh" ]; then
  source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1 || true
fi
OUT="$("$REPO/scripts/check.sh" --board "$BOARD" 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "check.sh (board $BOARD) est ROUGE avant de conclure :" >&2
  echo "$OUT" | tail -10 >&2
  exit 2
fi
exit 0
```

- [ ] **Step 4: Écrire `.claude/settings.json`**

```json
{
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Edit|Write|MultiEdit",
        "hooks": [
          { "type": "command", "command": "$CLAUDE_PROJECT_DIR/scripts/hooks/cc_post_edit.sh" }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          { "type": "command", "command": "$CLAUDE_PROJECT_DIR/scripts/hooks/cc_stop.sh" }
        ]
      }
    ]
  }
}
```

- [ ] **Step 5: Rendre exécutables et tester le dispatcher PostToolUse**

Test fichier surveillé (doit lancer les tests) :
```bash
chmod +x scripts/hooks/cc_post_edit.sh scripts/hooks/cc_stop.sh
echo '{"tool_input":{"file_path":"/x/main/input/keymap.c"}}' | ./scripts/hooks/cc_post_edit.sh; echo "exit=$?"
```
Expected: tests host tournent, `exit=0` (suite verte).

Test fichier non surveillé (doit être ignoré, instantané) :
```bash
echo '{"tool_input":{"file_path":"/x/docs/README.md"}}' | ./scripts/hooks/cc_post_edit.sh; echo "exit=$?"
```
Expected: pas de tests lancés, `exit=0`.

- [ ] **Step 6: Vérifier la remontée d'erreur du dispatcher**

```bash
cp test/test_keycodes.c /tmp/tk.bak
sed -i '0,/ASSERT_EQ(/s//ASSERT_EQ(999+/' test/test_keycodes.c
echo '{"tool_input":{"file_path":"/x/test/test_keycodes.c"}}' | ./scripts/hooks/cc_post_edit.sh; echo "exit=$?"
mv /tmp/tk.bak test/test_keycodes.c
```
Expected: `exit=2`, message `Régression tests host…` sur stderr.

- [ ] **Step 7: Commit**

```bash
git add .kase-board scripts/hooks/cc_post_edit.sh scripts/hooks/cc_stop.sh .claude/settings.json
git commit --no-verify -m "feat(ci): hooks Claude Code — PostToolUse tests host, Stop build board courant"
```

---

## Task 4: CI étendue — 6 boards, toutes branches

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Mettre à jour le déclenchement et la matrice**

Remplacer le bloc `on:` et le job `build-firmware` par :

```yaml
on:
  push:
    branches: ['**']
  pull_request:

jobs:
  host-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build and run unit tests
        run: |
          cmake -S test -B test/build
          cmake --build test/build
          ./test/build/test_runner

  build-firmware:
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        board: [kase_v1, kase_v2, kase_v2_debug, kase_dongle, kase_half_left, kase_half_right]
    steps:
      - uses: actions/checkout@v4
      - name: Build ${{ matrix.board }}
        uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.5.1
          target: esp32s3
          command: idf.py -B build_${{ matrix.board }} -DBOARD=${{ matrix.board }} -DSDKCONFIG=build_${{ matrix.board }}/sdkconfig build
      - name: Upload firmware binary
        uses: actions/upload-artifact@v4
        with:
          name: firmware-${{ matrix.board }}
          path: build_${{ matrix.board }}/KeSp.bin
```

Conserver le job `release` existant mais corriger les chemins d'artefacts si besoin (les `KeSp.bin` viennent maintenant de `build_<board>/`). Mettre à jour le bloc `Prepare release binaries` pour mapper les 6 artefacts.

- [ ] **Step 2: Valider la syntaxe YAML localement**

Run:
```bash
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml')); print('YAML OK')"
```
Expected: `YAML OK`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit --no-verify -m "ci: matrice 6 boards + toutes branches + sdkconfig isolé"
```

Note: la validation réelle se fait au prochain push (le job tourne sur GitHub Actions). Sur `main` sans les boards dongle/half, ces entrées de matrice échoueraient — c'est pourquoi cette extension de matrice doit être mergée **dans le même PR** que les boards dongle/half (cf. spec §3).

---

## Task 5: Checklist smoke-test hardware

**Files:**
- Create: `docs/HARDWARE_SMOKE_TEST.md`

- [ ] **Step 1: Écrire la checklist (une page)**

```markdown
# Smoke-test hardware KaSe

À cocher AVANT chaque release / merge vers `main`. Couvre le runtime que les
tests host ne peuvent pas attraper. Flasher le board, cocher, garder une trace
dans la PR/release.

## Commun (tous boards)
- [ ] Boot sans boot loop (pas de Guru Meditation en boucle)
- [ ] USB HID : toutes les touches tapent le bon keycode (layer base)
- [ ] Layers MO/TO/LT/MT : bascule et retour OK
- [ ] Scan : aucune touche fantôme, aucune touche morte
- [ ] NVS préservée après reflash app-only (keymaps/macros intacts)

## V1 (round display + LED)
- [ ] Écran rond GC9A01 affiche sans corruption
- [ ] LED strip : animation par défaut OK
- [ ] Tama : sprite s'affiche et réagit

## V2 / V2D (OLED I2C)
- [ ] OLED SSD1306 affiche le dashboard sans artefact
- [ ] V2D : COLS7/8 (GPIO21/4) scannent correctement

## Dongle
- [ ] Lien RF s'établit avec une half (pairing < 120s)
- [ ] NRF ne se wedge pas après 5 min (watchdog OK)
- [ ] set_id survit à un erase_flash

## Half (left / right)
- [ ] e-ink affiche le splash 'PAIRED' au pairing
- [ ] Dashboard e-ink : L/R/USB + batterie, sans corruption
- [ ] Trackpad (si présent) : curseur, clic G/D/M, scroll
- [ ] BLE pairing OK + reconnexion après deep sleep

## BLE (boards concernés)
- [ ] Appairage host OK, tape sans drop pendant 1 min
- [ ] Bascule de slot BT OK
```

- [ ] **Step 2: Commit**

```bash
git add docs/HARDWARE_SMOKE_TEST.md
git commit --no-verify -m "docs: checklist smoke-test hardware (runtime non couvert par tests host)"
```

---

## Task 6: CLAUDE.md — workflow, norme TDD, déclencheurs agents

**Files:**
- Modify: `CLAUDE.md`
- Modify: `.gitignore` (vérifier `build_*/` et `.kase-board`)

- [ ] **Step 1: Vérifier/compléter `.gitignore`**

```bash
grep -q '^build_' .gitignore || echo 'build_*/' >> .gitignore
grep -q '^\.kase-board' .gitignore && echo "WARN: .kase-board ignoré — il doit être committé" || true
```
Note: `.kase-board` DOIT être suivi (versionné), donc il ne doit PAS être dans `.gitignore`. `build_*/` DOIT être ignoré.

- [ ] **Step 2: Ajouter une section anti-régression à `CLAUDE.md`**

Insérer après la section `## Tests` :

```markdown
## Workflow anti-régression (OBLIGATOIRE)

Source unique de vérité : `scripts/check.sh`.
- `./scripts/check.sh --host-only` — tests host (~secondes)
- `./scripts/check.sh --board <name>` — host + build d'un board
- `./scripts/check.sh` — host + build des 6 boards (sdkconfig isolé par board)

**Activation des hooks git (une fois par clone)** :
```bash
git config core.hooksPath scripts/hooks
```
`pre-push` lance le check complet et bloque le push si rouge. WIP : `git push --no-verify`.

**Hooks Claude Code** (`.claude/settings.json`, automatiques) :
- `PostToolUse` sur édition de `.c/.h` dans `main/`, `boards/`, `test/` → tests host.
- `Stop` → host + build du board courant (lu dans `.kase-board`).
Changer de board courant : `echo kase_v1 > .kase-board`.

**Jamais** builder deux boards dans le même `build/` avec le `sdkconfig` racine
(fuite de config). Toujours `-B build_<board> -DSDKCONFIG=build_<board>/sdkconfig`.

### Norme TDD — nouvelle logique pure
Toute nouvelle fonction de logique pure (keymap, layers, combo, tap-hold,
parsing CDC, encoding keycodes…) : test host écrit **d'abord**, ajouté à
`test/CMakeLists.txt` + déclaré dans `test/test_main.c`. Invoquer l'agent
`kase-test-author`.

### Quand invoquer les agents kase-*
- `kase-firmware-debugger` → backtrace / boot loop / crash.
- `kase-test-author` → ajout de logique pure (cf. norme TDD).
- `kase-code-reviewer` → avant un merge / release.
Les autres (`cdc-protocol`, `board-variant`, `release-manager`, `maintainer`,
`security-auditor`) : à la demande ponctuelle.

### Avant un merge vers main / release
Dérouler `docs/HARDWARE_SMOKE_TEST.md` sur les boards concernés.
```

- [ ] **Step 3: Vérifier que CLAUDE.md reste cohérent**

```bash
grep -c "check.sh" CLAUDE.md
```
Expected: ≥ 3 (la section est bien insérée).

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md .gitignore
git commit --no-verify -m "docs: workflow anti-régression + norme TDD + déclencheurs agents dans CLAUDE.md"
```

---

## Vérification finale

- [ ] **Tout le tripwire est vert (avec ESP-IDF sourcé)**

```bash
source ~/esp/esp-idf/export.sh
./scripts/check.sh; echo "exit=$?"
```
Expected: les 6 boards build, tests host verts, `exit=0`. (Première exécution longue — chaque board build à froid.)

- [ ] **hooksPath actif**

```bash
git config --get core.hooksPath
```
Expected: `scripts/hooks`.

---

## Notes de risque

- **Premier `check.sh` full** : 6 builds à froid = long (10-20 min selon machine). Acceptable car amorti ensuite (builds incrémentaux par dossier `build_<board>`).
- **CI sur toutes les branches** : consomme des minutes runner sur chaque push. Si trop bruyant, restreindre `branches:` aux préfixes `feat/*, fix/*, *-firmware` (décision laissée à l'usage).
- **`build_*/` non versionnés** : les 6 dossiers build coexistent localement (espace disque). Normal.
