# KaSe firmware — Claude Code instructions

ESP32-S3 firmware for a custom split-ergo keyboard (KaSe V1/V2/V2D).
Distributed via GitLab Releases binaries. Built with ESP-IDF 5.5.

## Language

Everything in the repository is in **English** since 2026-09-20: code comments,
Markdown docs, the behaviour contract, agent definitions, commit messages. The
firmware's log strings are still French and get translated in a separate pass
together with the docs that quote them (they change the binaries, comments
don't). Two deliberate exceptions: the historical plans and specs under
`docs/superpowers/` and the two July audits in `docs/` stay in French (they are
records of executed work, not operating docs), and identifiers were not renamed
by the translation (many function and variable names are still French —
`veille_veto_poser`, `radio_emettre`, `s_etat_mux` — rename per module, with
the docs and tests that quote them, never in bulk). The translation of the
comments was verified by rebuilding the 7 boards to byte-identical binaries
(app descriptor masked): a comment-only change must keep the line count of
its file, because `assert`/`__LINE__` bake line numbers into the image.

## Repo
- **Reference**: https://github.com/mornepousse/KeSp_firmware (remote `github`)
  — this is the repo of record.
- **Mirror**: https://gitlab.com/harrael/KeSp_firmware (remote `origin`).
  GitLab pushes to GitHub via a server-side mirror, verified on 2026-09-05:
  a single `git push origin` is enough, GitHub catches up within seconds. No
  need to also push to `github` — the second push loses the race against the
  mirror and gets rejected.
  ⚠ The mirror cannot rewind GitHub. If `github/<branch>` gets ahead via a
  direct push, the mirror gets stuck **silently** on that branch: it happened
  on `main`, which stayed two commits behind on the GitLab side. Realign by
  fast-forward, not by force.
- **Local**: `~/Documents/GitHub/KeSp_firmware-gitlab/`
- **Related**: https://gitlab.com/harrael/KeSp_controller (remapping software)

## Versioning

Source of truth: git tag `vX.Y.Z`. Read by ESP-IDF via `git describe --tags`
at build time. No VERSION file.

To cut a release: see **Release workflow** at the end of this file
(`/tripwire:release`; the CI builds and publishes on the tag, beta.N =
pre-release, stable = Latest once the smoke test passed everywhere).

Between two releases: `vX.Y.Z-N-gHASH[-dirty]` via `git describe` — in the
heartbeat and the CDC `VERSION` reply.

## Scope — old halves removed, Niphargus coming

The first-generation split halves (`kase_half_left` / `kase_half_right`,
e-ink SSD1681, ESP-NOW) were removed at commit `c107df77`: they targeted
hardware that no longer exists.

The split keyboard is being redesigned under the name **Niphargus** —
hardware in `~/Documents/GitHub/rili` (KiCad), **firmware here**. Two
ESP32-S3 halves + nRF24L01+, 4×7 matrix, Azoteq TPS43 trackpad on the left,
Sharp Memory LCD on BOTH halves (portrait 68 × 160), wired TRRS link. No
WiFi or BLE: config and updates over USB.

Architecture decided: the **left half is the master under all
circumstances** (it carries the only keymap engine and the trackpad); the
right is a scanner. The dongle keeps its radio 1 for the keyboard — radio 2
belongs to the **Conchodytes** mouse (`~/Documents/GitHub/Conchodytes`).

Full design: `docs/superpowers/specs/2026-08-19-niphargus-firmware-design.md`.

**Status as of 2026-09-06** — both halves work and type together: pinout
checked against the netlist on both, inter-half radio link proven (channel
0x4F, address KaSe.03), keymap fusion, relay to the dongle. Design risk R1
is cleared (0 loss, 0.4 retransmission/packet).
**Battery power fixed on 2026-09-07**: both halves run standalone, with no
cable at all, and type together over radio. This is the keyboard's nominal
mode.
**Full RF on 2026-09-07**: the left listens to the right in PRX on the link
channel AND relays the finished HID to the dongle, via a PRX→PTX→PRX
excursion. Routing stays USB-first — USB plugged in → HID over USB, on
battery → radio.
**Fusion by default since 2026-09-18**: `KASE_DONGLE_FUSION=y` in the
defaults of all three boards (left, right, dongle) — pre-push keeps what is
flashed. The pre-fusion path `HALF_LINK_RX` (the left listening to the right
directly) is removed. The `build_*_fusion` folders no longer exist:
binaries come out of `build_niphar_left`, `build_niphar_right`,
`build_kase_dongle`.

⚠ **One chip, one owner.** Each half's radio belongs to
`comm/rf/radio_owner.c` (2026-09-19) and to it alone: one mode at a time
(PTX toward a target, PRX listening to a target, off), a lock on every
transaction, the excursion that **drains the FIFO into the consumer before**
leaving, the wake-up that **re-arms** the mode, the bus loan to the screen.
`half_link.c` (the right) and `kbd_relay_tx.c` (the left) are policies: they
see neither `rf_driver` nor a mutex, they request `radio_mode_set`,
`radio_send`, `radio_excursion_tx`, `radio_pair_round`. The owner talks to
the hardware through a table of operations: the invariants are **tested
host-side against a fake recorder** (`test_radio_owner`, the call sequence
is the oracle). History: two modules that each initialized the chip on
their own side made the left listen on the wrong channel **three times**,
silently (`rf_claim_chip` now says so out loud); and the refactor revealed
that the left's USB listening was leaving on the address derived from
set_id and was only "corrected" by the restore of the first excursion — the
owner restores faithfully, so the target must be right from the start
(`'KaSe'.03`). Any new chip config write goes through
`radio_mode_set`/`radio_rearmer`, never through `rf_driver_*` from a
policy.

⚠ **"Emit on change" and "release on silence" don't compose.** This pairing
produced three distinct failures on 2026-09-08, at three links in the
chain. Whatever only emits on change goes quiet while a key is simply HELD;
whatever releases on silence then drops it. The one remedy: silent at rest,
kept alive as long as something is held. The constants that tie a
transmitter to its listener's patience live in `comm/rf/rf_slot.h`
(`RF_STATUS_PERIOD_MS`, `RF_LINK_LOST_MS`) — this is a contract between two
firmwares, not a number each one picks on its own.

⚠ **An ESB acknowledgment does not prove software reception.** The nRF24
answers on its own as soon as channel and address match. An excursion that
drained the FIFO on return was destroying packets already acknowledged: the
right was reading 100% success while the left was losing 5% of frames.
Drain the FIFO BEFORE transmitting, never after.

**B7 done on 2026-09-08** — hybrid sleep (`main/power/veille.c`): light
sleep after **15 s** (60 s until 2026-09-15: awake and idle the board draws
~28 mA at 160 MHz, table 5-9 p. 67, i.e. a hundred times the sleep draw — a
day of typing lost 0.2 V; ~244 µA asleep, state retained, ~1 ms wake-up),
deep sleep after 4 h (~12 µA, EXT1 wake-up, 704 ms restart). Thresholds
tunable via Kconfig — testing EXT1 with the 4 h default would mean waiting
four hours.

**Idle wake tamed on 2026-09-16** (`main/power/pm_dfs.c`, tickless): DFS
160/40 MHz, scanning stopped at rest (keyboard_button in power-saving
mode), task cadences at 100 ms at rest, stats disabled, and automatic light
sleep between keystrokes (`CONFIG_FREERTOS_USE_TICKLESS_IDLE`, ~9
sleeps/s). The bench HB (`CONFIG_PM_PROFILING=y`) prints
`esp_pm_dump_locks` — read `light_sleep_counts` before believing it's
sleeping.
**Sleep is ONE task** (`power/veille_task.c`, 2026-09-19), the same on both
halves: inactivity, named **vetoes** (usb, link, sync, test — a module sets
its own, like an esp_pm lock), sleep/wake **hooks** (radio, screen, gauge —
called in reverse order on wake, before capture), heartbeat
`HB … vetos=…`. Never re-evaluate sleep inside a module's own task; a new
blocker = a veto, a new peripheral = a hook. A module's wake hook must NOT
touch the SPI bus (the radio holds the lock until its own hook).
⚠ **Tickless: at 100 Hz a 10 ms loop leaves ONE free tick, sleep requires
THREE** (`FREERTOS_IDLE_TIME_BEFORE_SLEEP`). The 10 ms keyboard task was
reporting "mode SLEEP 92 %" with zero actual sleep — an idle mode is not a
sleep. Any new periodic wait < 30 ms on the halves kills tickless silently.
⚠ **Slowing down a tick changes what it drains.** The left relay's tick,
slowed to 100 ms at rest, caused right-half keystrokes to be lost in USB
mode: it's the one draining the nRF24 FIFO (3 frames) of frames re-sent by
the dongle, and press + release were falling in the same round. Before
slowing a cadence, list what the tick consumes, not just what it emits
(`kbd_relay_cadence_ms`, tested).
⚠ A "USB won't switch" turned out to be a charge-only cable: check `lsusb`
(cafe:4003) before blaming DFS — a cold plug-in enumerates.

⚠ **Deep sleep was NOT reachable** (fixed on 2026-09-15): inactivity is
only evaluated while awake, and the board stays stuck in light sleep until
a key press, which resets the counter to zero. A TIMER wake at the deep
threshold switches to deep sleep; a GPIO wake disarms it. And a night
losing 0.2 V (~20 mA) is indistinguishable from a night at 244 µA without a
number: every wake-up logs the duration slept, both halves carry a
heartbeat `idle=… slept=X s/n`. Read that before
reaching for the multimeter.

⚠ **The ULP is ruled out by measurement**: 170 µA on its own (ESP32-S3
datasheet v2.2, table 5-10, p. 68), against a 50 µA target. The design's
"RTC scan" cannot hold — and it's unnecessary: the COL → switch → diode →
ROW wiring lets the columns be held high and wakes on any row.

⚠ **The radio is turned off as early as the light stage** — listening
costs 13.1 mA (nRF24L01+ PS v1.0, table 4, p. 14) and the nRF24 has no
low-power listening mode. A sleeping half does NOT hear the other: after a
long absence, the first keystroke must be on the left. On wake, reception
must be RE-ARMED (`rf_driver_power_up` does not touch CE), otherwise the
left comes back powered but deaf.

⚠ **`rtc_gpio_hold_en()` survives a restart**, not just sleep. Without
`veille_liberer_gpio()` at boot, the columns stay frozen and the whole
matrix reads garbage — `gpio_reset_pin()` does not undo an RTC hold.

⚠ **ESP log timestamps are not real time**: they follow the FreeRTOS tick,
which stops during light sleep. The heartbeat's `up` counter, backed by
`esp_timer` and therefore the RTC, is the only reliable witness of a
sleep's duration.

**B2 wired TRRS link on 2026-09-11**: UART1 transport
(`comm/link/link_uart.c`), probe/ACK handshake and 5 V load switch control
— the link comes up, both switches close, it holds. ⚠ **Charging** the
other half is NOT validated, and it's electrical: the firmware only offers
5 V if a HOST enumerates (`tud_ready`), but charging happens on a wall
charger that doesn't enumerate. The real signal is VBUS, which requires the
GPIO33 bridge to be populated. `KASE_LINK_FORCE_SOURCE` forces the source
to test the electrical side at the bench.

⚠ **A report slot gets recycled: clear `keycodes[i]` at the top of every
slot.** `build_keycode_report` rebuilds `current_press` from scratch, but
five branches (layer key, held tap-hold, deferred combo, leader, advanced
keycode with no HID output) were setting `extra_keycodes[i]` without
resetting `keycodes[i]` to zero. This held up as long as a slot kept the
same key type — but remote fusion REPACKS the slots (`matrix_apply_remote`
compacts the remote side after the local boundary), and an absorbed key
would then inherit the keycode of the digit it replaced: the key would
repeat forever under a held remote MO. Locked down by
`test_kp_slot_recycle_ne_gele_pas_le_keycode`.

**Screens done on 2026-09-14** (`main/display/memlcd/`): Sharp LS011B7DH03
(nice!view module) on both halves, SPI shared with the nRF24 (CS GPIO14
active-high, borrowed via `rf_bus_lock`). Status bar (route, ▲ dongle seen,
gauge), layer as rows of 4 on the left, generated Niphargus logo
(`scripts/gen_logo_memlcd.sh`) on the right. No "other half's battery"
(user decision; the ACK channel that would have carried it was removed).
⚠ The panel is **68 lines × 160 px** (Sharp catalog: "160 × 68", H = data
direction), portrait is a transposition; the **command word goes out RAW**
(M0 = first bit clocked MSB-first), only the line address goes through
rev8 — a write-only screen returns nothing, a wrong assumption produces
silence, not an error. Datasheets in lemia (docs 6844, 6845).
⚠ **Niphargus battery** (`main/power/batt_sense.c`, ADC2 GPIO13, 1M/1M):
the right reports a STATUS every 30 s; the DISPLAYED voltage is stabilized
over 30 s (a hysteresis around the displayed value once froze 4.2 V for a
whole night).
**Battery levels (2026-09-19)**: `batt_niveau_step` (pure, 0.1 V
hysteresis) — LOW < 3.5 V: thickened gauge border, no more 5 V for the
TRRS; CRITICAL < 3.3 V: light sleep at 5 s. No blinking, no forced shutdown
(the DW01A cuts at 2.5 V). Test at the bench by shifting `BATT_FAIBLE_DV` /
`BATT_CRITIQUE_DV` above the real voltage — without committing it.
**The dongle's engine replays every transition** (`comm/rf/fusion_file.h`,
2026-09-19): a queue of 8 fused states, no more "last state wins";
`transitions_ecrasees` now only counts overflow (0 in a minute of fast
typing, 536 per evening before). No more `[NON GARDÉ]` in the contract.
**A single USB presence rule** (`usb_presence_brut`) for routing, the 5 V
TRRS and the sleep veto: VBUS bridge if `KASE_VBUS_SENSE`, otherwise
`tud_ready`.

