# A board is one folder — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a new keyboard compiles and boots from `boards/<name>/` alone — no edit under `main/`, nothing at the repository root, and `check.sh` builds it without being told.

**Architecture:** the board folder owns its `sdkconfig.defaults`, its pin tables (`BOARD_ROW_PINS` / `BOARD_COL_PINS` and an X-macro `BOARD_PINS(X)` listing every GPIO the board uses); the core reads those tables instead of a hard-coded 5×13 shape; one generic host contract test checks every board's pins; a template plus `scripts/new-board.sh` create a board; Kconfig symbols are named after features, not boards.

**Tech Stack:** ESP-IDF 5.5 CMake, host CMake tests (`test/`), bash. Oracles: `./scripts/check.sh --fast`, the 7-board build, and — for the tasks that must not change behaviour — the **byte-identical binary oracle**: `sha256` of each `build_<board>/KeSp.bin` with the 256-byte app descriptor at 0x20 masked (same as the translation of 2026-09-20).

**Spec:** `docs/ROADMAP_MAKE_YOUR_OWN.md`, section 1.

## Global Constraints

- Every source change comes with a test or a `COMPORTEMENTS.md` line (Stop hook).
- No `git push --no-verify`; commit messages in English.
- Tasks 1, 3, 4 and 6 must leave the 7 binaries byte-identical (masked hash). Task 2 changes code: bench proof on the left, the right and the V2D (typing, one key per row and column).
- Never `erase_flash` the halves (pairing in NVS). MAC-checked flashes only.
- Reference hashes are taken **before** the first task and kept in the scratchpad:
  ```bash
  for b in kase_v1 kase_v2 kase_v2_debug kase_dongle niphar_left niphar_right conchodytes; do
    python3 -c "import hashlib;d=bytearray(open('build_$b/KeSp.bin','rb').read());d[0x20:0x120]=b'\0'*0x100;d[-33:]=b'\0'*33;print(hashlib.sha256(d).hexdigest()[:16],'$b')"
  done > /tmp/ref_hashes.txt
  ```
  (build all 7 first with `./scripts/check.sh --force` inside the devshell). The same loop after a task must print the same lines.
  ⚠ Both masks are needed (learned in Task 1): the app descriptor carries the
  `git describe` string (`-dirty` appears as soon as the tree has uncommitted
  changes) and the ELF sha; the image's last 33 bytes are its checksum + SHA-256
  and follow the descriptor. Build the reference and the candidate from a
  regenerated `build_<board>/sdkconfig` (`rm` it first) — a stale one hides a
  default that stopped being read.
- [x] Task 1 done 2026-09-20: 7/7 identical, old mechanism vs new, regenerated sdkconfigs.
- [x] Task 2 done 2026-09-20: bench left/right/V2D (rows, columns, wake per row) OK — the right re-flashed and re-tested after a stash-built stale binary was caught by the Task 3 oracle.
- [x] Task 4 done 2026-09-20: KASE_SPLIT_MASTER / KASE_DEVICE_ROLE_SPLIT_SCANNER, sdkconfigs regenerated, 7/7 identical.
- [x] Task 3 done 2026-09-20: 7 units green, bites on a duplicated V2 column, binaries identical to a HEAD rebuild (right: `cmp` empty).

---

### Task 1: `sdkconfig.defaults` lives in the board folder

**Files:**
- Move: `sdkconfig.defaults.niphar_left` → `boards/niphar_left/sdkconfig.defaults`, idem `niphar_right`, `dongle` → `boards/kase_dongle/`, `v2_debug` → `boards/kase_v2_debug/`, `conchodytes` → `boards/conchodytes/`
- Create: `boards/kase_v1/sdkconfig.defaults`, `boards/kase_v2/sdkconfig.defaults` (empty but for a comment: "no per-board override — the root sdkconfig.defaults is the whole config")
- Modify: `CMakeLists.txt:16-27`, `scripts/build_release.sh:15-18,79-86`, `COMPORTEMENTS.md:346`, `docs/SECURITY_KEY.md:159`, `docs/OPENPGP_CARD.md:415`, `CLAUDE.md` (Build system section)

- [ ] **Step 1: move the files**
```bash
git mv sdkconfig.defaults.niphar_left boards/niphar_left/sdkconfig.defaults
git mv sdkconfig.defaults.niphar_right boards/niphar_right/sdkconfig.defaults
git mv sdkconfig.defaults.dongle boards/kase_dongle/sdkconfig.defaults
git mv sdkconfig.defaults.v2_debug boards/kase_v2_debug/sdkconfig.defaults
git mv sdkconfig.defaults.conchodytes boards/conchodytes/sdkconfig.defaults
printf '# No per-board override: the root sdkconfig.defaults is this board'"'"'s whole config.\n' > boards/kase_v1/sdkconfig.defaults
cp boards/kase_v1/sdkconfig.defaults boards/kase_v2/sdkconfig.defaults
```

