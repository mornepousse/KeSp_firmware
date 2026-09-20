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
- [test:test_keyboard_cadence] The keyboard task runs at 10 ms as long as a
  timer can still fire — less than 1.5 s since the last keypress (covers
  tap-hold and tap-dance 200 ms, leader 1000 ms), a USB host present,
  matrix test mode — and at 100 ms at rest; a matrix change notifies it,
  the first key never waits. At 100 Hz its 10 ms loop was leaving ONE tick
  free when automatic light sleep requires three: SLEEP mode 92 % of idle
  time and light_sleep_counts = 0 (bench 2026-09-16).
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
  (`usb_presence_brut`) drives USB/RF routing, the 5 V TRRS source and the
  sleep veto: with the VBUS bridge soldered (`KASE_VBUS_SENSE`) the GPIO
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
- [test:test_veille_veto] Sleep veto registry (`power/veille_veto.h`,
  pure): one state per name (usb, lien, sync, test, pair), a posted veto
  blocks all sleep, lifting an absent veto has no effect, names bounded
  for the HB (all five fit in its 24 bytes). Wired into the single sleep
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
  0 column 159; the layer name truncates to 4 characters × 3 lines then
  "…", never zero lines, NULL-safe; the model only triggers a redraw if a
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
- [smoke:Memory-LCD screens UI] Both halves display in portrait: a banner
  (RF/USB route, ▲ "dongle seen" STICKY — a half is silent at rest, a
  time-stamped indicator would blink on every STATUS — which only drops
  after 3 consecutive transmissions with no ACK, never on a single
  isolated ESB refusal, and never lit before the first ACK; local gauge
  and voltage, the voltage STABILIZED over 30 s — [test:test_memlcd_model]
  a value different from the displayed one is shown once it has held for
  30 s: ADC oscillation never holds, a slow drift always eventually holds
  (a hysteresis around the displayed value had frozen it at 4.2 V for a
  whole night, 2026-09-15)), a centre section (LEFT: layer name in lines
  of 4 + "Ln"; RIGHT: 60 px Niphargus logo, centred, generated by
  scripts/gen_logo_memlcd.sh). NO battery reading for the other half: a
  user decision from 2026-09-14, and the ACK channel that would have
  carried it (DISPLAY frame) was removed along with it — the ACK stays
  bare outside sync. The screen only rewrites if a displayed field
  changes; an image refused because the bus is busy is pushed to the next
  tick, never lost; threshold and send are under the same mutex (LVGL
  flush and re-drive never transpose the same buffer at the same time —
  otherwise lines come out blank: "une partie de l'écran s'efface" (part
  of the screen goes blank), right, 2026-09-14).