Open items (2026-09-19):
- the trackpad driver (hardware). Its pure logic — IQS5xx frame parser,
  gesture→HID mapping, accel config — exists and is tested
  (`periph/trackpad/`); missing: the I2C+RDY bring-up on the LEFT side and
  the hookup to the mouse relay;
- the **multimeter measurement** of each half (awake-idle expected
  ~1-3 mA after DFS + tickless, asleep ~250 µA, while typing) — all the
  battery-life work from the 16th to the 19th is proven by the logs, not
  yet measured;
- ~~the light first key lost after a long pause~~ — **solved 2026-09-21**:
  not the switch. After a light sleep the esp_timer task replays every
  missed period of a periodic timer (battery gauge 10 s, LVGL tick 50 ms)
  BEFORE the sleep task's first instruction — 80 ms+ after 11 minutes, the
  tap is over when the rows are read. Rule: **a periodic esp_timer on the
  halves is either created with `skip_unhandled_events` or stopped by a
  sleep hook**. `KASE_VEILLE_DIAG` (`lines at exit=…`) is how it was seen:
  0x0 after a long sleep, non-zero after a short one, same 14 ms to capture.
Pinout: `docs/NIPHARGUS_V2_HARDWARE.md` (source of truth, checked against
the netlist).

**Standardisation roadmap (2026-09-20)**: `docs/ROADMAP_MAKE_YOUR_OWN.md` —
board = one folder (**done 2026-09-20**, `boards/README.md`), out-of-tree
boards (`-DBOARD_DIR`, **done 2026-09-20**; forkable template repository
`mornepousse/kesp-keyboard-template` on the reusable `build-board.yml`,
**2026-09-21**), GitHub Actions CI + release on tag
(**`.github/workflows/ci.yml`, 2026-09-20**), a "make your own"
document (**`docs/MAKE_YOUR_OWN.md`, 2026-09-20**) and a single project name —
decided: KeSp is the firmware, KaSe/Niphargus/Conchodytes are boards, the
`KASE_*` prefix stays until a major-version rename (README says so).
**The author's boards stay in the core** (decided 2026-09-21 after a dry run
of `Niphargus-firmware`): the template repository is the path for *other
people's* keyboards; the Niphargus halves, the dongle and V2/V2D are this
firmware's reference boards — the dongle includes the left half's keymap by
design and V2D inherits from V2, so they do not split cleanly, and a split
would cost two checkouts and two commits per change for no newcomer benefit
(details: `docs/ROADMAP_MAKE_YOUR_OWN.md`, item 2).