- [ ] **Step 2: CMake reads `${BOARD_DIR}/sdkconfig.defaults`** — replace lines 16-27 of `CMakeLists.txt` with:
```cmake
# Per-board sdkconfig defaults: boards/<name>/sdkconfig.defaults on top of the
# root sdkconfig.defaults. The folder is the whole registration of a board.
set(_PER_BOARD_DEFAULTS "${BOARD_DIR}/sdkconfig.defaults")
if(NOT EXISTS "${_PER_BOARD_DEFAULTS}")
    message(FATAL_ERROR "Board '${BOARD}': ${_PER_BOARD_DEFAULTS} is missing (create it, empty if the board needs no override)")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults" "${_PER_BOARD_DEFAULTS}")
message(STATUS "Board defaults: ${_PER_BOARD_DEFAULTS}")
```
and delete the `_BOARD_SHORT` regex line.

- [ ] **Step 3: fix the references** — `build_release.sh`: the two comments and the error text say `boards/kase_v2_debug/sdkconfig.defaults`; `COMPORTEMENTS.md:346`, `docs/SECURITY_KEY.md:159`, `docs/OPENPGP_CARD.md:415`: same path form; `CLAUDE.md` Build system: add one line "each board's overrides live in `boards/<name>/sdkconfig.defaults`, loaded on top of the root file".

- [ ] **Step 4: prove the defaults are really read** — regenerate every board's sdkconfig from scratch, rebuild, compare hashes:
```bash
for b in kase_v1 kase_v2 kase_v2_debug kase_dongle niphar_left niphar_right conchodytes; do
  rm -f build_$b/sdkconfig
  idf.py -B build_$b -DBOARD=$b -DSDKCONFIG=build_$b/sdkconfig build > /tmp/b_$b.log 2>&1 || { echo FAIL $b; tail -20 /tmp/b_$b.log; }
done
# same hash loop as in Global Constraints → diff against /tmp/ref_hashes.txt: no difference allowed
```
Expected: 7 identical lines. A difference means a default was not picked up (the exact failure this task exists to make impossible).

- [ ] **Step 5: `./scripts/check.sh --fast` green, commit**
```bash
git add -A && git commit -m "build: a board's sdkconfig.defaults lives in its folder — CMake loads boards/<name>/sdkconfig.defaults, root overrides removed (binaries byte-identical)"
```

---

### Task 2: pin tables in `board.h`, the core stops assuming 5×13

**Files:**
- Modify: `boards/kase_v1/board.h`, `boards/kase_v2/board.h`, `boards/kase_v2_debug/board.h`, `boards/niphar_left/board.h`, `boards/niphar_right/board.h` (add the tables; on the Niphargus boards delete the padding `COLS7..COLS12`/`ROWS4` macros that only exist for the hard-coded initializer, see `boards/niphar_left/board.h:40-58`)
- Modify: `main/input/matrix_scan.c:331,362-363,382,424-425`, `main/power/veille.c:47-48`
- Test: `test/test_matrix_constants.c`

**Interfaces:**
- Produces, in every board with a matrix:
  ```c
  #define BOARD_ROW_PINS { ROWS0, ROWS1, ROWS2, ROWS3 }            /* MATRIX_ROWS entries */
  #define BOARD_COL_PINS { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6 }  /* MATRIX_COLS entries */
  ```

- [ ] **Step 1: failing test** — in `test/test_matrix_constants.c` add, for the left and the V2 headers already included there (or include them the way the pin tests do):
```c
static void test_board_pin_tables_match_the_geometry(void)
{
    static const int rows[] = BOARD_ROW_PINS;
    static const int cols[] = BOARD_COL_PINS;
    TEST_ASSERT_EQ(sizeof rows / sizeof rows[0], MATRIX_ROWS, "BOARD_ROW_PINS has MATRIX_ROWS entries");
    TEST_ASSERT_EQ(sizeof cols / sizeof cols[0], MATRIX_COLS, "BOARD_COL_PINS has MATRIX_COLS entries");
}
```
Run `./scripts/check.sh --fast` → red: `BOARD_ROW_PINS` undeclared.

- [ ] **Step 2: add the tables to the five boards** (values = the existing `ROWSn`/`COLSn`, in order). On `niphar_left`/`niphar_right`, remove the padding macros and their comment block.

