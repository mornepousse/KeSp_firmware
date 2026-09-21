# Roadmap — from a personal firmware to "make your own keyboard"

Decided on 2026-09-20: all four workstreams below are wanted, in this order.
None is started. Each has a "done when" so it can be picked up cold.

The core (keymap engine, fusion at the dongle, radio owner, sleep, CDC +
KeSp_controller) is already board-agnostic. What is missing is the boundary
between core and board, and the front door for a newcomer.

## 1. A board is one folder — nothing else to touch

**Done 2026-09-20** (commits 27385978..HEAD, plan
`docs/superpowers/plans/2026-09-20-board-is-one-folder.md`): see
`boards/README.md`. Learned on the way: a KEYBOARD-role board could not say
"no screen" (`KASE_NO_DISPLAY` added) and CMake picked the display backend
from any mention of its name (now a real `#define`).

- Move `sdkconfig.defaults.<name>` from the repository root into
  `boards/<name>/`; CMake finds it from `-DBOARD=<name>`.
- `boards/_template/`: `board.h` commented field by field (matrix pins and
  diode direction, radio pins, optional screen/battery/link pins), a
  minimal `board_keymap.c`, `board_layout.c`, `sdkconfig.defaults`.
- `scripts/new-board.sh <name>`: copies the template, registers nothing
  else (the folder is the registration).
- A generic board contract test (pins unique, no strapping pin in the
  matrix, `MATRIX_ROWS/COLS` consistent with the tables) — the Niphargus
  pin tests already do this; make the checks table-driven.
- The core knows no board: every `#if CONFIG_KASE_…` that encodes a
  particular board's case moves into that board's defaults; Kconfig
  symbols are named after *features* (split, radio, screen kind, BLE,
  battery), not after history.
- Done when: a new board compiles and boots from its folder alone, with no
  edit under `main/`, and `check.sh` builds it without being told about it.

## 2. The board out of the tree (QMK userspace / ZMK config-repo model)

**`-DBOARD_DIR` done 2026-09-20** (`boards/README.md`): a template board created
in `/tmp` built from its folder alone. Left for item 3: the release CI building
such a board.

- `-DBOARD_DIR=/path/to/my_board`: the user keeps board + keymap in their
  own repository; this firmware is a dependency they pin (submodule or
  `idf_component.yml` git source).
- **Done 2026-09-21: the forkable template repository**
  https://github.com/mornepousse/kesp-keyboard-template on the reusable
  `.github/workflows/build-board.yml` (this repository, `workflow_call`).
  **First real out-of-tree board (2026-09-21): Conchodytes** —
  `mornepousse/Conchodytes-firmware`, the PCB repository stays hardware-only
  and links to it; the board, its pin test and the CI left this repository,
  the `MOUSE` role code stays.
  **Tried and declined the same day: moving the author's keyboard boards out
  too** (Niphargus → `Niphargus-firmware`, KaSe → `KaSe-firmware`, a
  "downstreams" CI job). A dry run of `Niphargus-firmware` built both halves
  byte-identical out of tree — the mechanism is fine — but the boards are not
  separable the way the split assumed: `kase_dongle` includes the left half's
  `board_keymap.c` and layout *by design* (one keymap source, never two that
  could diverge), and `kase_v2_debug` inherits from `kase_v2`. Going through
  meant a core with no real board, three satellite repositories, a bench
  spanning two checkouts and two commits per core+board change — for one
  person, and for no gain to a newcomer, who has the template. Decision
  (2026-09-21): **the template is for other people's keyboards, the author's
  boards stay in the core** as its reference boards; Conchodytes stays out
  because it is another product. Not to be reopened without a second
  maintainer. **Conchodytes goes further (same day): a dedicated firmware**
  (Rust `esp-hal`, bare metal — its reasons are a small self-contained
  project, not performance, which the 1 ms USB poll of the dongle bounds
  anyway), sharing only the slot-2 radio contract, now written down:
  `docs/DONGLE_MOUSE_CONTRACT.md`. The `MOUSE` role stays in KeSp until that
  firmware pairs and clicks.
  Original note:
  — `kesp-keyboard-template`: one example board (the template filled in), this
  firmware as a submodule pinned on a release, a GitHub workflow calling a
  reusable `workflow_call` workflow of this repository to build and publish
  that board's binaries, a ten-line README ("Use this template → rename →
  fill board.h → push → download"). The visible signal that anyone can make
  their keyboard, without splitting the author's boards out of the core (the
  7-board pre-push stays). Estimated one day: ½ reusable workflow, ½ template
  repository + docs. Splitting Niphargus/KaSe/Conchodytes into their own
  repositories was costed at 2-3 days plus a permanent cross-repo tax and
  declined for now.