## Board variants

- **V1**: round SPI display (GC9A01), LED strip, legacy pinout
- **V2**: I2C OLED (SSD1306), production pinout
- **V2D**: V2 + GPIO overrides for prototype (COLS7/8 on GPIO21/4 instead
  of UART0)
- **dongle**: USB receiver, two nRF24 radios (slot 1 keyboard, slot 2
  mouse), neither matrix nor keymap engine — it relays already-finished HID
- **niphar_left**: LEFT half of the Niphargus, the master. 4×7 matrix, the
  keyboard's only keymap engine, `KEYMAP_COLS = 14` to cover both halves,
  trackpad (driver to write), relay to the dongle
- **niphar_right**: RIGHT half, a scanner. 4×7 matrix with a pinout table
  DIFFERENT from the left (routing permutations), sends its half-matrix
  over radio, neither keymap nor HID. Its columns are **mirrored**
  relative to the left's (same PCB flipped over): the conversion happens
  at the master, via `BOARD_REMOTE_COLS_MIRRORED` and
  `half_col_to_keymap()`. It emits on change, and **reaffirms holds every
  100 ms** — since the scan callback only fires on change, a held key
  would otherwise produce nothing more and the left would release it
  after 250 ms
- **conchodytes**: mouse (PMW3389), dongle slot 2 — **out of the tree since
  2026-09-21**: `mornepousse/Conchodytes-firmware` (board + this firmware as a
  pinned submodule + CI on `build-board.yml`); the `MOUSE` role code stays here