- [ ] **Step 3: the core uses the tables** — `matrix_scan.c` and `veille.c`: every `{ COLS0, …, COLS12 }` / `{ ROWS0, …, ROWS4 }` initializer becomes `BOARD_COL_PINS` / `BOARD_ROW_PINS`; loops already bound on `MATRIX_ROWS/COLS` stay as they are. Grep afterwards: `grep -rn "COLS12\|ROWS4" main/` must return nothing.

- [ ] **Step 4: green + build the 7 boards**
```bash
./scripts/check.sh --fast && ./scripts/check.sh --force
```
Binaries WILL differ (initializer size): that is expected for this task only.

- [ ] **Step 5: bench** — flash left, right (FTDI, MAC-checked) and the V2D (CDC DFU → ROM → esptool). Type one key per row and per column on each; on the halves also check the wake from light sleep by a key on each row (the row list feeds `veille.c`'s wake mask). Record the result in the commit.

- [ ] **Step 6: contract + commit** — `COMPORTEMENTS.md` (Input section): "[test:test_board_pin_tables_match_the_geometry] Matrix pins come from the board's `BOARD_ROW_PINS`/`BOARD_COL_PINS` tables; the core has no fixed matrix shape (the 5×13 padding of the Niphargus boards is gone)."
```bash
git commit -am "input: matrix pins from the board's BOARD_ROW_PINS/BOARD_COL_PINS tables — no 5x13 shape in the core, Niphargus padding macros removed (bench: left, right, V2D)"
```

---

### Task 3: a generic board contract test, one per board

**Files:**
- Create: `test/board_contract.inc` (the checks), `test/test_board_contract_kase_v1.c`, `…_kase_v2.c`, `…_kase_v2_debug.c`, `…_kase_dongle.c`, `…_niphar_left.c`, `…_niphar_right.c`, `…_conchodytes.c` (each: the host GPIO stubs, `#include "../boards/<name>/board.h"`, `#define BOARD_CONTRACT_NAME "<name>"`, `#include "board_contract.inc"`)
- Modify: every `boards/<name>/board.h` (add the X-macro), `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces:**
- Produces, in every `board.h`, the list of every GPIO the board drives or reads:
  ```c
  #define BOARD_PINS(X) \
      X(ROWS0) X(ROWS1) X(ROWS2) X(ROWS3) \
      X(COLS0) X(COLS1) X(COLS2) X(COLS3) X(COLS4) X(COLS5) X(COLS6) \
      X(BOARD_NRF_SCK) X(BOARD_NRF_MISO) X(BOARD_NRF_MOSI) X(BOARD_NRF_CE) X(BOARD_NRF_CSN) X(BOARD_NRF_IRQ) \
      X(BOARD_LINK_TX) X(BOARD_LINK_RX) X(BOARD_LINK_5V_EN) X(BOARD_VBAT_SENSE_GPIO)
  ```
  and, where relevant, `#define BOARD_USES_NATIVE_USB 1` (GPIO19/20 reserved) and `#define BOARD_PSRAM_OCTAL 1` (GPIO35-37 reserved).

- [ ] **Step 1: write `test/board_contract.inc`**
```c
/* Included once per board by test_board_contract_<name>.c. Checks every board
 * the same way; board-specific facts stay in the board's own pin test. */
static int bc_pins[] = {
#define X(p) p,
    BOARD_PINS(X)
#undef X
};
#define BC_N (sizeof bc_pins / sizeof bc_pins[0])
static int bc_strapping(int g) { return g == 0 || g == 3 || g == 45 || g == 46; }
static void bc_no_duplicate(void)
{ for (unsigned i = 0; i < BC_N; i++) for (unsigned j = i + 1; j < BC_N; j++)
      TEST_ASSERT(bc_pins[i] != bc_pins[j], BOARD_CONTRACT_NAME ": a GPIO is used twice"); }
static void bc_no_strapping(void)
{ for (unsigned i = 0; i < BC_N; i++) TEST_ASSERT(!bc_strapping(bc_pins[i]), BOARD_CONTRACT_NAME ": strapping GPIO (0/3/45/46) in use"); }
static void bc_no_reserved(void)
{ for (unsigned i = 0; i < BC_N; i++) {
#ifdef BOARD_USES_NATIVE_USB
      TEST_ASSERT(bc_pins[i] != 19 && bc_pins[i] != 20, BOARD_CONTRACT_NAME ": GPIO19/20 are the native USB");
#endif
#ifdef BOARD_PSRAM_OCTAL
      TEST_ASSERT(!(bc_pins[i] >= 35 && bc_pins[i] <= 37), BOARD_CONTRACT_NAME ": GPIO35-37 belong to the octal PSRAM");
#endif
  } }
#ifdef MATRIX_ROWS
static void bc_geometry(void)
{ static const int r[] = BOARD_ROW_PINS; static const int c[] = BOARD_COL_PINS;
  TEST_ASSERT_EQ(sizeof r / sizeof r[0], MATRIX_ROWS, BOARD_CONTRACT_NAME ": BOARD_ROW_PINS vs MATRIX_ROWS");
  TEST_ASSERT_EQ(sizeof c / sizeof c[0], MATRIX_COLS, BOARD_CONTRACT_NAME ": BOARD_COL_PINS vs MATRIX_COLS");
  TEST_ASSERT(KEYMAP_COLS >= MATRIX_COLS, BOARD_CONTRACT_NAME ": KEYMAP_COLS covers the matrix"); }
#endif
void BOARD_CONTRACT_FN(void)
{ TEST_SUITE("board contract: " BOARD_CONTRACT_NAME);
  TEST_RUN(bc_no_duplicate); TEST_RUN(bc_no_strapping); TEST_RUN(bc_no_reserved);
#ifdef MATRIX_ROWS
  TEST_RUN(bc_geometry);
#endif
}
```
Each `test_board_contract_<name>.c` defines `BOARD_CONTRACT_FN test_board_contract_<name>` before the include; `test_main.c` declares and calls the seven.

- [ ] **Step 2: add `BOARD_PINS(X)` (and the two reserved-pin flags) to the seven `board.h`**; the dongle and the mouse list their radio/click pins, no matrix.

- [ ] **Step 3: red → green → it bites** — first run is red (`BOARD_PINS` undefined) until step 2; then green; then temporarily change `COLS1` to the value of `COLS0` in `boards/kase_v2/board.h` → `bc_no_duplicate` red → revert. Mention this in the commit.

- [ ] **Step 4: binaries** — the X-macro is unused by firmware code: the 7 masked hashes must match the reference from Task 2's build (take new reference hashes after Task 2).

- [ ] **Step 5: contract + commit** — `COMPORTEMENTS.md`: "[test:test_board_contract_niphar_left] Every board passes the same pin contract from its `BOARD_PINS(X)` list: no GPIO twice, no strapping pin, no native-USB or PSRAM pin when the board uses them, tables consistent with the geometry (one test unit per board; the Niphargus tests keep their hardware-specific facts)."

---

### Task 4: Kconfig symbols named after features, not boards

**Files:**
- Modify: `main/Kconfig.projbuild` (`KASE_NIPHAR_MASTER` → `KASE_SPLIT_MASTER`, `KASE_DEVICE_ROLE_NIPHAR_SLAVE` → `KASE_DEVICE_ROLE_SPLIT_SCANNER`, prompts/help accordingly), every `boards/*/sdkconfig.defaults`, every `main/**` and `test/**` use (`grep -rn "NIPHAR_MASTER\|NIPHAR_SLAVE" main boards test docs CLAUDE.md COMPORTEMENTS.md`), `main/CMakeLists.txt` if it tests them.

- [ ] **Step 1: rename everywhere in one commit** (sed over the grep list; read each hit before applying — help texts describing the Niphargus stay factual, the symbol changes).
- [ ] **Step 2: regenerate every `build_<board>/sdkconfig`** (`rm` then build, as in Task 1 step 4) — a stale sdkconfig would keep the old symbol and hide a missed rename.
- [ ] **Step 3: masked hashes identical to the Task 3 reference; `check.sh --fast` green.**
- [ ] **Step 4: commit** — "kconfig: KASE_SPLIT_MASTER / KASE_DEVICE_ROLE_SPLIT_SCANNER — features, not board names (binaries byte-identical)". `COMPORTEMENTS.md`: one line in the Fusion section noting the rename and the oracle.

---

### Task 5: template, `new-board.sh`, boards discovered — not listed

**Files:**
- Create: `boards/_template/board.h` (every macro of a full board — matrix tables, `BOARD_PINS(X)`, radio, optional screen/battery/link/trackpad blocks each under a `/* --- optional: … --- */` banner with the Kconfig symbol that enables it), `boards/_template/board_keymap.c` (one layer, `K_NO` everywhere but a comment showing the syntax), `boards/_template/board_layout.c`, `boards/_template/sdkconfig.defaults` (commented feature switches), `boards/_template/README.md` (the four files, what to fill first)
- Create: `scripts/new-board.sh`
- Modify: `scripts/check.sh:36` (`ALL_VARIANTS` derived from the folders), `.tripwire-divergences` (declare it), `.esp-dev.yml` (`boards:` derived or documented), `CMakeLists.txt` (refuse `BOARD=_template`)

- [ ] **Step 1: `scripts/new-board.sh`**
```bash
#!/usr/bin/env bash
# Create boards/<name>/ from boards/_template/. The folder is the registration:
# check.sh, CMake and the release script discover boards from boards/*/sdkconfig.defaults.
set -euo pipefail
name="${1:?usage: new-board.sh <name>  (lowercase, digits, underscore)}"
[[ "$name" =~ ^[a-z][a-z0-9_]*$ ]] || { echo "bad name: $name" >&2; exit 2; }
root="$(cd "$(dirname "$0")/.." && pwd)"
dst="$root/boards/$name"
[ -e "$dst" ] && { echo "$dst exists" >&2; exit 1; }
cp -r "$root/boards/_template" "$dst"
sed -i "s/__BOARD_NAME__/$name/g" "$dst"/*
echo "created $dst — fill board.h (pins, BOARD_PINS), then: idf.py -B build_$name -DBOARD=$name -DSDKCONFIG=build_$name/sdkconfig build"
```
- [ ] **Step 2: `check.sh` discovers boards** — replace the literal `ALL_VARIANTS=(…)` with
```bash
mapfile -t ALL_VARIANTS < <(for d in boards/*/sdkconfig.defaults; do d="${d#boards/}"; d="${d%/sdkconfig.defaults}"; [ "$d" != "_template" ] && echo "$d"; done)
```
and add to `.tripwire-divergences`: `scripts/check.sh<TAB>mapfile -t ALL_VARIANTS<TAB>boards are discovered from boards/*/sdkconfig.defaults — a new board folder is its own registration (plan 2026-09-20 board-is-one-folder)`.
- [ ] **Step 3: CMake refuses `_template`** — after the `BOARD_DIR` check: `if(BOARD STREQUAL "_template") message(FATAL_ERROR "copy the template with scripts/new-board.sh <name>") endif()`.
- [ ] **Step 4: prove it** — `scripts/new-board.sh demo_board`, fill `BOARD_PINS` with the V2 pins, then `./scripts/check.sh --force` must build **8** boards and the contract test must run for… note: the host contract test is per-file (Task 3), so `new-board.sh` also appends `test/test_board_contract_<name>.c` + the two lines in `test/CMakeLists.txt`/`test_main.c` (add this to the script in step 1). Then `rm -rf boards/demo_board test/test_board_contract_demo_board.c` and revert the two test registrations; `check.sh` back to 7.
- [ ] **Step 5: commit** — "boards: _template + scripts/new-board.sh; check.sh discovers boards from their folder (declared divergence)". `COMPORTEMENTS.md`: "[smoke:New board from the template] `scripts/new-board.sh demo` produces a folder that `check.sh` builds and contract-tests without any other edit" + the matching item in `docs/HARDWARE_SMOKE_TEST.md` (done once on 2026-…).

---

### Task 6: the folder's README and the doc pointers

**Files:**
- Create: `boards/README.md` — "A board is one folder": the files and what each owns, `BOARD_PINS(X)`/tables, the optional feature blocks and their Kconfig symbols, `new-board.sh`, how the contract test protects it, out-of-tree boards = roadmap item 2.
- Modify: `CLAUDE.md` (Board variants + Build system: point to `boards/README.md`, remove the root-defaults wording), `docs/ROADMAP_MAKE_YOUR_OWN.md` (item 1 → done, date, commit range).

- [ ] **Step 1: write, commit** — "docs: boards/README.md — a board is one folder; roadmap item 1 done".

---

## Self-review

- Spec coverage: defaults in the folder (T1), template + script (T5), generic contract test (T3), core knows no board (T2 removes the 5×13 shape, T4 the board-named symbols), `check.sh` builds a new folder unasked (T5). Done-when of the roadmap is T5 step 4.
- Placeholders: none — every step names files and commands.
- Type consistency: `BOARD_ROW_PINS`/`BOARD_COL_PINS` (T2) are what T3's `bc_geometry` reads; `BOARD_PINS(X)` (T3) is what the template (T5) ships; `BOARD_CONTRACT_FN`/`BOARD_CONTRACT_NAME` are defined by each per-board test unit.
- Ordering: T2 must precede T3 (geometry check needs the tables); T4 is independent and can run any time after T1.
