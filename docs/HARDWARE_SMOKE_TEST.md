# KaSe hardware smoke test

To be checked BEFORE every release / merge to `main`. Covers the runtime
that host tests cannot catch. Flash the board, check it off, keep a record
in the PR/release.

## Common (all boards)
- [ ] Boot with no boot loop (no repeated Guru Meditation)
- [ ] USB HID: every key types the right keycode (base layer)
- [ ] MO/TO/LT/MT layers: switch and return OK
- [ ] Scan: no ghost key, no dead key
- [ ] NVS preserved after an app-only reflash (keymaps/macros intact)

## V1 (round display + LED)
- [ ] Round GC9A01 screen displays without corruption
- [ ] LED strip: default animation OK
- [ ] No trace of the tamagotchi (removed in v4.1.0): no pet, no bars

## V2 / V2D (OLED I2C)
- [ ] OLED SSD1306 displays without artefacts
- [ ] V2D: COLS7/8 (GPIO21/4) scan correctly

## OLED multi-screen redesign (V2 / V2D) — tamagotchi removed
- [ ] Boot: "KaSe" splash + version ~2s, then HOME
- [ ] Splash ONLY at boot: turn the screen off/on (sleep→wake) → NO splash, straight back to HOME
- [ ] HOME: status bar (USB/BLE/RF connection + slot + CAP); **layer name in large text** (font_28) + "layer N" below it
- [ ] Layer change → HOME updates the name (NO MORE full-screen LAYER screen)
- [ ] Nothing overflows / overlaps (long layer name → truncated …)
- [ ] K_DISP_NEXT key (0x3F00, to be mapped): cycles HOME → STATS → HOME
- [ ] STATS: KPM/WPM move while typing, sparkline fills in, total grows
- [ ] V2D: screen turns off after ~30s of inactivity; wakes on the 1st keypress
- [ ] NO tama left AT ALL: no pet, no TAMA screen, no hunger/joy bars anywhere (V1 round included)
- [ ] No flicker / rebuild loop between HOME and STATS
- [ ] Pressing a BT key (switch/pair) while HOME is displayed → no crash, the screen rebuilds

## Dongle
- [ ] **5 V handshake on sleeping halves**: leave BOTH halves untouched for
      ≥ 30 s (no USB, nothing typed — they light-sleep), then plug the USB-C
      into one half and press ONE key on that half only. Expected: the bolt
      appears in the banner of BOTH screens within a second (the probe wakes
      the sleeping peer over UART1). Before the 2026-09-23 fix you had to
      type on both halves. ⚠ Pressing a key on the plugged half is required:
      the cable alone does not wake it (no USB wake source on the S3).
- [ ] RF link establishes with a half (pairing < 120s)
- [ ] NRF doesn't wedge after 5 min (watchdog OK)
- [ ] Idle wake (DFS): console at boot "DFS actif : 160 MHz en travail,
      40 MHz oisif" (DFS active: 160 MHz while working, 40 MHz idle); type
      for 1 min → "TX … acquittes" (TX … acked) ≥ 98 %; screen up to date;
      console readable at rest; sleep at 15 s and wake-on-key unchanged;
      multimeter in series: awake idle ≈ 13-19 mA (versus 28-42 before),
      asleep ≈ 250 µA; scanning stopped at rest: typing after a pause
      produces the first key with no delay or duplicate, holding a key then
      pressing another on the same row doesn't ghost, a key held at wake
      doesn't read across its whole row; sleeping between keystrokes: TRRS
      link: left on USB + cable → left console "state=2 5V=1" and the
      right's ACKs climbing, "sleep REFUSED … vetos=usb+link"; cable removed → "state=0 5V=0" in under a
      second; left on USB alone: "vetos=usb" and never any sleep; USB
      removed: "vetos=-", sleep at 15 s, console "tache de veille : tick
      1000 ms, 3 hook(s)" (sleep task: tick 1000 ms, 3 hook(s)) at boot; at
      rest the bench HB (CONFIG_PM_PROFILING=y) shows "light_sleep_counts"
      climbing (~90 per 10 s) and "light_sleep_reject_counts:0", typing
      stays instant