Each board lives under `boards/<name>/` with `board.h`, `board_keymap.c`,
`board_layout.c` and `sdkconfig.defaults` — **a board is one folder**, see
`boards/README.md` (2026-09-20: `scripts/new-board.sh`, `boards/_template/`,
pin tables `BOARD_ROW_PINS`/`BOARD_COL_PINS` read by the core, `BOARD_PINS(X)`
checked by the generic contract test, boards discovered by `check.sh`). V2D
inherits from V2 via `#include "../kase_v2/board.h"`.

## Build system

```bash
source ~/esp/esp-idf/export.sh
idf.py -B build_kase_v1       -DBOARD=kase_v1       -DSDKCONFIG=build_kase_v1/sdkconfig       build
idf.py -B build_kase_v2       -DBOARD=kase_v2       -DSDKCONFIG=build_kase_v2/sdkconfig       build
idf.py -B build_kase_v2_debug -DBOARD=kase_v2_debug -DSDKCONFIG=build_kase_v2_debug/sdkconfig build
```

CMake parameter: `-DBOARD=<name>` (not `-DBOARD_VARIANT`). Each board has
its own build folder (`build_kase_<name>/`) **and its own `sdkconfig`** via
`-DSDKCONFIG=build_kase_<name>/sdkconfig` — this is what avoids config
leakage between boards (see Anti-regression workflow). 6 in-tree boards: V1,
V2, V2D, dongle, niphar_left, niphar_right (Conchodytes builds from its
own repository). To check
everything at once: `./scripts/check.sh`.