- Later, only if there is demand: the core published as an ESP-IDF
  component (registry); the user's repo holds nothing but the board.
- Done when: a board living outside the repository builds and flashes with
  the documented command, and the release CI can build it too.

## 3. CI on GitHub Actions

**Written 2026-09-20** (`.github/workflows/ci.yml`): host tests + contract,
7-board matrix, an out-of-tree template board, release on tag (14 artefacts,
pre-release on a suffixed tag, embedded version checked against the tag).
The old GitLab-mirror release job was removed (it clobbered the notes of
v4.2.0-beta.1 with `null` while waiting for a GitLab release that no longer
exists). First green run on `b5ae1d1c` (host tests, 7 boards, out-of-tree
board). What it took: no `path:` in `dependencies.lock` (local components are
project components), `IDF_COMPONENT_CHECK_NEW_VERSION=0` inside the container
command, the out-of-tree board inside the checkout (`.ci_boards/`) and a
`FORCE` on the `BOARD_DIR` cache entry. **First tag through the release job:
`v4.2.0-beta.2` on 2026-09-21** — 14 artefacts, pre-release, embedded version
checked. One transient 403 on an artefact upload (GitHub side): `gh run rerun
--failed` re-ran the one build and the release job. Item done.

- Build matrix of the 7 boards on every push and PR (ESP-IDF 5.5 docker
  image; ccache keyed on the lock file).
- On a `v*` tag: build the 14 artefacts (`scripts/build_release.sh`) and
  create the GitHub release (pre-release if the tag has a `-beta.N`
  suffix, Latest otherwise) — replaces the local `gh release create`.
- The local pre-push stays the author's safety net; the CI is the
  contributors'.
- Done when: a PR from a fork shows the 7 builds green or red, and a tag
  publishes without a human running a script.

## 4. The front door

**Document done 2026-09-20**: `docs/MAKE_YOUR_OWN.md` (hardware, toolchain,
the board folder, flash, pairing, remapping, reading the heartbeat and
`rfstat`), plus the two bench tools promoted to `scripts/` (`kesp_cdc.py`,
`console-capture.py`). Left: the name.

- `docs/MAKE_YOUR_OWN.md`: hardware requirements (ESP32-S3, nRF24L01+,
  COL → switch → diode → ROW for key wake-up, optional Sharp memory-LCD,
  battery divider, TRRS), the four files of a board, build and flash
  (plain ESP-IDF, no Nix required), pairing with the dongle, remapping with
  KeSp_controller, what the heartbeat and `rfstat` tell you.
- **Decided 2026-09-20 (option 1)**: clarify, don't rename yet — README says
  KeSp is the firmware, KaSe/Niphargus/Conchodytes are boards, `KASE_*` is a
  historical prefix. The rename to `KESP_*` (Kconfig prefix, release names,
  KeSp_controller aligned) is deferred to a major version.
- One name. Today KaSe / KeSp / Niphargus / Conchodytes are four names for
  one project — and "kase" is used everywhere for the firmware (`KASE_*`
  Kconfig, `kase_*` boards, `KaSe_` binaries) although KaSe is a keyboard and
  KeSp the firmware (noticed 2026-09-20). Pick a firmware name, keep board
  names for boards, rename the repository, the Kconfig prefix and the
  binaries accordingly — a bulk rename, to plan on its own.
- Done when: someone who has never seen the repository follows the
  document to a typing board without asking a question.

## Not in this roadmap (ongoing, per module)

- French identifiers (`veille_*`, `radio_emettre`, French test names):
  renamed when a module is worked on, never in bulk (decision 2026-09-20).
- `scripts/check.sh` and hooks: generated by the tripwire scaffold, they
  follow its template.

## Known issues logged while executing (not part of the roadmap)

- 2026-09-20, V2D: the CDC `DFU` command (0x03) intermittently does not
  reboot the board — it answers, stays enumerated (same USB device number),
  and its CDC goes mute until a power cycle (1 success, 2 failures in one
  afternoon, build v4.1.0-195 and later). Old v4.0.0-76 rebooted on the first
  try. Suspects: `ks_respond_ok` blocking in the TinyUSB CDC write path before
  `reboot_to_dfu()`, or `esp_restart()` after `RTC_CNTL_FORCE_DOWNLOAD_BOOT`.
  Workaround: power-cycle and retry.