- [ ] A night on battery: a half loses on the order of a hundredth of a
      volt; 0.2 V = it didn't sleep. Console in the morning: "HB … slept=X
      s/n vetos=-" (HB … slept=X s/n vetoes=-) with X ≈ the length of the
      night, and "wake after N s of sleep"
      consistent; after 4 h with no typing: "deep sleep"
      then a restart on the first key (EXT1 wake, ~700 ms)
- [ ] set_id survives an erase_flash
- [ ] Fusion — local engine dormant: left on battery (RF route), type →
      the dongle types and the left console shows NO local processing (no
      tap-hold/combo/HID); plug a USB host into the left → it types locally
      from the next keypress on, the dongle falls silent; unplug → back to
      raw (binaries coming from `build_niphar_left` / `build_niphar_right` /
      `build_kase_dongle` — not a `_fusion` folder)
- [ ] Fusion — Config divergence reported: dongle keymap ≠ left's → dongle
      console logs "config DIVERGENCE" and
      KS_CMD_CONFIG_COHERENCE returns match=0; identical keymaps → match=1.
      Bench recipe (no dongle console needed): `scripts/kesp_cdc.py
      /dev/ttyACM0 setkey 1 0 0 0x0004` then `coherence` at 1 Hz. Done
      2026-09-21: match=0 with own_fp≠left_fp, back to match=1 on the NEW
      fingerprint once the left transmitted (twice: there and back)
- [ ] Fusion — ACK payload return channel: the dongle loads a known
      payload into the ACK (EN_ACK_PAY); the left, over wireless, reads it
      after every transmission and logs it. Go/no-go for nRF24 clones: if
      RX_DR never rises on the left side, automatic sync via ACK is
      impossible → fallback B. Done 2026-09-21 by the keymap sync below:
      the beacon and the 40 chunks only travel in ACK payloads
- [ ] Radio — one owner: dongle UNPLUGGED, type on the right → right
      console "fallback: switch TX -> LEFT KaSe.03" (fallback: switch TX ->
      LEFT KaSe.03) (then GAUCHE/DONGLE oscillation if nobody's listening);
      dongle plugged back in → ACKs resume without a reset ("TX n envois, m
      acquittes" [TX n sent, m acked] ≥ 95 % cumulative); left on USB →
      "fusion USB: listening for the re-emitted right half (PRX ch=0x4F
      KaSe.03)" and the right types through the left; USB removed → "fusion: back to PTX
      emission toward the dongle",
      both type through the dongle
- [ ] Short presses without doubling: one minute of brief taps on both
      halves → no doubled character on screen, `rfstat.py`
      (RF_STATUS[43..46]) `reappuis=0` (re-presses=0); a counter climbing on
      only one half = switch bounce (5 ms debounce), on both = a
      transmission regression
- [ ] Fusion — Keymap sync without a cable: change a layer on the dongle
      (SETLAYER) → left over wireless, NOTHING ELSE plugged in: in < 15 s
      the left console logs "sync keymap : balise" (keymap sync: beacon)
      then "40/40 recus … enregistree en NVS" (40/40 received … saved to
      NVS), and KS_CMD_CONFIG_COHERENCE (0x17) goes back to match=1 on the
      NEW fingerprint; afterwards no more ACK payload at rest (beacon cut
      off). Done 2026-09-21 (v4.2.0-beta.2): two rounds (key changed on the
      dongle with `kesp_cdc.py setkey`, then restored), `coherence` back to
      match=1 on the new fingerprint within one poll of the left's first
      transmission — the left was asleep in between (radio off), which is
      why `age_ms` climbed to ~60 s first; the sync needs an awake left

## Half (left / right)
- [ ] Low battery: build a half with `BATT_FAIBLE_DV`/`BATT_CRITIQUE_DV`
      shifted above the real voltage (bench, do not commit) → console
      "battery: LOW" then, when critical, "light sleep"
      at ~5 s; screen: voltage unchanged, gauge readable with a thick
      border (the alert); left on USB + TRRS → "state=0 5V=0", 0 probes; real
      thresholds reflashed → the link comes back up
- [ ] Battery gauge: console at boot "batt: gauge: NN dV" (batt: gauge:
      NN dV) with NN plausible (36-42) and within ±0.1 V of a voltmeter on
      the battery; CDC BATTERY (dongle) gives BOTH halves with a fresh age
      (left ~1 s, right ≤ 30 s); the right still falls asleep at 15 s
      despite its slow STATUS; a half that's off shows back as "inconnue"
      (unknown); battery charging → after ≥ 2 min at ≥ 4.15 V, charging = 2
      (FULL)
- [ ] Memory-LCD screens (left AND right): at boot, console "panneau
      attache (bus radio pret), mire ecrite" (panel attached (radio bus
      ready), test pattern written) AFTER "radio PTX … init OK"; crisp test
      pattern across the whole panel (1 px frame, solid 16×16 block in the
      TOP-LEFT of the portrait, 8×8 checkerboard elsewhere), no stray
      pixel; typing during a refresh loses no keypress; in light sleep the
      image stays frozen and readable, the board still sleeps at 15 s; on
      wake the screen comes back to life