**ccache**: `check.sh` exports `IDF_CCACHE_ENABLE=1` — the boards share
most of their components, so after the 1st board the rest reuse the
compiled objects (big win on the full build + pre-push). For your
interactive builds, add `export IDF_CCACHE_ENABLE=1` to your shell (or
source it before `idf.py`). Stats: `ccache -s`.

**Component manager**: `check.sh` also exports
`IDF_COMPONENT_CHECK_NEW_VERSION=0`. Without it, the 2.2.2 manager
(2026-09-18) queries the registry on every configure and evaluates LVGL 9's
manifests against our LVGL 8 sdkconfig → `MissingKconfigError:
LV_USE_LIBJPEG_TURBO`, fatal. For a manual `idf.py` outside `check.sh`,
export the same variable. **To change `main/idf_component.yml`** (which forces
a re-solve of `dependencies.lock`), manager 2.2.2 cannot: use a newer one in a
throw-away venv layered on the devshell python —
`python3 -m venv --system-site-packages /tmp/icm && /tmp/icm/bin/pip install
idf-component-manager==2.5.2 && /tmp/icm/bin/python $IDF_PATH/tools/idf.py -B
build_<b> -DBOARD=<b> -DSDKCONFIG=build_<b>/sdkconfig reconfigure` — then READ
the lock diff: a re-solve moves every `^` dependency to its latest (2026-09-20:
TinyUSB 0.19 → 0.21 silently; now pinned `==0.19.0~3`, upgraded on purpose
only). The lock must contain no `path:` — a local component listed in the
manifest gets an absolute path of one machine and breaks every other checkout
(the CI did). Local components are plain project components under
`components/`, required by name from `main/CMakeLists.txt`.

**Per-board defaults**: `boards/<name>/sdkconfig.defaults` is loaded on top of
the root `sdkconfig.defaults` (CMake refuses a board without one — empty is
fine). Nothing at the repository root names a board: the folder is the whole
registration (2026-09-20, plan "a board is one folder").

**Important**: with `-DSDKCONFIG=build_kase_<name>/sdkconfig`, each board
has its sdkconfig isolated in its build folder — no more config leakage
between boards. The legacy `sdkconfig` at the root remains that of a
legacy build without `-DSDKCONFIG`; do not mix the two modes on the same
board.

## Flash

**App only** (NVS preserved):
```bash
idf.py -B build_v<N> -p /dev/ttyUSB0 flash
# ou: esptool.py write_flash 0x20000 build_v<N>/KeSp.bin
```

**Full flash** (erase + bootloader + partition table + app + storage):
```bash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 erase_flash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x0 kase_<board>_full.bin
```

Required after a partition table change (e.g. NVS resize).

## Partition table

`partitions.csv` — 16MB flash:
- `nvs`      : 0x9000  + 0x10000 (64KB) — config, keymaps, stats
- `otadata`  : 0x19000 + 0x2000
- `phy_init` : 0x1B000 + 0x1000
- `factory`  : 0x20000 + 0x200000 (2MB)
- `ota_0`    : 0x220000 + 0x200000 (2MB)
- `storage`  : 0x420000 + 0xF0000 (LittleFS)

NVS MUST stay at 64KB — stores ~21KB of bigrams + keymaps + macros + etc.
Do not shrink it without removing the bigrams first.

## Architecture

