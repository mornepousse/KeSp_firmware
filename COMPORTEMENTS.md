# Behaviour contract — KeSp

What the firmware must do, and what guards it. Read **before touching a
source file**. Each behaviour is tagged by its guard: `[test:X]` (X is in a
test file), `[smoke:X]` (X is an item in `docs/HARDWARE_SMOKE_TEST.md`,
checked by hand before every release), or `[NON GARDÉ]` — the admission,
counted in `.tripwire-nongardes`, which is only allowed to go down.

A source file modified without a test or a line here is a question left
unanswered: the per-edit hook raises it, the Stop hook blocks. Answering it
means a test, or a line.

## Radio — split link

- [test:test_repos_ne_reemet_pas] At rest, the HID report is not
  retransmitted. Retransmission is bounded (100 packets/s), arms on change
  and falls silent afterwards. Without this, the retransmission spiral
  saturated the link. In fusion, the same bounded retransmission arms on
  every MATRIX change from the left (last bitmap, even empty): a change
  frame refused by the ESB (~1 %) is repeated 5× at 10 ms then silence — a
  brief keypress no longer gets only one chance to get through (Super+Q
  swallowed, bench 2026-09-13).
- [test:test_rf_status_cadence] The RF status respects its period: nothing
  before, one transmission at the period. Three links were losing
  keystrokes for the same reason — a cadence that didn't wait.
- [test:test_half_tx_repeat] The right repeats every matrix change a
  BOUNDED number of times (HALF_TX_REPEATS ticks) then falls silent: a
  change frame refused by the ESB is no longer lost, and rest stays silent
  (R1). The hold rule (reaffirmation at 100 ms) stays intact behind it.