- [ ] Memory-LCD screens UI: left → banner "RF ▲" (or "USB") + gauge +
      voltage, layer name in lines of 4 (e.g. "DVO / RAK"), "L0"; right →
      same banner, crisp centred Niphargus logo; changing layer (MO held)
      updates the name in < 200 ms without losing a keypress; right served
      at 1 s: no zone greys out in 2 min (VCOM ~1 Hz), the image comes back
      to life within the second following the first key after a sleep
- [ ] First key after sleep: let the half sleep — 15 s for the short case
      AND at least 10 minutes for the long one (the failure of 2026-09-16..21
      only showed after minutes: esp_timer replay of missed periods before
      the capture) — then type ONE brief key → the character comes out (not
      swallowed) and nothing stays stuck; console: "wake: 1 key(s) captured",
      and with `KASE_VEILLE_DIAG` "lines at exit" non-zero. Left AND right,
      over wireless (fusion). Done 2026-09-21 on the left (761 s) and both
      halves typed
- [ ] e-ink displays the 'PAIRED' splash at pairing
- [ ] e-ink dashboard: L/R/USB + battery, without corruption
- [ ] Trackpad (if present): cursor, L/R/M click, scroll
- [ ] BLE pairing OK + reconnection after deep sleep
- [ ] Power Phase 1: typing stays instant (scan/TX unchanged)
- [ ] Power Phase 1: after ~3 s with no typing, the heartbeat slows down (half console: TX heartbeat spaced out) with no loss of link
- [ ] Power Phase 1: immediate return of the 100 ms heartbeat on the first keypress
- [ ] Power Phase 2: enters light sleep after ~15 s of inactivity (silent console)
- [ ] Power Phase 2: measured current drop (WiFi + NRF off) — note the value
- [ ] Power Phase 2: 1st keypress wakes + registers (acceptable latency), subsequent ones at full speed
- [ ] Power Phase 2: e-ink readable, frozen during sleep, becomes live again on wake
- [ ] Power Phase 2: no key stuck/ghosted on wake; release handled
- [ ] Power Phase 2: if wake doesn't trigger on a key, invert the column GPIO polarity (see half_scan_arm_key_wake BENCH-TUNE)
- [ ] Power Phase 2: left + right independently

## BLE (relevant boards)
- [ ] Host pairing OK, types with no drop for 1 min
- [ ] BT slot switch OK

## Boards — tooling

- [ ] New board from the template: `scripts/new-board.sh demo` creates
      `boards/demo/` and `test/test_board_contract_demo.c`; without any other
      edit `./scripts/check.sh --fast` runs its pin contract and
      `./scripts/check.sh` builds it (8 boards). Then delete the folder, the
      test unit and its two registration lines. Done 2026-09-20.