```
main/
├── main.c                # app_main, safe boot, task orchestration
├── comm/
│   ├── cdc/              # Binary protocol only (KS/KR frames, CRC-8)
│   │   ├── cdc_acm_com.c        # USB CDC dispatch
│   │   ├── cdc_binary_protocol.c # Frame parser, CRC
│   │   ├── cdc_binary_cmds.c    # All command handlers
│   │   └── cdc_ota.c            # OTA binary helpers
│   ├── rf/               # nRF24 — dongle relay, inter-half link
│   │   ├── rf_driver.c          # SPI + ESB, nRF24 registers (hardware)
│   │   ├── radio_owner.c        # the halves' chip: ONE owner (mode, lock, excursion, sleep)
│   │   ├── keymap_pull.c        # left: pulling the keymap from the dongle via ACK payload
│   │   ├── rf_packet.h          # frames + half-matrix geometry (4×7)
│   │   ├── rf_slot.h            # dongle slots + 2.4 GHz CHANNEL PLAN
│   │   ├── kbd_relay_tx.c       # the LEFT: raw → dongle, listens for the right's USB, keymap sync
│   │   ├── half_link.c          # the RIGHT: half-matrix → dongle, fallback to the left
│   │   └── rf_probe.c           # bench diagnostic (NRF_PROBE), line test
│   ├── ble/              # Bluetooth LE HID
│   │   └── hid_bluetooth_manager.c
│   ├── usb/              # USB HID + CDC TinyUSB init
│   │   └── usb_hid.c
│   └── hid_transport.c   # USB/BLE routing (usb_bl_state)
├── input/
│   ├── matrix_scan.c     # keyboard_button driver wrapper
│   ├── keyboard_task.c   # Main scan loop
│   ├── key_processor.c   # Keycode decoding, layers, advanced
│   ├── key_features.c    # OSM, OSL, caps_word, repeat, leader, etc.
│   ├── tap_hold.c tap_dance.c combo.c leader.c
│   ├── hid_report.c      # HID queue + sender task
│   └── keymap.c key_stats.c
├── display/
│   ├── display_backend.h # vtable for OLED/round/memlcd
│   ├── status_display.c  # Coordinator
│   ├── oled/             # I2C OLED (V2/V2D)
│   ├── round/            # SPI GC9A01 (V1)
│   ├── memlcd/           # Sharp memory-LCD for the Niphargus halves (68×160 portrait)
│   └── assets/           # LVGL images (Niphargus logo generated by scripts/gen_logo_memlcd.sh)
├── power/                # veille_task.c (ONE task: vetoes, hooks, HB), veille.c (B7 sequence),
│                         # cadence.h (all the cadences, _Static_assert), pm_dfs.c, batt_sense.c
└── led/                  # WS2812 strip anim (V1 only)

boards/
├── kase_v1/   kase_v2/   kase_v2_debug/   kase_dongle/
├── niphar_left/   niphar_right/   # Niphargus split (boards manufactured, in service)
└── kase_layout.inc  # Layout JSON shared V2/V2D
```

## CDC protocol — binary only, no ASCII

Frame format KS (request) / KR (response) with CRC-8:
```
KS: [0x4B][0x53][cmd:u8][len:u16 LE][payload...][crc8]
KR: [0x4B][0x52][cmd:u8][status:u8][len:u16 LE][payload...][crc8]
```

See `docs/CDC_BINARY_PROTOCOL.md` for the full doc and
`main/comm/cdc/cdc_binary_protocol.h` for the IDs (KS_CMD_*).

**Never add an ASCII command** — the text protocol was removed in v3.7.

## Keycodes (16-bit)

Encoding in `main/input/key_definitions.h`. Ranges:
- `0x00-0xFF` : HID standard
- `0x0100-0x0A00` : MO(layer)
- `0x0B00-0x1400` : TO(layer)
- `0x1500-0x2800` : Macros
- `0x2900-0x2F00` : Bluetooth actions
- `0x3000-0x3DFF` : OSM, OSL, CapsWord, Repeat, Leader, Tama, GESC, etc.
- `0x4000-0x4FFF` : LT(layer, kc)
- `0x5000-0x5FFF` : MT(mod, kc)
- `0x6000-0x6FFF` : TD(index)
- `0x7000-0x7FFF` : LM(layer, mods)

## NVS — persisted data

Namespace: `"storage"` (defined as `STORAGE_NAMESPACE`).
Keys:
- `keymaps`, `layout_names` — ⚠ the blob size follows `KEYMAP_COLS`, which
  is 14 on the Niphargus left half versus 7 elsewhere. `load_keymaps`
  rejects a blob of a different size and keeps the compile-time defaults:
  a dimension change therefore invalidates the stored keymaps, with a
  warning.
- `macros`
- `key_stats`, `key_stats_tot`, `bigram_stats`, `bigram_total`
- `td_configs`, `td_count`
- `combo_cfg`, `combo_cnt`
- `leader_cfg`, `leader_cnt`
- `ko_cfg`, `ko_cnt`
- `bt_slots`, `bt_active`, `bt_enabled`

**Never** erase NVS at boot without an explicit reason (safe mode has
preserved the data since v3.7.8).

## Safe boot

RTC memory tracking of the number of consecutive boots (`BOOT_CRASH_MAGIC`).
If > 3 → `safe_mode = true`: skip display/BLE/NVS config loads. Basic USB
HID stays functional. NVS NOT erased.

## C conventions

- **No `malloc` in hot paths** (scan, HID send, ISR callbacks). Static or
  stack buffers.
- **`IRAM_ATTR`** for ISR / gptimer callbacks.
- **No `ESP_LOGI`** in matrix scan callbacks (too slow). Use `ESP_LOGD`
  with the level set to NONE in production.