- [smoke:NRF doesn't wedge after 5 min] The right has a radio watchdog. A
  frozen nRF24 is relaunched; there is no more permanent death of the link.

- [test:test_mouse_slot_vectors] The dongle's slot 2 is a published contract
  (`docs/DONGLE_MOUSE_CONTRACT.md`, vectors in
  `docs/contracts/mouse_slot_vectors.json`): HID mouse frame `50 01 btn dx dy
  wheel`, `PAIR_REQ` v2 with `RF_DEV_MOUSE = 2`, `PAIR_ACK` with a big-endian
  `set_id`, address `'K' 'S' set_id slot` and channel `80 + 2·(set_id % 20) +
  1`, rendezvous `KSPR\xff` / 0x28, factory 0x52, loss of the slot after
  2.5 s releases the buttons only. A foreign firmware (the Conchodytes rewrite
  in Rust, 2026-09-21) is built against these bytes: changing one changes the
  document, the generator and the test in the same commit.

<!-- Other radio invariants still need to be written down (Mae, 2026-09-13). -->

## Sleep — wake

- [test:test_veille] Grace period after a GPIO wake: for 300 ms
  (VEILLE_GRACE_REVEIL_MS, between 100 ms and 1 s) the board does not go
  back to sleep, even if inactivity — never refreshed by a wake with no key
  — says otherwise. A key with a slow pre-contact wakes the board before
  capture sees it (two empty passes); without the grace period the loop
  sent it back to sleep in ~15 ms, before the recreated driver had seen the
  key. A glitch costs 300 ms of wake time, not 15 s of radio. Holds even at
  counter overflow.
- [smoke:First key after sleep] ONE sleep path per board only: the V2D path
  (v2d_sleep.c, OLED + radio, USB probe every 3 s) is EXCLUDED from the
  B7-sleep boards. Since the left screen (2026-09-14) the "wireless +
  screen" guard was also compiling it in on that board: on a wake with an
  empty capture, V2D destroyed the driver, cut the radio, went back to
  sleep, recreated everything on its next wake — the wake key fell into
  that gap (two `matrix_setup` calls 30 ms apart in the log, 2026-09-16).
  Console on wake: a single "matrix_setup".
- [smoke:First key after sleep] No periodic esp_timer may replay its missed
  periods at wake: the esp_timer task runs BEFORE the sleep task's first
  instruction after `esp_light_sleep_start` returns, and a replay longer than
  a tap (battery gauge: one 1.2 ms ADC burst per missed 10 s → 80 ms after
  11 min; LVGL tick: one event per missed 50 ms) releases the key before the
  wake capture reads the rows. The gauge timer is created with
  `skip_unhandled_events`, the LVGL tick is stopped by the screen's sleep hook
  and restarted at wake (`lvgl_port_stop/resume`); the relay timer was
  already stopped by its hook. Bench 2026-09-21, left half, wake capture
  instrumented (`KASE_VEILLE_DIAG`): 673 s asleep → `lines at exit=0x0`, 0
  key captured, letter lost; with the fix 761 s → `lines at exit=0x4`, 1 key
  captured, letter typed (dongle: 22 frames, 4 reports within 4 s). Both
  halves showed the same signature; the "light first key lost on the left"
  open since 2026-09-16 was this — the 13 ms then measured were counted after
  the replay.
- [smoke:First key after sleep] The bench instrumentation for wake
  (timings, GPIO lines and mask on exit, 150 ms re-read window, dump of
  both passes) is under `CONFIG_KASE_VEILLE_DIAG` (default n). Outside that
  option, wake keeps: capture, single re-read at 5 ms, driver recreation,
  reconciliation — the log line "wake: n key(s) captured" and the summary "wake after N s"
  remain.
- [smoke:First key after sleep] The key that wakes the board is captured,
  emitted and reconciled — never lost. Under fusion, the left emits the
  press captured at wake (matrix_wake_capture) and its release at
  reconciliation, with the callback's own emitter: the recreated scanner
  sees no change, and on its own would never have emitted it (first key
  swallowed, bench 2026-09-13). A wake key held DOWN is never released by
  mistake: reconciliation waits for the driver's first event, not a bare
  tick. A first edge read during bounce (empty capture on a GPIO wake) is
  re-read 5 ms later before being declared a ghost — no going back to sleep
  that would swallow a brief tap; a real glitch (two empty captures) is
  still rejected. An EMPTY capture logs both raw passes ("empty capture:
  pass1=… pass2=…"): bounce (one full
  pass), slow pre-contact or ghost (both empty) can be told apart in the
  log — "touche de réveil perdue sur la gauche, depuis toujours" (wake key
  lost on the left, always has been) (2026-09-16) is hunted with this, not
  by ear. Bench diagnostic in progress (to be removed once settled): on an
  empty capture, re-read every 10 ms for 150 ms, the log says after what
  delay a key appears ("JAMAIS" [NEVER] = late wake or glitch; 10-20 ms =
  false early read; 100 ms+ = the next keypress) — the left sees 4 presses
  out of 5, the first one, the one that wakes it, is missing every time.
  And a stopwatch on the BLIND WINDOW in the log ("entry timing: radio / driver / arming / until sleep",
  "exit timing: sleep -> capture"): between destroying the driver and actual sleep, then
  between wake and capture, a key can neither be scanned nor wake the board
  — 160 to 570 ms of wake time around a sleep seen on the tick, to be
  pinned down. Measured: entry 9 ms, exit 6-8 ms (timer), that's not it.
  The same log line now also logs the RAW line levels at the very first
  instruction after wake and the GPIO state register ("lines at
  exit=0x.. ; wake-up pins=0x.."): a trigger line already low on exit = slow GPIO wake; high but
  invisible to capture = false capture. A light tap after a long pause did
  not wake the left, a held press did. Measured on 2026-09-16 11:03: the
  line for "a" (GPIO2), the wake trigger, but LOW 7 ms later, the key only
  readable at +36 ms — signature of a level right at the threshold's edge
  (COL 3.3 V → 1N4148W → line ~2.6-2.7 V, high threshold S3 2.475 V).
  Measured with ADC1 (diagnostic removed afterwards, it put the pin into
  analog mode): 2885 mV on the line during a held press — real margin,
  hypothesis ruled out. Still observed and unexplained by the firmware: a
  first press after a long pause seen nowhere (neither wake, nor capture,
  nor driver); switch theory (hesitant first contact), to be settled by
  moving the key to another position.
- [test:test_wake_grace] The grace period left to the recreated driver at
  wake always covers its debounce (debounce × interval + 2 scans), floor
  10 ms, ceiling 50 ms. A `vTaskDelay(1)` (between ~0 and 10 ms depending on
  the phase) was wrongly releasing a held key — Super tapped, Super+F lost.
- [test:test_veille] Light sleep kicks in at 15 s (between 5 and 20 s):
  awake and idle the board draws ~28 mA at 160 MHz (datasheet v2.2 table
  5-9 p. 67) versus 0.24 mA asleep — at 60 s, a day of typing broken up by
  pauses lost ~0.2 V (2026-09-15). Waking on a key is the nominal path, not
  an exception.
- [smoke:Idle wake] The halves run at dynamic frequency (CONFIG_PM_ENABLE,
  esp_pm): 160 MHz while a task is working, 40 MHz (XTAL, PLL off) as soon
  as both cores are idle — 27.6 → 13.2 mA (datasheet v2.2 table 5-9 p. 67).
  Under DFS: the radio still acks at ≥ 98 %, the screen still refreshes,
  the UART0 console stays readable (esp_pm switches it to XTAL), the TRRS
  link UART is on XTAL, sleep and wake are unchanged; a mounted USB host
  holds APB at 80 MHz (a lock) and suspends automatic sleeps; a cold USB
  plug-in during idleness DOES ENUMERATE (bench 2026-09-16: cafe:4003 seen,
  route=USB, lock held — the only failure observed was a charge-only
  cable). Boot log: "DFS actif : 160 MHz en travail, 40 MHz oisif" (DFS
  active: 160 MHz while working, 40 MHz idle).
- [smoke:Idle wake] At rest, matrix scanning STOPS (keyboard_button in
  power-save: gptimer stopped, columns held high, an interrupt on the rows
  restarts it on the first press, first scan < 1 ms). Without this the
  processor left idle 1000 times a second and DFS never dropped down. The
  hold (gpio_hold) that this mode places on the columns is LIFTED before
  any driving outside the driver (capture at wake, sleep arming,
  recreation): otherwise a held key reads across its whole row. The
  driver's ISR is not in IRAM (it calls flash code): a key pressed during
  an NVS write waits a few ms instead of crashing. The LVGL tick moves to
  50 ms, the task sleeps for up to 500 ms.
- [smoke:First key after sleep] Left/right review of 2026-09-16: NO
  keystroke statistics on the halves (CONFIG_KASE_KEY_STATS=n: no
  counting, no bigrams, no NVS — "pas de stats sur le clavier, au mieux sur
  le dongle" [no stats on the keyboard, at best on the dongle]; a flash
  write cuts the cache and stops scanning and transmission — 21 saves in
  one morning on the left, zero on the right), and the left TURNS OFF its
  radio in sleep just like the right (kbd_relay_sleep_prepare /
  wake_restore around light sleep: the chip comes back ~5 ms before
  capture, same as the right).
- [smoke:Idle wake] At rest, almost nothing wakes the processor: the
  left's radio relay drops to 100 ms (10 ms as soon as a key is held, a
  bounded repair is in progress, a sync, or the left is LISTENING to the
  right re-transmitted in USB route — [test:test_kbd_refresh]
  `kbd_relay_cadence_ms`: at 100 ms this tick, which drains the receive
  FIFO, was swallowing brief presses from the right over USB (regression
  b545e2aa, bench 2026-09-16); a change wakes it immediately); the right's
  refresh task at 100 ms (20 ms with a held key, notified by
  scan-on-change); the TRRS link is EVENT-DRIVEN at rest (see "5 V
  handshake"); the DFS USB lock follows TinyUSB events, no more polling;
  the heartbeat at 10 s. Typing, repairs (ACK ≥ 98 %) and wake are
  unchanged — that's what the DFS smoke test checks.
- [test:test_cadence] All the halves' cadences live in `power/cadence.h`;
  every REST cadence is guarded by a `_Static_assert` ≥ 30 ms (3 ticks at
  100 Hz, the automatic light sleep threshold) — a shorter periodic wait
  doesn't compile (verified: 10 ms → "static assertion failed"). ACTIVE
  cadences stay ≤ 20 ms.
- [smoke:Sleep current] The matrix scan timer (keyboard_button's gptimer,
  running only while a key is held) is clocked from the XTAL, not the default
  APB: the IDF gptimer driver takes an `ESP_PM_APB_FREQ_MAX` lock for an APB
  source and only `ESP_PM_NO_LIGHT_SLEEP` otherwise, so the chip no longer
  waits between two 1 ms scans at 80 MHz. Key held on the left: 36 → 27-33
  mA (2026-09-25).
- [smoke:Sleep current] The halves scan every 5 ms while a key is held, with
  a 2-scan debounce (10 ms of stability, a `_Static_assert` keeps it ≥ 5 ms):
  at 1 ms the scan task alone kept DFS up. Key held: 27-33 → 24 mA, several
  keys 28 mA; fast typing, first key after sleep and a held Backspace checked
  on the bench (2026-09-25).
- [test:test_kbd_refresh] [test:test_cadence] A key simply held on the left
  runs the relay tick at 50 ms, not 10: it only has to reaffirm the hold every
  100 ms. Repair, keymap sync and USB listening keep the 10 ms tick. Worst gap
  between two reaffirmations (150 ms) leaves one losable under the dongle's
  400 ms release. Bench on the left: key held 24 → 23 mA, a held Backspace
  and a held MO unaffected (2026-09-25).
- [test:test_cadence] At rest the halves have no poller shorter than ~0.5 s,
  so automatic light sleep gets its 30 ms of calm between keystrokes. Six
  interleaved pollers used to wake core 0 ~140 times a second while the CPU
  sat >99 % idle (PM profiling + per-task CPU stats, 2026-09-25): CDC command
  task 50 ms even with no USB (now woken by `receive_data()`, 1 s safety
  net), LVGL tick 50 ms and refresh 200 ms and status display 100 ms (now 1 s
  on the memory-LCD halves — status only, Mae; `_Static_assert` in
  memlcd_backend.c), keyboard task 100 ms (1 s, its timers being tracked),
  relay 100 ms (500 ms; `KBD_RELAY_REPOS_MS + RF_STATUS_PERIOD_MS <
  RF_LINK_LOST_MS` tested, proven biting). Diagnostic build with all of them
  at ~1 s: 24 → 6-10 mA between keystrokes on the left; the real change
  measured 6-12 mA (Mae's ammeter). Side effect: the CDC answers in 2.2 ms
  median instead of ~30 ms (10 requests, left vs dongle still polling). A new periodic task
  on the halves is a new interleaved wake-up: justify it in cadence.h.
- [test:test_keyboard_cadence] The keyboard task runs at 10 ms only while a
  timer IS waiting on the clock — a tap-hold undecided (`tap_hold_pending`,
  tested), a tap-dance counting (`tap_dance_pending`, tested), a leader
  sequence, a macro queued — or a USB host is present, or matrix test mode;
  100 ms otherwise. A matrix change notifies it, the first key never waits.
  Until 2026-09-25 ANY keypress armed 1.5 s at 10 ms "in case": typing every
  ~200 ms, the window never closed and the half never slept while typing
  (~20 mA over a working day, measured). At 100 Hz a 10 ms loop leaves ONE
  tick free when automatic light sleep requires three (bench 2026-09-16:
  SLEEP mode 92 %, light_sleep_counts = 0). A new timed feature must join
  the list in keyboard_task.c, or it runs with a 100 ms resolution.
- [smoke:Idle wake] The halves SLEEP BETWEEN KEYSTROKES: tickless idle
  (CONFIG_FREERTOS_USE_TICKLESS_IDLE) + esp_pm's automatic light sleep as
  soon as both cores are idle for more than 30 ms — at rest ~9 sleeps/s
  (100 ms cadences), the bench heartbeat proves it (CONFIG_PM_PROFILING:
  "light_sleep_counts" climbing, rejections at 0). In automatic sleep the
  nRF24 pins are held (CE low, CSN and IRQ high) and the screen follows a
  200 ms LVGL refresh. B7 sleep at 15 s remains the only path to the long
  stage and deep sleep; typing, ACK, screen, console and wake are
  unchanged.
- [smoke:Idle wake] The RIGHT's heartbeat (half_link, 10 s) carries the
  same bench indicators as the left's when CONFIG_PM_PROFILING is set:
  esp_pm modes and locks, armed esp_timer alarms, and per-task CPU time if
  FreeRTOS statistics are compiled in. Measured: the right does not sleep
  between keystrokes during its ~first 10 seconds after a boot (116
  wakes/s per core instead of 60-80, TinyUSB ruled out: its CPU time stops
  moving after init), then ~100 sleeps per 10 s, including before its
  first B7 sleep. A boot only follows a deep sleep or a flash: ≤ 15 s at
  13 mA, accepted and not pursued further. No effect outside the bench.
- [smoke:A night on battery] A half holds a night on battery: on the order
  of a hundredth of a volt lost (244 µA), not 0.2 V (= ~20 mA: it didn't
  sleep — 2026-09-12 left, 2026-09-15 again). To READ it: every wake logs
  "wake after N s of sleep (cause=…) — total: n sleeps, X s slept out of
  Y s", and the heartbeat carries "idle=… slept=X s/n vetos=…"; a sleepless night reads without
  a multimeter, and a refusal reads by its name.
- [smoke:A night on battery] The console is FLUSHED before
  `esp_light_sleep_start` (`uart_wait_tx_done`, ≤ 20 ms): the "light sleep"
  line and the entry timings come out BEFORE sleep, closer to the wake
  log.
- [smoke:Idle wake] A SINGLE sleep task (power/veille_task.c), identical on
  both halves, owns inactivity, the vetoes and the heartbeat ("HB up=
  idle= slept= vetos=…" + role suffix: route/relay on the left,
  link/batt on the right). A module with a reason to prevent sleep POSTS A
  VETO — usb (left only: TinyUSB event + tud_ready catch-up at 1 s), link
  (5 V TRRS active), sync (keymap pull), test (matrix test mode); a module
  with something to put to sleep REGISTERS A HOOK (radio, screen, gauge),
  called in order at sleep and in reverse order at wake, all BEFORE key
  capture. No module evaluates sleep anymore, sleep no longer calls any
  module by name. Tick 1 s (sleep only kicks in at 15 s). "sleep REFUSED
  depuis N s : vetos=…" (sleep REFUSED for N s: vetoes=…) every 30 s while
  a veto holds.
- [smoke:A night on battery] DEEP sleep is reachable: a timer wake at the
  deep threshold (4 h minus the light stage) switches to deep sleep
  without going through a keypress — since inactivity was only evaluated
  while awake, the board stayed in light sleep until a key ("it never goes
  into deep sleep", 2026-09-15). A GPIO wake disarms the timer.
- [smoke:A night on battery] In sleep, the screen's CS, SCK and MOSI are
  pulled LOW (GPIO sleep config), never floating on the panel's CMOS
  inputs — the ESP isolates its pins in light sleep.

## Input — matrix and HID report

- [test:test_kp_slot_recycle_ne_gele_pas_le_keycode] A recycled key slot
  does not keep the keycode from the previous cycle. Otherwise a released
  key kept emitting the old code.
- [test:test_kp_held_key_keeps_the_layer_it_was_pressed_on] A key keeps the
  keycode of the layer it was PRESSED on until it is released — the layer is
  latched at press time, per physical key (QMK's rule). Arrows on MO(2),
  Right arrow on the 'U' position: releasing MO a hair before the arrow used
  to re-resolve the held key on the base layer and type a 'u' (2026-09-20).
  Mirror case ([test:test_kp_key_pressed_before_mo_keeps_the_base_layer]):
  a key held before the MO keeps its base keycode while the layer is active.
  LT ([test:test_kp_key_that_resolves_an_lt_hold_is_on_the_lt_layer]): the
  key that resolves an LT as a hold is read on the LT layer from its first
  report (before: the base character was typed once, then the layer one).
- [test:test_board_pin_tables_match_the_geometry] Matrix pins come from the
  board's `BOARD_ROW_PINS`/`BOARD_COL_PINS` tables (exactly MATRIX_ROWS /
  MATRIX_COLS entries); the core has no fixed matrix shape — the 5×13
  initializer of matrix_scan.c/veille.c and the GPIO_NUM_NC padding of the
  Niphargus boards are gone (2026-09-20). Bench: left, right and V2D, one key
  per row and column, sleep and wake by a key on each row.
- [test:test_board_contract_niphar_left] Every board passes the same pin
  contract from its `BOARD_PINS(X)` list (`test/board_contract.inc`, one test
  unit per board): no GPIO used twice, numbers in 0..48, no strapping pin
  (0/3/45/46 — waived by the legacy KaSe V1/V2 pinouts, said so in their
  board.h), no native-USB pin (19/20) nor octal-PSRAM pin (35-37) when the
  board declares them, pin tables consistent with MATRIX_ROWS/COLS and
  KEYMAP_COLS. Proven biting: a duplicated column on the V2 turns V2 and V2D
  red. The Niphargus pin tests keep their hardware-specific facts.
- [test:test_left_chest_link_pins] The chest link uses GPIO3 (CS_P4, R48 to
  the chest's rail) and GPIO46 (IRQ_P4, R49 pull-down) on the left — from
  the netlist, 2026-09-29; the matrix never lands on them, and the generic
  board contract exempts exactly these two strapping pins
  (BOARD_PINS_STRAPPING_WIRED). The chest's own HARDWARE.md said IO7/IO11:
  those are the P4-side numbers.
- [test:test_kp_sec_confirm_routes_to_chest] K_SEC_CONFIRM goes to the chest
  while it has an operation pending, to the local gate otherwise — one press,
  one destination; a held key confirms once
  [test:test_kp_sec_confirm_held_confirms_chest_once]; only a NEW physical
  press may confirm — a chest op that arrives while the key is already held
  is NOT confirmed by that hold
  [test:test_kp_sec_confirm_held_before_chest_op_does_not_confirm].
  chest_gate_press() is called from key_processor.c only — never from CDC;
  enforced by scripts/tripwire.d/chest-confirm.sh (proven biting).
  The press carries the op code seen on screen at press time, not a live
  reference: the link task confirms it only if the chest's CURRENT block is
  OK, the chest is READY, and its pending op still equals that stored op —
  otherwise the press is dropped, never applied to a different operation
  [test:chest_press_matches]. A press taken on a non-OK round (chest reboot,
  a corrupt block, a bad version, a skipped read) does not survive to the
  next OK block [test:test_kp_sec_confirm_press_records_the_seen_op].
  The press carries the ARMING (instance) seen on screen at press time, not
  just the op code [test:test_kp_sec_confirm_records_the_instance].
  K_SEC_CONFIRM from the right half (a remote column, over the
  unauthenticated inter-half radio) is ignored — it confirms neither the
  chest nor the local gate [test:test_kp_sec_confirm_ignored_from_the_right_half]
  [test:test_sec_confirm_from_local].
  K_CHEST_NEXT requests the next chest USB mode once per physical press
  [test:test_kp_chest_next_requests_a_mode_change_once].
  K_OATH_PREV/K_OATH_NEXT queue a cursor move over the TOTP accounts, one
  step per physical press, saturating the queued delta at +-16 steps, and
  touching no other channel (not a code request, not a mode change, not a
  chest confirm) [test:test_kp_oath_next_requests_nav_once]
  [test:test_kp_oath_prev_requests_nav_once]
  [test:test_kp_oath_nav_sequence_take_once]
  [test:test_kp_oath_nav_saturates_positive]
  [test:test_kp_oath_nav_saturates_negative]. The accumulator has two
  writers — key_processor.c accumulates, the link task's take clears — and
  is a CAS loop, not a plain store, so a take racing an accumulate never
  drops or duplicates a step [test:test_chest_gate_oath_nav_cas_survives_a_concurrent_take].
  K_OATH_CODE requests a code for the account under the cursor, once per
  physical press and touching no other channel
  [test:test_kp_oath_code_requests_once]
  [test:test_kp_oath_code_twice_before_take_yields_one]. None of the three
  keys is left-only or security-bound: browsing the list and asking for a
  code carry no authority by themselves — only K_SEC_CONFIRM arms or
  authorizes; a press on any of them from a remote (right-half) column is
  accepted the same as from the left
  [test:test_kp_oath_keys_accepted_from_the_right_half]. A right-half key
  held through more than `HALF_LINK_TIMEOUT_MS` (400 ms) of radio silence is
  released by the left and then re-affirmed once the link resumes, which
  `is_new_press` sees as a fresh press — one extra queued nav step or code
  request. Harmless: a code is still never shown without a following
  K_SEC_CONFIRM on the left.
  A K_IS_SEC key (K_SEC_CONFIRM/K_CHEST_NEXT/K_OATH_*) that resolves an LT
  hold on its OWN press — the LT's tapping-term key is itself a security
  action on the target layer — fires on that very cycle, not never: these
  keys gate on `is_new_press`, which the LT re-latch step only ever sees
  true on the resolving cycle, so folding them back in like a plain HID code
  (zero the slot, wait for "next cycle") silently swallowed the press
  forever [test:test_kp_lt_resolves_oath_next_on_its_press_cycle]
  [test:test_kp_lt_resolves_sec_confirm_from_left_confirms_once]. The
  left-only filter on K_SEC_CONFIRM survives this path unchanged — a remote
  column reached through an LT resolution still confirms nothing
  [test:test_kp_lt_resolves_sec_confirm_from_right_confirms_nothing].
- [smoke:New board from the template] A board is one folder:
  `scripts/new-board.sh <name>` copies `boards/_template/` (board.h with the
  pin tables and `BOARD_PINS(X)`, keymap, layout, sdkconfig.defaults, README)
  and writes the board's contract test unit; `check.sh` discovers boards from
  `boards/*/sdkconfig.defaults` (declared divergence), CMake refuses
  `_template`, and a keyboard without a screen says so with
  `KASE_NO_DISPLAY=y` instead of inheriting the V2 OLED. The display backend
  is chosen by a real `#define BOARD_DISPLAY_BACKEND_*`, not by any mention
  of the name (the template's comment listed them and used to select the
  memory-LCD). Proven 2026-09-20 with a demo board: contract test runs, the
  full check builds 8 boards, the 7 existing binaries unchanged.
- [smoke:New board from the template] A board may live OUTSIDE the
  repository: `idf.py -DBOARD_DIR=/path/to/board` builds it (BOARD = the
  folder's name unless given), `scripts/new-board.sh <name> <parent>` creates
  it there. Proven 2026-09-20: a template board in /tmp built from its folder
  alone, PRODUCT_NAME in the image.
- [test:test_take_consumes_the_signal] A matrix edge is never lost during
  reading: the signal is taken, consumed, never overwritten by the next
  read.
- [test:test_first_report_always_sent] A HID report refused by the host is
  not dropped — it is resent. Otherwise a modifier stayed stuck.
- [test:test_th_lt_oob_wins_recompute_after_valid_release] An out-of-bounds
  LT (layer-tap) cannot win the layer recompute: only a valid release
  participates in it.

## Niphargus — 5 V handshake

- [test:test_lost_probe_eventually_reprobes] After a lost probe, the 5 V
  handshake probes again. It doesn't stay stuck on a failure.
- [test:test_kbd_route] ONE rule for USB cable presence
  (`usb_presence_brut`) drives USB/RF routing, the 5 V TRRS source, the
  sleep veto — and, since 2026-09-25, the DFS APB lock (`pm_dfs.c`), which
  was driven by TinyUSB's attach/detach events alone: the S3 does not always
  signal the unplug, the lock stayed held and automatic light sleep never
  came back (32-33 mA between keystrokes instead of 6-12, on the RF route,
  sleeping normally — the ammeter was the only witness). The sleep task
  re-applies it every second on both halves: with the VBUS bridge soldered (`KASE_VBUS_SENSE`) the GPIO
  level is authoritative — a wall charger doesn't enumerate and still
  makes this half the source; without the bridge, `tud_ready()`; the bench
  override (`KASE_LINK_FORCE_SOURCE`) wins over everything. Until
  2026-09-19 each of the three read its own source; the day the bridge is
  soldered, enabling `KASE_VBUS_SENSE` is enough.
- [smoke:Idle wake] The TRRS link task is EVENT-DRIVEN at rest (5 V dead,
  no USB): blocked on the UART driver's event queue, one byte from the
  peer wakes it immediately; USB, a human event, is only polled at 1 s
  (`LINK_REPOS_MS`). A 10 ms tick only during handshake or with the link
  established. A UART overflow (floating TX from a sleeping peer) drains
  and restarts. Bench 2026-09-19: left USB + TRRS → `state=2 5V=1`, 490
  probes / 485 ACKs, sleep refusal `link=1`; unplugged → `state=0 5V=0` in
  < 1 s.
- [smoke:Sleep current] VDD_SPI is powered down in light sleep on both halves
  (`CONFIG_ESP_SLEEP_POWER_DOWN_FLASH`): the N16R8's in-package PSRAM is never
  initialised, its CS floats while the IOs are isolated, and it leaked 2.5 mA
  all night (7.5 → 5.0 mA, ammeter, 2026-09-25). The IDF's own guard
  (`ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND`) depends on SPIRAM and cannot be
  selected here. The TRRS UART1 on the main crystal cost another 3.4 mA
  asleep; it is released during sleep since 2026-09-25: 1.75 mA measured.
- [smoke:Sleep current] The half withdraws from the USB bus before EVERY
  light sleep (`tud_disconnect()`, `tud_connect()` on wake), whatever
  `tud_mounted()` says. Gated on it, the sleep current depended on history:
  1.76 mA after a reset with no host ever seen (controller left attached),
  0.67 mA after a USB session and a hot unplug (`tud_mounted()` stays stuck
  true on the S3, so the disconnect happened to run) — found and reproduced
  3× by Mae with an ammeter on 2026-09-25. With a host present the half does
  not sleep (`VEILLE_VETO_USB`), so the unconditional pair costs nothing.
- [smoke:5 V handshake on sleeping halves] The 5 V needs BOTH halves awake:
  a sleeping half does not hear the probe, by design since 2026-09-25. The
  link's UART1 runs on the main crystal (the only clock that keeps its baud
  under DFS) and, installed, kept that crystal alive through light sleep —
  3.4 mA asleep, measured. So the sleep hook has the link task delete the
  driver and the wake hook reinstall it; an established link vetoes sleep
  anyway (`VEILLE_VETO_LIEN`), so the UART is only released when it serves
  nothing. The UART wake source of 0cd026ed (a probe waking a sleeping half)
  needed that clock and was removed — Mae chose the 3.4 mA over it. USB is
  not a wake source either (TRM table 10.4-3): plug the cable, press a key on
  each half, the bolt appears on both screens.
- [test:test_memlcd_model] The screens SAY whether the link is up: a 16x12
  two-arrow pictogram (⇆) in place of the charge marker while the 5 V is
  closed on our side (`link_uart_active()`) — the right beside its voltage,
  the left beside its battery percentage (`test_statut_haut`) — displayed on
  both halves, part of the redraw diff. It replaced a bolt (2026-09-25) that overlapped the " +"
  marker exactly while the link was charging the half. Without it the
  handshake had no witness but the console — which you do not have while
  typing on battery, and which is exactly what was missing to see this bug
  (Mae, 2026-09-23: "I have no icon on the screen to see whether it's
  active").
- [test:test_veille_veto] A held key keeps its half awake
  (`VEILLE_VETO_TOUCHE`, posted and lifted by the matrix change callback,
  which also fires on the release). The driver reports changes only: without
  the veto, a key held with nothing else happening let inactivity grow, the
  half slept key down, woke at once on the high row, and looped. Required by
  the 5 s light-sleep threshold of 2026-09-25 (Backspace held while the host
  auto-repeats, a layer key held while reading).
- [test:test_veille_veto] Sleep veto registry (`power/veille_veto.h`,
  pure): one state per name (usb, lien, sync, test, pair, key, code), a
  posted veto blocks all sleep, lifting an absent veto has no effect,
  names bounded for the HB (all seven fit in `VEILLE_VETOS_STR_MAX`, 40
  bytes since the `code` veto). Wired into the single sleep
  task (Task 7 of the power structure plan). The `pair` veto is posted by
  both active pairing tasks (`kbd_pairing_task`,
  `half_fusion_pairing_task`): each round holds the chip for ~150 ms over
  30-40 s with no typing — without it, at 15 s of inactivity `radio_sleep`
  would miss the lock and cut the chip out from under the pairing task
  (review 2026-09-20).

## Niphargus — radio: one chip, one owner

- [test:test_radio_owner] A half's nRF24 chip has ONE owner
  (`comm/rf/radio_owner.c`): one mode at a time (PTX towards a target, PRX
  listening to a target, off), idempotent, `radio_rearmer` to rewrite the
  current mode (watchdog); transmitting while in PRX is REFUSED (that's
  what the excursion is for); an excursion DRAINS the receive FIFO into
  the consumer BEFORE leaving and comes back to listen to the previous
  target; waking RE-ARMS the current mode (power_up does not touch CE);
  the lock is held for the whole sleep; a chip absent at probe time
  refuses everything without touching it. Verified against the sequence
  of calls to the hardware (fake recorder, has bite).
- [test:test_radio_owner] A transmission whose state is STALE is not
  sent: `radio_emettre` evaluates `encore_valide` ONCE THE LOCK IS
  ACQUIRED and returns PERIME without touching the chip (a counter
  separate from ESB refusals and unavailability). This is what closed the
  double-press on a short tap of 2026-09-20: a REPEAT (bounded repair,
  reaffirmation at 100 ms) was re-reading the press while the scan
  callback emitted the release, waited for the lock behind it, then
  emitted the stale press — P, R, P, R, which the dongle's transition
  queue faithfully replayed. Both halves keep a state generation (+1 per
  change, BEFORE transmission, under a critical section); every repeat
  leaves with its snapshot + generation. The owner's sleep is idempotent
  (deep sleep calls the hooks again after light sleep) and only returns at
  wake the lock it took. On the RIGHT side, INDISPO (missed lock, chip
  asleep) is treated like PERIME: nothing left, so no sequence number
  consumed, no "no ACK" for the screen, no step of the fallback FSM —
  eight missed locks in a row were switching the target to the left
  without a single frame having been refused (review 2026-09-20). The
  switch is decided on the target snapshot taken under `s_etat_mux`
  (before/after the step), not by re-reading the owner's live state
  (`radio_cible` is now nothing more than a test oracle). The left's
  "relay" hook (refresh timer) is registered BEFORE the owner's: at wake
  (reverse order) the radio is up before the timer starts again. The
  keyboard task's cadence reads USB presence through
  `usb_presence_cable()`, the same rule as routing, the link and sleep.
- [test:test_radio_owner] A pairing round (`radio_pair_round`) targets the
  rendezvous, transmits, listens, then RETURNS to the current target no
  matter what — a board that stayed on the rendezvous channel would stop
  acking anything.
- [smoke:Idle wake] The RIGHT no longer touches the chip: `half_link.c`
  only builds frames (half-matrix, STATUS, target fallback through the
  pure FSM) and goes through radio_owner to transmit, switch target,
  re-arm, pair, sleep. Bench 2026-09-19: ACK 100 % / 97 %, four wakes with
  key captured and radio re-armed, dongle unplugged → "fallback: switch TX
  -> GAUCHE" (fallback: switch TX -> LEFT) then dongle back and 92 → 100 %
  ACK.
- [smoke:Fusion — local engine dormant] The LEFT no longer touches the
  chip: `kbd_relay_tx.c` asks the owner for PTX towards the dongle
  (wireless) or PRX on the link (USB), delivers its frames (raw, HID,
  STATUS, sync REQ) through radio_send_ap and its USB announcement through
  radio_excursion_tx — which drains the right's frames from the FIFO into
  the consumer BEFORE leaving. ⚠ The link listens on the FIXED address
  'KaSe'.03 (the one the dongle targets), not the address derived from
  set_id: before the owner existed, listening started on the wrong address
  and the first excursion fixed it by accident. The relay timer stops on
  sleep through a local hook (otherwise its ticks would count
  "unavailable" and skew "dongle seen"). The right's half-matrix received
  while listening over USB (`s_remote_bm` + flag) is written by the
  esp_timer task and read by scan and the keyboard task: the two always
  travel together under `s_left_mux`, the flag can never be seen before
  the bytes (review 2026-09-20). Bench 2026-09-19, four scenarios: battery
  (98.3 % ACK left alone, 5 wakes with radio re-armed), simultaneous USB
  (the right goes out through the left), return (98.6 %), sync via
  round-trip ACK (40/40, match=1 twice).

## Fusion — engine routing
- [test:test_board_contract_niphar_left] Kconfig roles are named after what a
  board does, not after a board: `KASE_SPLIT_MASTER` (the half with the
  keymap engine — Niphargus left) and `KASE_DEVICE_ROLE_SPLIT_SCANNER` (the
  half that only sends its matrix — Niphargus right) replaced
  `KASE_NIPHAR_MASTER` / `KASE_DEVICE_ROLE_NIPHAR_SLAVE` on 2026-09-20;
  `cdc_split_scanner_stubs.c` likewise. Pure rename: the 7 binaries are
  byte-identical after regenerating every sdkconfig.

- [test:test_gauche_par_usb] Without a USB host, the left doesn't type
  locally: it transmits its raw data, the dongle types. On the RF route,
  its USB HID transmitters (`hid_transport.c`) fall silent — no waiting on
  an EP nor "report not sent (EP busy)" on every keypress towards a USB
  with no host.
- [smoke:Fusion — local engine dormant] Off USB, the left's LOCAL engine
  doesn't run at all: the scan callback transmits the raw matrix to the
  dongle, remembers the state, notes activity and stops (no report, no
  tap-hold, no combos, no silent HID). USB plugged in → the route switches
  and the engine resumes on the next scan, keymaps already loaded at boot.
  Matrix test mode (CDC) keeps control. "Only load the local keymap with
  USB" (2026-09-16): it's execution that gets conditioned, not the code.
- [smoke:Fusion — local engine dormant] FUSION is the DEFAULT configuration
  of the three Niphargus boards (boards/niphar_left|right/sdkconfig.defaults,
  dongle): what `scripts/check.sh` builds at pre-push is what gets
  flashed. Until 2026-09-18 the check was guarding the pre-fusion left
  (HALF_LINK_RX) while the boards were running unguarded `*_fusion`
  builds.
- [smoke:Fusion — local engine dormant] The pre-fusion path "the left
  listens to the right directly" (HALF_LINK_RX, B3 first version) is
  REMOVED on 2026-09-18: no board was compiling it in anymore.
  `half_link.c` now only contains the right, `kbd_relay_tx.c` only the
  left; sleep no longer has an #if ladder by role for the radio.

- [test:test_fusion_file] The dongle's engine REPLAYS every received
  transition, in order (`comm/rf/fusion_file.h`, an 8-state fused queue):
  a press + release falling between two cycles make two cycles, a tap
  played — no more "last state wins". A state identical to the last one
  pushed (hold reaffirmation) is not a transition; when full, the queue
  merges new ones into its last slot and COUNTS it: `transitions_ecrasees`
  (CDC RF_STATUS[27..30]) now only measures this overflow. Dongle SILENT
  (left on USB): the queue is DRAINED every cycle (`fusion_file_vider`:
  the wait leaves, the counter stays, the last pushed state is forgotten)
  and on resume the current state is pushed once more — without this, up
  to 7 stale transitions from the right (typed during USB) were replayed
  at unplug time: ghost keystrokes (review 2026-09-20). The left-USB mode is
  taken from the LEFT's STATUS only: the right shares the slot and sends a
  battery STATUS every 30 s with mode_usb=false, which flipped the dongle
  back to "wireless" for ~200 ms each time and made it type the held key
  (bench 2026-09-20: 9 reports emitted in two minutes of typing with the
  left on USB, expected 0). Bench 2026-09-19:
  one minute of fast two-handed typing, 699 frames, 182 reports, 0
  overwrites (536 in one evening with the old engine), nothing lost in
  use. The dongle counts RE-PRESSES (the same key pressed again < 30 ms
  after its release, RF_STATUS[43..46], the last one attributed in
  [47..50]: half, key, delay): the signature of a stale repeat or a
  mechanical bounce, now replayed and no longer masked by sampling; the
  halves' debounce goes from 3 to 5 ms (2026-09-20). Bench: 13 re-presses
  in 5000 frames with the left uncorrected ("ppa", "pap", "paa" — p and a
  are on the left in Dvorak); with both corrected: 1697 frames of chained
  "pa", 0 re-presses, 0 overwrites, no duplicate on screen.
  RF_STATUS[31..34] (max engine gap) and [35..42] (USB sent/refused,
  retries) remain the witnesses of the dongle→host link.

## Fusion — config-sync guard rail

- [test:test_rf_status_config_fp] The keymap's CRC-32 fingerprint travels
  in PKT_TYPE_STATUS (round-trip), and is 0 if absent (backward-compatible
  with a pre-fingerprint firmware). The left announces it; the dongle
  reads it.
- [test:test_coherence_once_par_changement] The dongle only reports
  coherence (match or divergence) on a fingerprint CHANGE, not on every
  state frame (~1/s) — otherwise the console would be flooded.
- [test:test_fp_match] A null fingerprint ("not announced yet") never
  counts as a match: 0 vs 0 stays incoherent.
- [smoke:Config divergence reported] Over wireless, if the dongle's keymap
  diverges from the left's, the dongle logs it and exposes it over CDC
  (KS_CMD_CONFIG_COHERENCE: own_fp/left_fp/age/match) — the controller can
  warn about it. Otherwise two engines would silently type differently.

## Fusion — automatic keymap sync (ACK payload)

- [smoke:ACK payload return channel] The pull is carried by
  `comm/rf/keymap_pull.c` (extracted from the relay on 2026-09-19, a
  literal move): `on_ack` decodes beacon/chunk under the radio owner's
  lock, `tick` saves to NVS outside the lock and sends a REQ every 100 ms
  via the relay, with a `sync` sleep veto meanwhile. Bench: divergence →
  40/40 in ~10 s → NVS → restore → 40/40 → match=1.
- [test:test_keymap_sync_frames] The BEACON/CHUNK/REQ frames survive
  encode/decode and fit in an nRF24 ACK payload (≤ 32 B); the 40 × 28 =
  1120 = keymap geometry is locked in, with no partial chunk.
- [test:test_keymap_sync] The reassembler only accepts the next expected
  chunk; duplicates and out-of-sequence chunks are ignored without writing
  anything — a replayed ACK payload never corrupts the keymap being
  received.
- [smoke:ACK payload return channel] The PTX (left) reads the payload
  carried by the ACK after TX_DS, and its RX FIFO never clogs (drained if
  unread or corrupted). Without this, three loaded ACKs are enough to
  silently mute the return channel.
- [smoke:Keymap sync without a cable] A dongle↔left divergence resolves ON
  ITS OWN over radio: the dongle slips the keymap into the ACKs of the
  left's normal transmissions (beacon, then chunks on demand), the left
  reassembles, saves to NVS and announces the new fingerprint; `match`
  goes back to 1 without plugging in the left, and the beacon falls
  silent right away (zero cost once synced). A held key keeps priority
  over the pull (never a key wrongly released for a keymap). The dongle
  only loads an ACK payload after a frame FROM THE LEFT (STATUS, SYNC_REQ,
  left MATRIX) — the right shares the slot and would consume it for
  nothing: sync also converges under two-handed typing.

## Battery — half gauges

- [test:test_batt_calc] Battery voltage is converted from the 1M/1M
  divider (V_batt = 2 × V_adc), averaged, and rejected outside [2.5 V;
  4.5 V] (0 = unknown, never a wrong number); SoC is a bounded, monotonic
  Li-ion 16340 table; "full" requires a plateau ≥ 4.15 V held for 2 min
  with hysteresis, "probably charging" a rise ≥ 0.1 V within a 5 min
  window — a discharge or a slow drift never counts; an unknown reading
  forgets everything.
- [test:test_rf_status_half_et_charge] STATUS carries half identity and
  charge state in its flags nibble, without changing size; an old frame
  reads as left / unknown (backward-compatible).
- [smoke:Battery gauge] Both halves report a plausible voltage to the
  dongle (CDC BATTERY, left/right slots), the right through a STATUS every
  30 s without stopping itself from sleeping; an unknown voltage displays
  as "inconnue" (unknown) (0xFF), never 0 V; while charging, FULL appears
  after the plateau.
- [test:test_batt_calc] Battery level with hysteresis (`batt_niveau_step`):
  LOW below 3.5 V, CRITICAL below 3.3 V, recovery with 0.1 V of margin; a
  rejected sample (0) KEEPS the level (a forced NORMAL was causing
  LOW→normal→LOW, log and sleep threshold included, for the duration of
  one reading), a gauge silent since boot stays normal; the log says
  "battery: LOW/CRITICAL/normal (dV)" (battery: LOW/CRITICAL/normal
  (dV)) on every change.
- [smoke:Battery gauge] LOW battery: the voltage stays displayed as-is (no
  blinking: one more redraw for nothing), the gauge keeps its reading with
  a THICKENED border (that's the alert), and the half no longer declares
  itself SOURCE of the 5 V TRRS (no probing, `state=0 5V=0` even on USB).
  CRITICAL: on top of that, light sleep at 5 s instead of 15. No forced
  shutdown (the DW01A cuts at 2.5 V). Bench 2026-09-19 with shifted
  thresholds (4.4/4.3 then 4.4/4.1 V on a 4.2 V cell): CRITICAL → "light
  sleep" at 5.7 s; LOW → USB + TRRS plugged in, 0 probes, link dead; real
  thresholds → the same setup brings the link up (36/38 ACK).

## Screens — the halves' Sharp memory-LCD

- [test:test_memlcd_model] rev8 is an involution (the line address reads
  CA0 first, the ESP32 sends MSB-first: a wrong reversal = a silently
  blank screen, no error); the portrait buffer transposes into 68 lines ×
  20 bytes, 1 = white, an empty buffer = a white panel, pixel (0,0) → line
  0 column 159; on the left the layer name is the screen's hero: the WIDEST
  Montserrat (32 down to the 12 px floor) holding it on one line, else two
  BALANCED lines at 12 px ("NAVIG" / "ATION", never a stranded letter), a
  '~' cut only when nothing else fits (2 lines of 6 UNSCII-width characters
  with "…" until 2026-09-30); the DISPLAYED layer is the
  stable one — the engine's last_layer (base, TO, Layer Lock), never a held
  MO/LT/LM: in RF the left does not hear the right and a 1 s status screen
  cannot follow a momentary layer (Mae, 2026-09-26: "if we can't be
  reactive, we don't handle MOs"); the two status lines read "CAPS", "CW",
  then armed one-shot mods as C S A G (right mods merged) and the one-shot
  layer "Ln", empty when nothing is armed; the screen task runs at 100 ms
  on USB and 1 s on battery [test:test_cadence] and a model change is
  rendered at once (lv_refr_now), not at LVGL's 1 s refresh; the model only triggers a redraw if a
  DISPLAYED field changes — every redraw is a transaction on the bus
  shared with the radio.
- [test:test_ecran_memlcd_gauche] The left has THE SAME screen as the
  right (J12 soldered on 2026-09-14): CS 14 active-high, portrait 68×160,
  and no other backend (ROUND/OLED) can be selected by CMake for this
  half.
- [smoke:Memory-LCD screens] The screen's CS (active-high) is held LOW
  from boot on both halves; the protocol follows the Sharp app note
  (lemia doc 6845 p. 10-12): RAW command word (M0 = first bit clocked),
  line address in rev8 (CA0 first), 68 lines × 20 bytes (catalogue doc
  6844 p. 5: 160 × 68, H = data direction) transposed from the 68 × 160
  portrait; the panel's attachment waits for the radio to have created
  the SPI bus (deferred init); every screen transaction goes through the
  radio owner's lock and yields if it's busy; the bring-up test pattern
  (frame, solid block in the TOP-LEFT, 8 px checkerboard) is crisp and
  correctly oriented; the image stays frozen in light sleep and no wake is
  ever caused by the screen.
- [smoke:Memory-LCD screens UI] The right's screen is served at 1 s
  (model: 30 s gauge, dongle seen); VCOM upkeep is timestamped (~1 Hz),
  independent of the cadence of the task calling update(); at wake the
  screen hook only sets flags (the SPI bus still belongs to the radio),
  the image is pushed to the next tick of the screen task.
- [smoke:Memory-LCD screens UI] Both halves display in portrait. The RIGHT
  keeps its icon column (2026-09-26, Mae: icons were too small) — route
  (Montserrat 24 USB or radio symbol), ▲ "dongle seen" STICKY — a half is silent at rest, a
  time-stamped indicator would blink on every STATUS — which only drops
  after 3 consecutive transmissions with no ACK, never on a single
  isolated ESB refusal, and never lit before the first ACK; horizontal
  gauge, voltage in 4 characters ("4.1V", "4.1+" charging), 16×12 ⇆ when the
  TRRS 5 V is closed — the voltage STABILIZED over 30 s — [test:test_memlcd_model]
  a value different from the displayed one is shown once it has held for
  30 s: ADC oscillation never holds, a slow drift always eventually holds
  (a hysteresis around the displayed value had frozen it at 4.2 V for a
  whole night, 2026-09-15)) and its 60 px logo under the column. The LEFT
  is the "cave" since 2026-09-30 (plan 2026-09-30-left-screen-cave, spec
  2026-09-29-left-screen-redesign): dark (pale ink on black), a rocky edge
  top and bottom that no text ever enters, the 28 px Niphargus logo in the
  corner and the chest padlock beside it, then in words `USB` / `RADIO` and
  `SEEN` on its own line, the WATER-DROP gauge (13 x 16, filled from the
  bottom with a wavy surface, outlined 2 px when LOW) with the PERCENTAGE
  beside it — not volts: "87%", "+" charging, `FULL` charged, "?" unknown —
  the chest's rows, the layer name and its flags (Caps Lock only on the USB
  route, the host LED being unknown in RF and stale from the last USB
  session). Every layout decision is pure (`memlcd_cave_vue`,
  `memlcd_cave.h`) and host-tested [test:test_memlcd_model]
  [test:test_memlcd_safe_wrap]: every line inside the panel and its box,
  nothing overlapping, whatever combination of route, SEEN, link, chest rows,
  flags and names — the view trades the full rock for a thin one, then drops
  the corner logo, before it ever lets a line out; one font family, no text
  under Montserrat 12 (below it the panel's 1-bit threshold erases `:` `.`
  `,` `'` — tools/memlcd_sim's glyph-ink gate). Widths are measured with the
  width oracle, kerning included, equal to LVGL's own on every printable pair
  (tools/memlcd_sim's check_widths gate). Logos generated by
  scripts/gen_logo_memlcd.sh with NO palette (ALPHA_1BIT has none: the 8
  bytes it used to prepend shifted the 60 px logo by one row, and would
  shear the small ones); the rock, the padlock and the link by
  scripts/gen_memlcd_cave_assets.py.
- [smoke:Memory-LCD screens UI] A sleeping half's image is honest: the
  screen's `avant` sleep hook (veille_task.h, run before ANY `dormir`, while
  the radio has not yet taken the SPI bus lock for the sleep) re-reads the
  model and renders it NOW (lv_refr_now, on the sleep task's stack, 6144)
  with "zZ"; the frozen image says it is frozen (a ⇆ used to stay on after
  the cable was pulled, 2026-09-25): the right keeps its state under the zZ,
  the left shows ONLY the large 56 px logo and "zZ" — nothing that could
  pretend to be live [test:test_memlcd_model]. The wake re-read has no zZ: the diff redraws. NO battery reading for the other half: a
  user decision from 2026-09-14, and the ACK channel that would have
  carried it (DISPLAY frame) was removed along with it — the ACK stays
  bare outside sync. The screen only rewrites if a displayed field
  changes; an image refused because the bus is busy is pushed to the next
  tick, never lost; threshold and send are under the same mutex (LVGL
  flush and re-drive never transpose the same buffer at the same time —
  otherwise lines come out blank: "une partie de l'écran s'efface" (part
  of the screen goes blank), right, 2026-09-14).
- [test:test_memlcd_model] The left shows the chest's status (plan
  chest-link-v3 Task 8 — "SD c'est confusant", Mae 2026-09-29; cave layout
  2026-09-30): a 24 x 28 PADLOCK = chest present, beside the corner logo;
  under the battery row, in Montserrat 12: `..` while the chest is not
  READY, `?` on a protocol version mismatch; once READY the USB mode in plain
  words (`chest_mode_label`: DISK, PGP, OTP, FIDO, TOTP — MSC/OATH until
  then — the ACTIVE mode upper case once ARRIVED, the WANTED one lower case
  while a switch is PENDING (0xFF or not yet taken), `ERR` once active and
  wanted have disagreed for 8 reads straight), and `NO CARD` ONLY when the SD
  card is missing (no `SD` line when it is there); nothing without a chest
  [test:test_chest_mode_track][test:test_chest_mode_label].
- [test:test_memlcd_model][test:test_chest_view] The chest views — prompt,
  code, browser, in that priority, otherwise the normal screen — are built
  end to end from the chest's own raw register/DMA bytes through
  `chest_proto_parse` -> `chest_view_build` (pure, `main/comm/chest/
  chest_view.c`) -> the memlcd model -> `memlcd_cave_vue` (pure,
  `memlcd_cave.h`), pinned on the chest's V1/V9/V15/V16/L1/C1 vectors so a
  regression anywhere in the chain shows up as a wrong string, not just a
  wrong struct field (added after the chest found a RESET path publishing
  op_count 1 to the wire while the contract and V16 said 12 — the vectors
  alone proved the parser, never that the screen showed it). Readable type
  since plan Task 8 ("c'est tout petit", bench 2026-09-29): the PROMPT and
  the CODE take the WHOLE screen (nothing of the normal screen — logo,
  padlock, route, drop, layer name, flags — is drawn with them),
  the BROWSER keeps the top status (logo, route, drop) and the layer name.
  The cave (2026-09-30) keeps every rule of Task 8 and adds the spec's text
  security (`memlcd_cave_vue`, `memlcd_safe_wrap.h`, host-tested; widths from
  the width oracle, kerning included, `scripts/gen_memlcd_font_widths.py`,
  guarded by the `font-widths` tripwire brick and equal to lv_txt_get_width
  on every printable pair; a line keeps 2 px of its box free):
  - NO NAME LINE READS LIKE A CODE (bench incident 2026-09-29: `TEST:RFC6238`
    pixel-cut to `TEST:RFC` / `6238` was read as a truncated code): a name or
    label broken over lines never leaves a line with a digit and no letter
    (`:4021` is as bad as `4021`) [test:test_memlcd_safe_wrap]; the breaks
    are chosen (a dynamic programme over the break positions), `:` first,
    then `.` `@` `-` space `,` `;` `_`, mid-word only as a last resort, then
    as few lines as possible — `TEST:RFC6238` -> `TEST:` / `RFC6238`,
    `BANQUE:4021` -> `BANQU` / `E:4021`, `AB:123456789:CD` -> `AB:123456` /
    `789:CD`; pinned on a 20 000-string fuzz of issuer:account names and on
    every view that shows one. Only a text with no letter at all is exempt
    (it IS digits);
  - prompt (an operation is pending, beats a simultaneously-visible code —
    review I3, the code's digits never on a prompt): FULL screen, thin rock;
    the op (`chest_op_label`) at most Montserrat 20 (TOTP), smaller when it
    does not fit (RESET! in 18); a wavy divider; the CHEST's OWN label
    (register 0x14) — NEVER the OATH browser's cursor name, even when an op
    is pending while the cursor sits on a different, named account (V1: op
    named GITHUB, cursor moved to OVH:PRO — the prompt still reads GITHUB) —
    on ONE line in Montserrat 14 or 12 when it fits, else safe-wrapped at
    12 px; `N ACCTS` when more than one account is targeted (V16: op_count
    12, label "12 COMPTES" — both reach the screen); `PRESS` at the bottom.
    The label is NEVER cut (review C1 of Task 5 stands) and nothing overlaps
    it: room is made by shrinking the op title rung by rung to 12 px; past
    that (only wide glyphs — 34 `W` need 9 lines at 12 px) UNSCII 8, a bitmap
    font whose every glyph keeps its ink, safe-wrapped over the full width;
    and when NO safe split exists at all (a run of digits and punctuation
    wider than two lines — `AWS:123456789012`) a plain 8-character cut, the
    label still WHOLE, flagged `coupe_brute` in the view: the one place a
    line of digits may appear, on a prompt, which never shows a code
    (`_Static_assert`: 5 UNSCII lines + N ACCTS + PRESS always fit). The
    lines concatenated ARE the label, no `~` on a prompt ever; every scan is
    bounded by `strnlen(s, CHEST_LABEL_MAX)` (review M-b, ASan-pinned by
    test_chest_sanitized, which runs both memlcd suites);
  - code visible (after `K_OATH_CODE` AND `K_SEC_CONFIRM`, the account
    still under the cursor — chest_oath's own gate, re-proven here end to
    end; never on `coffre_code_visible` false, whatever `coffre_code`
    holds): FULL screen; the account name (the browser's copy) on up to 2
    lines, cut with a `~` — a cut line that would be left with digits and no
    letter is dropped rather than shown; the code as 3 + 3 digits in
    Montserrat 32 (6 digits) or 4 + 4 in Montserrat 24 (8 digits) — a size
    no name ever gets; the countdown as a water bar draining (seconds left /
    30) over a dithered shadow and `NN s`, the seconds ROUNDED UP (never 0
    while still shown) — gone at the deadline, gone on the first navigation
    key, never refreshed on its own, and BOUNDED even if the transport stalls
    (`chest_view_age`, review I2, below);
  - browsing (OATH active, a page cached, nothing pending/shown): the top
    status and the layer name (at most Montserrat 16) stay, then the
    cursor's 1-based position over the total, the account name — whole when
    the room holds it, else cut with a `~` under the same no-code rule — and
    `NO TIME` at the bottom whenever the chest's TIME_VALID bit is clear
    (V15) — never shown once the bit is set.
  - Countdown cadence note (corrected, review M-e): the memlcd halves'
    display-refresh cadence is `status_disp_periode_ms()` (`cadence.h`) —
    1000 ms at rest, but 100 ms whenever USB is present (`STATUS_DISP_USB_MS`),
    since a USB-powered half has no rest cadence to protect and follows the
    typing; the chest link task itself only rebuilds the view every
    `CHEST_POLL_MS` (250 ms, `chest_link.c`) or on a GPIO46 IRQ. Neither
    number changed for this review round, and `chest_view_age()` (I2)
    introduces NO new periodic wait of its own: it runs INLINE inside the
    already-scheduled `lire_modele()` call, at whatever cadence the caller
    already uses (100 ms on USB, 1000 ms on battery) — it is a pure
    computation on already-read data, not a new task or timer. Net effect:
    `coffre_code_secs` can now change (and trigger a real redraw via
    `memlcd_model_diff`) as often as the EXISTING refresh cadence allows
    (100 ms on USB, 1000 ms at rest) rather than only on the chest link
    task's own ~250 ms read cadence — more frequent while a code is visible
    (<= 30 s window), never a new periodic wait, so the tickless-sleep rule
    (nothing below `CADENCE_REPOS_MIN_MS`) is unaffected.
  - `chest_link.c` (transport) builds the view with `chest_view_build()`
    every round — also a round whose read failed on a busy bus, from the
    last block seen — and copies the result under a critical section
    (`s_view_mux`) for the display task to read — no field is filled by
    hand. Since plan Task 6 it passes the REAL `mode_wanted` and
    `mode_state` (`chest_mode_track`) and the OATH model fed by the DMA
    channel.
  - review I2, bounding a stale code: `memlcd_backend.c`'s `lire_modele()`
    calls `chest_view_age(&v, esp_timer_get_time() / 1000)` on the view
    snapshot right after `chest_link_view(&v)`, BEFORE mapping it into the
    model (`memlcd_model_set_coffre`, review M-c — the same pure mapping
    function now used by both the backend and the host tests instead of
    each hand-rolling its own copy). Without this, a code could stay on
    the panel past its deadline for as long as the transport keeps
    failing to read the chest (before Task 6, `chest_task`'s `continue` on
    a bus-busy round skipped `chest_view_build()` entirely, so the OLD
    code only re-evaluated `code_visible` on a round that actually got
    that far; the task now rebuilds every round, and the display-side
    ageing still bounds a task that stops running): `chest_view_age` ages the FROZEN SNAPSHOT on the display
    task's own clock read instead, independently of whether the link task
    ever reads the chest again — same millisecond clock both sides use
    (`esp_timer_get_time() / 1000`), same round-up rule as
    `chest_oath_code_visible` (`(remaining_ms + 999) / 1000`), wrap-safe.
    A visible code still SURVIVES one CORRUPT/ABSENT round on its own
    (armed on the live `chest_oath_t`, untouched by a transport hiccup —
    review M-d) but is always bounded by its real deadline regardless.

## Chest link (Niphar_chest)

- [smoke:Chest link] Presence = a USB host (the chest is powered by the
  left's USB): absent, the SPI device is removed and GPIO3 is an input —
  R48 pulls CS to the chest's rail, driving it into a dead rail would cost
  ~0.33 mA; no poller on battery (the link task blocks, the sleep task's
  1 s USB check hands presence over). If `spi_bus_add_device` fails on
  presence, the task retries every 1 s instead of blocking forever; a
  non-OK `spi_bus_remove_device` on the way out is logged. Present: a read
  every 250 ms or at once on a GPIO46 rising edge, under the radio owner's
  bus lock; an absent block is never acted on and never logged (the
  ordinary case), a corrupt block is logged once per presence session; a
  real K_SEC_CONFIRM press writes `{0x5A, instance}` at 0x38 in ONE write
  (contract §5 — the instance of the block that showed the op), delivered
  when the chest's counter moves, one retry after 200 ms at most.
- [smoke:Chest link] Protocol v3 on the wire (plan Task 6, 2026-09-29;
  contract Niphar_chest `docs/LINK_CONTRACT.md` §1/§5/§6/§13 at 46499d6).
  USB mode: K_CHEST_NEXT cycles the WANTED mode (none → storage → pgp →
  otp → fido → oath → none; shown as disk/pgp/otp/fido/totp) on a READY
  chest only; 0x3A (one byte, never in the
  confirm write) is rewritten whenever its read-back differs — the
  self-heal after a chest reboot or the confirm reclaim's word RMW; the
  wanted mode returns to none on presence lost (and therefore also after
  a keyboard reboot under a powered chest); the mode line tracks
  arrived/pending/ERR from 0x0D (`chest_mode_track`). OATH (active mode 5
  and READY): LIST on entry and whenever the cursor leaves the cached page;
  K_OATH_CODE → CODE(the entry's chest index) → the chest's prompt names
  the account → K_SEC_CONFIRM → the code and its countdown. A request is
  WRDMA (8 bytes) + WR_END, then the doorbell at 0x3C, all under one bus
  lock; a segment is RDDMA of exactly the published length + INT0, under
  one bus lock, into a static 512-byte buffer wiped after decoding. Every
  decision is `chest_round` (below). A chest key (K_OATH_*, K_CHEST_NEXT, a
  K_SEC_CONFIRM the chest takes) wakes the link task at once instead of at
  the next 250 ms poll; a navigation key moves the cursor and hides a code
  even on a round where the bus is busy. A CODE segment's digits never
  reach the log, diagnostic or not. `CONFIG_KASE_CHEST_DIAG` (off in every
  default) traces each DMA transaction, the whole first LIST in hex, each
  decode result and the task's stack high-water mark after the first LIST.
- [test:test_chest_round] The DMA channel's decisions, pure
  (`main/comm/chest/chest_round.c`), each mutant-proven: a segment is read
  only when 0x11 CHANGED and `chest_dma_segment_ok`, never on 0x10 alone,
  first contact only takes the reference; an unreadable announcement is
  consumed, not retried; one request in flight; a LIST is never re-sent
  for the same first index until a decision (OATH entry, navigation key,
  timeout retry — at most 2), so neither a tick nor a chest answering a
  page that misses the cursor drives a loop, and an empty chest asks
  nothing; nothing is sent outside OATH+READY; K_OATH_CODE with TIME_VALID
  clear sends nothing and is not kept for later; a CODE is never sent over
  a prompt already up or behind a request in flight (the press is
  dropped), only retried when the bus refused the send and dropped by a
  navigation key; the code request is cancelled when the chest does not
  arm it within 2 s, when another arming replaces it, when the prompt ends
  without a CODE segment in the same block, and on leaving OATH (which
  also resets the browser) — never on the wire timeout of a request that
  was armed; the doorbell is seeded from its read-back at first contact
  (a keyboard reboot under a powered chest never re-sends the value the
  chest already served) and advances only on a send that went out. Review
  round 1: a second K_OATH_CODE is served after a served code, whether the
  read sees 0x11 and the op clearing in one block or in two
  [test:test_a_second_code_after_a_served_code]; re-entering OATH asks
  LIST(0) at once, even with a LIST left in flight on leaving
  [test:test_reentering_oath_lists_again]; a navigation key during a LIST
  in flight waits for its answer or its timeout
  [test:test_nav_during_a_list_in_flight_waits]; a chest whose account
  count shrinks is followed to the clamped cursor's page, with retries of
  its own [test:test_a_shrinking_chest_is_followed_with_fresh_retries].
- [test:test_chest_oath_hidden_code_is_wiped] A TOTP code's digits are
  zeroed in the OATH model the moment it stops being shown — navigation,
  deadline, a LIST moving another account under the cursor — and the link's
  DMA buffer is wiped after every decode and after an RDDMA whose INT0
  failed.
- [test:test_chest_gate_notify_wakes_the_link_task] The chest gate calls a
  registered wake-up hook (chest_link.c's task notify) on K_CHEST_NEXT,
  K_OATH_PREV/NEXT/CODE, and on a K_SEC_CONFIRM the chest takes — not on
  one it does not; NULL (host, boards without the chest) calls nothing, so
  the gate stays FreeRTOS-free.
- [test:test_code_visible_est_un_veto] A TOTP code on the screen vetoes
  sleep (`VEILLE_VETO_CODE`, "code" in the HB; Mae, 2026-09-29): light sleep
  after 5 s is shorter than a 30 s window and the memory LCD keeps its
  image asleep. Posted by the chest link task from the view it builds,
  lifted when the code expires or hides (at most one 250 ms round late),
  and on presence lost — bounded by the code's own deadline.