- **LVGL mutex**: every LVGL access must be wrapped in
  `lvgl_port_lock()` / `lvgl_port_unlock()`.
- **`lv_obj_is_valid()`** before accessing an LVGL object after a possible
  `display_clear_screen()`.
- **NVS writes** via `nvs_save_blob_with_total()` to avoid corruption if
  the struct changes.

## Tests

Host-side tests in `test/` (standalone CMake). No embedded tests on
target. Run with: `cd test/build && ./test_runner`.

Tests must be parallel-safe: no mutated global state, no shared temp
paths. NVS mocks via fake implementations in the test.

## Anti-regression workflow (MANDATORY)

Single source of truth: `scripts/check.sh` (tripwire scaffold v0.13.0;
`--host-only`/`--board` are aliases kept from `--fast`/`--variant`,
declared in `.tripwire-divergences`).
- `./scripts/check.sh --fast` — host CMake tests (~seconds)
- `./scripts/check.sh --variant <name>` — fast + build of one board
- `./scripts/check.sh` — fast + every in-tree board, discovered from `boards/*/sdkconfig.defaults` (sdkconfig isolated per board)
- Skip-if-already-green: unchanged state since the last green → immediate
  exit; `--force` to rerun anyway.
- On red: the failing command's detail is in
  `.git/tripwire/last-fail.log` — read it instead of rerunning the build.
- **Without `idf.py` in the PATH** (outside the Nix devshell), the build
  phase **skips itself while announcing it** instead of turning red: a red
  that means "toolchain missing" is indistinguishable from a red that
  means "code broken", and ends up not being read anymore. For a full
  check, run `./scripts/check.sh` inside the devshell.

**Enabling git hooks (once per clone)**:
```bash
./scripts/install-hooks.sh   # ou: git config core.hooksPath scripts/hooks
```
`pre-push` runs the full check and blocks the push if red. WIP:
`git push --no-verify`.

**Claude Code hooks** (`.claude/settings.json`, automatic). Severity
scale: **during → informs, at wrap-up → blocks, on push → blocks.**
- `PostToolUse` on editing `.c/.h` in `main/`, `boards/`, `test/` →
  `check.sh --fast`, as a **non-blocking notice**. It flags red without
  interrupting: the TDD standard requires writing the red assertion
  BEFORE the implementation, and blocking there would sound the alarm on
  every correct step. A notice is still not to be ignored.
- `Stop` → `check.sh --fast` and it **blocks**: a turn is not concluded on
  red. The 7-board build is NOT rerun at the end of every turn, it stays
  guaranteed at pre-push.
- `pre-push` → full check, **blocking**.

A red from Stop or pre-push is never ignored and never bypassed with
`--no-verify` without a written reason.

Current board (read by `cc_session_start.sh`): `echo kase_v1 > .kase-board`.

**Declared divergences**: `.tripwire-divergences` (committed) lists the
deviations accepted from the standard scaffold. One line
`file<TAB>pattern<TAB>why`; `check.sh` turns red if a declared pattern
disappears. The host file must be **tracked by git**: a gitignored file
doesn't change the skip-if-already-green fingerprint, so its loss can slip
under an "already green — skip". **Limitation**: an undeclared deviation
is protected by nothing and the next re-scaffold will erase it — every
deliberate divergence is declared at the moment it is introduced.

**Behaviour contract**: `COMPORTEMENTS.md` (committed) lists what the
firmware must do, each behaviour tagged by what guards it — `[test:X]`,
`[smoke:X]` (an item from `docs/HARDWARE_SMOKE_TEST.md`, checked by hand
before every release), or `[NON GARDÉ]` (counted in
`.tripwire-nongardes`, a strict ratchet at push time). **Read it before
touching a source file.** A source modified with neither a test nor the
contract touched is an unanswered question: the per-edit hook raises it,
Stop blocks it. Answering it means a test, or a line in the contract. A
`[NON GARDÉ]` is an honest answer, not a free pass.

**Never** build two boards in the same `build/` with the root `sdkconfig`
(config leakage). Always `-B build_<board>
-DSDKCONFIG=build_<board>/sdkconfig`.

### TDD standard — new pure logic
Every new pure-logic function (keymap, layers, combo, tap-hold, CDC
parsing, keycode encoding…): a host test written **first**, added to
`test/CMakeLists.txt` + declared in `test/test_main.c`. The test must be
red before the implementation, green after, and parallel-safe. Invoke the
`kase-test-author` agent.

### Model economy (subagents)
The check.sh pipeline allows dropping to a cheaper model tier WITHOUT risk
of hallucination, but only where an oracle catches the error:
- **Economy model (haiku) OK**: transcription of already-specified code,
  mechanical refactors, cited extraction (`file:line` mandatory) — the
  check, the compilation, or cross-checking the citations catch drift.
- **Never below sonnet**: review, audit, debug, **and writing test
  assertions** — a tautological assertion or a hallucinated verdict pass
  the mechanical oracle as green. Judgment does not drop a tier.
- Every economy-tier task MUST end with `./scripts/check.sh --fast` green,
  and a rewired/written test MUST prove it bites (transient bug → red →
  revert).

### When to invoke the kase-* agents
- `kase-firmware-debugger` → backtrace / boot loop / crash.
- `kase-test-author` → adding pure logic (see TDD standard).
- `kase-code-reviewer` → before a merge / release.
The others (`cdc-protocol`, `board-variant`, `release-manager`,
`maintainer`, `security-auditor`): on an ad-hoc basis.

### Before a merge to main / release
Run through `docs/HARDWARE_SMOKE_TEST.md` on the boards concerned.

## ESP-IDF dependencies

Managed via `main/idf_component.yml`:
- `espressif/esp_tinyusb`
- `lvgl/lvgl: ^8`
- `espressif/esp_lvgl_port`
- `espressif/esp_lcd_gc9a01`
- `joltwallet/littlefs`
- `keyboard_button`: NOT managed — our patched copy is the project component
  `components/keyboard_button/` (a managed local override wrote an absolute
  path of one machine into `dependencies.lock`, 2026-09-20)
- `espressif/led_strip`

Lock: `dependencies.lock` (tracked in git). To update:
```bash
rm dependencies.lock && rm -rf managed_components/
idf.py reconfigure
```

## Hardware specifics

**USB**: ESP32-S3 OTG Full-Speed only (12 Mbps, max packet 64 bytes). No
High-Speed possible.

**Console UART disabled** (`CONFIG_ESP_CONSOLE_NONE=y`) — frees up
GPIO43/44/16 for matrix scanning on V2 (which uses UART0).

**GPIO reset**: `matrix_setup()` calls `gpio_reset_pin()` on all matrix
pins to detach bootloader functions (UART0, secondary SPI flash).

## Release workflow

Driven by `/tripwire:release`. Version source: the git tag only (no VERSION
file, no manifest duplicating it).

**Beta vs stable (2026-09-20).** The major number marks a compatibility
boundary (v4 = wireless split + dongle + the current CDC protocol), not a
promise of "no bugs" — maturity is said by the suffix:
- `vX.Y.Z-beta.N` = beta, published as a GitHub **pre-release**; boards that
  were not on the bench ship "build only", said so in the notes; `beta.2`,
  `beta.3`… may follow on the same X.Y.Z;
- `vX.Y.Z` = stable, published as **Latest**, only when the smoke test has
  passed on every board the release concerns — no "build only" left.
  **Scope of the 4.2 stables (decided 2026-09-21):** the keyboard boards —
  Niphargus left/right, dongle, V2/V2D. **V1** (no hardware left) and
  **Conchodytes** (the mouse, its own project) ship build-only and are out
  of the stable's scope; the notes say so.
`git describe` reads `v4.2.0-beta.1-12-gabcd` between two tags, in the HB and
in the CDC `VERSION` reply.

1. Working tree clean, `./scripts/check.sh` green (every in-tree board builds)
2. Smoke test (below), then `git tag vX.Y.Z && git push && git push --tags`
3. `scripts/build_release.sh vX.Y.Z` → `release/KaSe_vX.Y.Z_<HW>.bin` (app,
   flash at 0x20000) and `release/KaSe_vX.Y.Z_<HW>_full.bin` (bootloader +
   partitions + app + LittleFS, flash at 0x0) for the in-tree boards: V1, V2,
   V2_Debug, Dongle, Niphargus_Left, Niphargus_Right
4. The release is published on **GitHub** (the repo of record): the CI
   (`.github/workflows/ci.yml`) builds the 14 artefacts on the tag and creates
   the release — pre-release when the tag has a suffix — with generated notes;
   then `gh release edit vX.Y.Z --notes-file <curated notes>` (first done for
   v4.2.0-beta.2 on 2026-09-21; a transient artefact-upload 403 is fixed by
   `gh run rerun <id> --failed`). Manual fallback:
   `gh release create vX.Y.Z release/KaSe_vX.Y.Z_*.bin --repo mornepousse/KeSp_firmware --notes-file …`.
   ⚠ Tags don't always ride the GitLab→GitHub mirror: `git push github vX.Y.Z`
   (a tag advances no branch, it cannot stall the mirror).

### Smoke test

`docs/HARDWARE_SMOKE_TEST.md` is the checklist; the mandatory items are the
`[smoke:X]` guards of `COMPORTEMENTS.md` (each X is a title in that document).
Walk them on the boards at hand before tagging; a board that is not on the
bench ships as "build only", said so in the release notes.

See `docs/` for detailed protocols and keycodes.
