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
- [ ] **Sleep current**: ammeter in series with the battery, no USB, no TRRS,
      keyboard untouched ≥ 20 s (console: one `light sleep` line, then
      silence). Reference 2026-09-25, left half: 7.5 mA before any fix, 5.0
      mA with the VDD_SPI power-down (the in-package PSRAM leak), 1.75 mA with
      the link's UART released during sleep (aa5a8170), **0.67 mA** with the
      unconditional USB withdrawal, 0.2 mA in deep sleep. Measure RIGHT AFTER
      A RESET (no USB since boot): that is the case the USB fix is about —
      after a USB session the old firmware also read 0.67 mA, by accident.
      Then plug the USB-C: it must enumerate and type. Key held (steady, the
      veto keeps it awake): 36 mA with the matrix gptimer on APB, 27-33 mA
      on the XTAL, **24 mA** with the 5 ms scan (28 mA with several keys
      down, 2026-09-25). If
      ~5 mA comes back, the UART is not being released (look for "not
      released in time" in the console). After flashing, also type one key after a 20 s pause: the flash
      power-down lengthens the wake (the first-key-lost path).
- [ ] **5 V handshake on sleeping halves**: leave BOTH halves untouched ≥ 10 s
      (they light-sleep at 5 s), plug the TRRS cable and the USB-C into one
      half, press ONE key on EACH half. Expected: the bolt on BOTH screens
      within a second. A key on one half only is not enough, by design (the
      link's UART is released during sleep to save 3.4 mA).
- [ ] **Held key**: hold Backspace ≥ 10 s with nothing else — the host keeps
      auto-repeating without a stutter, the heartbeat shows `vetos=key`, no
      `light sleep` line until release.
- [ ] **RF trace (dongle)**: remapping software closed, `fuser` the dongle
      port first. Send `KS BA [01 1E 00]` (start, 30 s) → `KR BA 00` ack
      `[01][session u32][1E 00]` (the session +1 at every start, repeated by
      every periodic record of the run), then a periodic batch (kind 3) about every second; type on both
      halves → RX batches (kind 2) with emitters 0 and 1, `seq` climbing,
      `delivered` set on MATRIX frames; other CDC commands (`rfstat`) still
      answer with intact CRCs in the middle of the stream. After 30 s the
      last periodic record arrives and the stream stops by itself; `KS BA
      [00]` while off → ack; `KS BA [02]` → `ERR_INVALID`. Reboot during a
      trace → nothing is pushed after the reboot until a new start. Typing
      unchanged with the trace on (no double, no lost key). Back-pressure:
      start 120 s, suspend the reader (Ctrl-Z) for 20 s while typing, resume
      → every frame read decodes (no CRC error), the next periodic record
      reports `drops` > 0, and `rfstat` answers intact. Presence: start 120 s,
      close the port, reopen 5 s later → the first bytes read are the reply
      to the first command sent, no stale trace frame; no trace frame at all
      until a new start.
- [ ] **RX settle — dongle core 0 not burnt** (2026-10-21, per-radio settle):
      dongle on its CDC, `rf_scope capture --dongle /dev/ttyACM0 --duration
      60`, nothing typed for the first 30 s, then type for 30 s. The report's
      DONGLE section prints `RX health: FIFO width flushes N   engine gap max
      G ms   settle wait S ms`. Expected: at rest `settle wait` ≈ 0 ms per
      second (timer passes wait nothing; 88d49728 burnt ~500 ms/s), `engine
      gap max` ≤ 12 ms (the engine ticks every 10 ms; a starved core 0
      shows up here first), `FIFO width flushes` 0 — a growing count is the
      path that lost 6 % of acked frames at a 200 µs settle. While typing,
      `settle wait` stays a few ms per second (≤ 0.5 ms per frame), the gap
      still ≤ 12 ms. With the Conchodytes mouse moving (1 kHz): gap still ≤ 12
      ms, keys typed during the motion arrive once, tap-hold still resolves.
      No `BENCH` line in the report (a non-shipped nRF24 knob would print
      one; a half's HB would end with ` BENCH-RF(...)`).
- [ ] **RX settle — first attempt under load**: `rf_scope stress --dongle
      /dev/ttyACM0 --left /dev/ttyUSB0 --scenario held --duration 30` →
      first-attempt success ≥ 95 % (2026-10-21 reference with the
      unconditional 500 µs: 97-99.9 %), retries/frame ≤ 0.05, no loss, and
      the editor stays empty. Same on the right half. A drop below 95 % after
      a change of the dongle's RX path is a regression of the settle.
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
- [ ] RF trace (half) — ESP-Prog on the half's prog connector, nothing else
      holding the port. (1) Trace OFF (fresh boot, no request sent): no
      frame on the console, the half sleeps at 5 s as before, `HB … slept=` grows as on the previous
      build, and the ammeter in series with the battery reads the usual
      light-sleep current (left ~0.67 mA, 2026-09-25) — the UART0 RX reader
      must not add anything. (2) Send `RF_TRACE start(120)` (KS frame, cmd
      0xBA, payload `01 78 00`) and repeat it every 250 ms until the KR ack
      `4B 52 BA 00 07 00 01 <session u32> 78 00 <crc>` comes back — press a key if the half sleeps
      (bytes sent while it sleeps are not received); console `rftrace: ON
      for 120 s`. (3) Type on that half: one TX record per transmission
      (left on battery: PTX, `flags=0`; left on USB: excursion records,
      `flags=1`, channel = the dongle's), a periodic record every ~1 s, the
      console text still readable between frames. (4) Stop typing: a
      SLEEPING record at the 5 s sleep, nothing while asleep, WOKE on the
      next key. (5) At 120 s: console `rftrace: OFF (duration over)`, no
      more frames. (6) Start again, then reset the half: no frame after
      the reboot until a new start. (7) With the trace OFF again, the
      sleep current and `slept=` are back to (1). (8) Build with
      `KASE_RF_TRACE_HALF=y` (the default), trace never started, ESP-Prog
      UNPLUGGED from the half: light-sleep current = 0.67 mA as before (left,
      2026-09-25) — a floating RX0 would wake the drain task; GPIO44 keeps
      its pull-up. Tracing ON perturbs timing slightly: never measure
      current with a trace running.
- [ ] RF stress — dongle on its CDC, ESP-Prog on the half under test,
      `fuser` both ports first, remapping software closed. On the HOST,
      focus a text editor (an empty document) and leave it focused for the
      whole item: **NOTHING may be typed in it, ever** — a single character
      fails the item (a stress frame reached the host as a keystroke).
      Run each scenario 30 s, on the left then on the right:
      `rf_scope stress --dongle /dev/ttyACM0 --left /dev/ttyUSB0 --scenario
      burst --duration 30` then `max`, `held`, `rate --rate 200` (`--right`
      for the right half). For each: (1) the half's console logs `rfstress:
      session N: <pattern> …` and, at the end, `rfstress: session N stopped
      (duration over): S sent (A acked, R MAX_RT), U unavailable`; (2)
      during the run, `HB … vetos=stress` (with whatever else holds it), and
      in the first HB after the end `stress` is gone — then the half sleeps
      at 5 s as usual; (3) the report's "stress" section shows the frames
      offered and delivered, the dongle's `stress` counter close to the
      half's acked count, the loss, retries and latency under load; (4)
      typing on the OTHER half during a `max` run still works, keys arrive
      once (the radio is loaded, not the engine); (5) the editor is still
      empty. Also: left on USB (chip in PRX): a `burst` run goes out by
      excursions (`flags=1` TX records), still nothing typed. A `stop`
      mid-run (Ctrl-C in the tool) → `stopped (stop command)` and the veto
      goes. With the trace stopped and no run, sleep current unchanged.
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
      (unknown); battery charging ON USB → "+" at once, then after ≥ 2 min
      at ≥ 4.15 V, charging = 2 (FULL)
- [ ] Battery percentage (2026-09-30): left on battery, USB unplugged, a
      full cell (4.1-4.2 V) → the reading settles on ONE value (100 % or
      95 %) and stays there for 10 min — no 90 / 100 / FULL alternation, no
      "+", no `FULL` anywhere without USB (screen, CDC BATTERY charging = 0);
      the percentage is a multiple of 5 and only ever goes DOWN on battery
      (a drop shows ≥ 60 s after the cell got there); plug USB → "+" within
      ~1 s, the percentage may climb (60 s per step); a charge on a WALL
      charger (not seen by the USB rule) re-anchors the reading once it is
      ≥ 20 % above for 5 min; after a reboot the first reading shows at once
- [ ] Memory-LCD screens (left AND right): at boot, console "panneau
      attache (bus radio pret), mire ecrite" (panel attached (radio bus
      ready), test pattern written) AFTER "radio PTX … init OK"; crisp test
      pattern across the whole panel (1 px frame, solid 16×16 block in the
      TOP-LEFT of the portrait, 8×8 checkerboard elsewhere), no stray
      pixel; typing during a refresh loses no keypress; in light sleep the
      image stays frozen and readable, the board still sleeps at 15 s; on
      wake the screen comes back to life
- [ ] Memory-LCD screens UI: RIGHT, the same cave since 2026-09-30 — dark,
      the rock edges, the SAME band as the left at the top (the drop with
      the same rules, the route icon beside it, `⇆` while its TRRS 5 V is
      closed) and the large Niphargus logo centred under it, no text at all
      above 15 % and no voltage; the right's route icon: the plug while ITS
      USB is up (plug the cable into the right: plug; its keys still reach
      the host by radio), otherwise 3 waves over a filled dot while the
      dongle acknowledges it, 2 waves over a hollow dot once it has fallen
      back to the left (dongle unplugged, left on USB: type a few keys on
      the right) — the icon may take a key or two to change, it follows the
      ACKs. LEFT, the cave
      (2026-09-30): dark, rock edges top and bottom with no text touching
      them, the small logo top-left, then ONE band under it (2026-09-30):
      the large water drop (full at 100 %, a wavy surface part-way, NO
      number above 15 %; at 15 / 10 / 5 % the digits INSIDE the drop, clear
      of its outline and of the water; a thick outline when LOW, no
      blinking), the route icon beside it (USB: a plug; radio with the
      dongle seen: 3 waves over a filled dot; dongle unplugged: 2 waves over
      a hollow dot), `⇆` beside that while the TRRS 5 V is closed; on USB a
      `+` in the drop while charging (readable at any level), a full drop
      with no mark once FULL, never a `+` or a full drop on battery; no
      words `USB` / `RADIO` / `SEEN`; readable in LOW light
      (Mae's verdict on the dark palette is still open); the layer NAME
      small, one size (Montserrat 14: "BASE"; "NAVIGATION" as NAVIG /
      ATION), the base
      or locked layer only — holding a MO does NOT change it; TO /
      Layer Lock does; on USB a change shows within ~0.1 s, on battery
      within ~1 s; Caps Word shows the hollow up-arrow icon under the name,
      Caps Lock the same arrow over a bar (over USB only), both side by side
      when both, no "CAPS" / "CW" words; a one-shot Shift armed and left
      alone shows "S", a one-shot layer "L3"; when a half goes to sleep its last image carries "zZ" (on
      BOTH halves since 2026-09-30 ONLY the large logo and "zZ"), and "zZ" goes away at the first key; right served
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
- [ ] Held key on battery (LEFT, route=RF): unplug USB, hold a key down for
      more than the 5 s light-sleep threshold → HB keeps showing
      "vetos=…key…", NO "light sleep" line while it's held, screen does NOT
      flash to "zZ"; release → veto drops, normal sleep resumes after 5 s of
      real inactivity. Regression bug (2026-09-30): the fusion early-return
      (left off USB never types locally, `matrix_scan.c`) used to skip the
      `VEILLE_VETO_TOUCHE` post entirely on every scan, so a key held on
      battery never blocked sleep at all — the half slept key-down, woke at
      once on the held row (cause=0/7), and looped every ~5 s. On USB the
      bug did not show (the early return isn't taken), which is why an
      earlier capture over USB looked fine
- [ ] **USB enumeration after a sleeping plug-in** (2026-10-01, the real-world
      case Mae hit): USB-C plugged into the PC but the host never mounted
      (e.g. it was slow, or a previous explicit sleep aborted it) → the half
      goes to explicit light sleep at 5 s (no `VEILLE_VETO_USB`, since
      `tud_ready()` is false pre-mount). Press ONE key to wake it → within
      ~2 s the host's USB icon / `lsusb`/`dmesg` shows the device enumerated.
      Repeat 10× in a row (sleep ≥ 10 s each time, one wake key) — must
      succeed every time, not "usually". Console around the wake: "wake"
      (veille.c) then "usb: bus activity, holding light sleep off until
      mount" then "USB host mounted: APB held at 80 MHz" (`pm_dfs.c`) —
      and critically NOT a repeating "light sleep" / "wake" pair once a
      second while waiting (that flicker is the regression this guards
      against: `VEILLE_VETO_USB_ENUM` in the HB `vetos=` string while
      holding, e.g. `vetos=enum`, gone once mounted or after 2 s if the host
      gave up). Root cause chain: (1) the ESP32-S3's USB-OTG controller is
      not a documented light-sleep wakeup source, so a half idle between
      keystrokes (automatic light sleep, cadences at rest ~1 s) misses the
      host's bus reset/SETUP outright — fixed by watching D+/GPIO20 as a
      GPIO wakeup + interrupt source (`usb_wake_guard.c`); (2) EXPLICIT sleep
      is worse: `veille_legere_entrer()` calls `tud_disconnect()` before
      every explicit sleep, which actively pulls D+ LOW
      (`dcd_disconnect()`/`USB_WRAP.otg_conf.dp_pulldown=1` in TinyUSB's S3
      DWC2 port) — left armed, that would make `esp_light_sleep_start()`
      return instantly every time, producing the once-a-second flicker just
      described; fixed by a veille sleep/wake hook that disarms D+'s GPIO
      wakeup before `tud_disconnect()` and re-arms it after `tud_connect()`,
      plus `VEILLE_VETO_USB_ENUM` blocking the explicit sleep outright while
      a handshake may be in progress.
- [ ] **USB enumeration while typing**: type continuously on battery (keeps
      automatic light sleep short between keystrokes), then plug the USB-C
      into the PC mid-typing → enumerates within ~2 s, not ~30 s; same
      console lines as above.
- [ ] **No battery regression, no cable**: on battery with the USB-C never
      plugged in, HB `slept=` keeps growing normally and `light sleep` lines
      are never immediately followed by a wake with `cause=GPIO` (that would
      mean D+ is triggering spuriously) — nothing should wake or hold a lock
      or post `enum` in `vetos=` with no host present. D+ is pulled HIGH
      locally by the device-side full-speed pull-up when no cable is
      attached (not bench-proven — if `usb_wake_guard_tick` ever logs a hold
      with nothing plugged in, that pin needs a different wakeup level or a
      debounce).
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
- [ ] Chest link (left, chest flashed, USB-C plugged): beside the logo a
      BIG padlock (24 x 28, it must not touch the logo), under the battery
      row the USB mode in plain words ("PGP" upper case once a mode is
      mounted, lower case while the switch runs); with the SD card in, NO
      card line; with the card out, "NO CARD" under the mode; `gpg
      --card-status`, then `echo t | gpg --sign` → the prompt takes the
      WHOLE screen: "SIGN" at the top, a wavy divider, the account's REAL
      label from the chest (not a placeholder), "PRESS" at the bottom; a
      press on K_SEC_CONFIRM signs; no press → 6985 after 15 s and the
      normal screen comes back (logo, padlock, the drop + route band, layer);
      radio and screen keep working meanwhile; unplug USB → padlock gone; on
      battery the sleep current is unchanged (GPIO3 released).
- [ ] Chest views, what only the physical screen shows (host tests pin the
      STRINGS, their widths — kerning included — and their positions;
      tools/memlcd_sim renders them with the firmware's own engine; the
      glass is the last judge): TOTP browse (`i/total`, the name, `NO TIME`
      when the time is not set, the logo and the drop + route band still
      visible above it, a two-line name such as `OVH:PERSO` whole as
      `OVH:` / `PERSO`, nothing running into the rock); a `K_OATH_CODE` prompt on
      the `TEST:RFC6238` test account reads `TEST:` / `RFC6238` — NEVER a
      line of digits alone; a prompt on an `AWS:123456789012` test
      account shows `AWS:` then two lines each starting with the `↳`
      continuation mark — the mark reads as an arrow, never as a `4`, and
      nothing of the label is missing; a label near the chest's 34-character maximum
      spans its 12 px lines with nothing clipped at either edge; a label of
      wide letters (a `WWW…` test account) switches to the small UNSCII 8
      type and still shows WHOLE (no `~` on a prompt); a RESET shows
      `RESET!`, the label, `N ACCTS` and `PRESS` without overlap; the code
      screen: the name on top, the 6 digits LARGE as two rows of 3, the
      `Jetable:huit` 8-digit code as two rows of 4, the water bar draining,
      `NN s` under it — nothing of the normal screen shows through; every
      character used renders (no tofu box): digits, upper/lower case,
      space, `~`, `/`, `?`, `:`, `!`, `@`, `.`.
      Stack (console on the ESP-Prog): after a prompt on the longest label
      and a browse of a long name, read the heartbeat's
      `HB status_disp stack: N bytes never used (new low)` — the status
      display task has 6144 bytes; the view's own deepest path measures
      ~1.6 KB of frames (xtensa `-fstack-usage`, 2026-09-30), LVGL's render
      and flush come on top. Below ~1 KB free, raise the task's stack.
- [ ] Chest link protocol v3 (left flashed with the Task 6 transport, chest
      flashed with its v3 head, USB-C plugged; for the raw traces build the
      left with `CONFIG_KASE_CHEST_DIAG=y` and read the console detached,
      `dtr=rts=False`), in this order — stop at the first that fails, the
      later ones depend on it:
      1. idle block: the padlock alone (no mode, card in); the chest's `link` dump matches
         the reference idle block `4E 49 50 48 03 05 00…` with CRC `76 A3`
         at 0x36 (version 3, SD|READY, nothing armed) — anything else at
         idle is the wire, not the composition;
      2. `K_CHEST_NEXT` pressed five times from none: the mode line walks
         `disk`/`DISK`, `pgp`/`PGP`, `otp`/`OTP`, `fido`/`FIDO`,
         `totp`/`TOTP` (lower case while each switch runs);
         unplug/replug → back to none;
      3. in OATH: the first LIST arrives with no press — the bottom area
         shows `1/total` and the first name; `K_OATH_NEXT`/`K_OATH_PREV`
         walk the names and wrap; past the first page a new LIST follows
         (diag: one WRDMA per page, never one per 250 ms; paste the raw
         first LIST for the chest session, success or failure);
      4. `niphar-oath set-time` → TIME_VALID (the `NO TIME` hint goes);
      5. `K_OATH_CODE` → the prompt names the account under the cursor
         (full screen: `TOTP` large, the CHEST's label, `PRESS`) →
         `K_SEC_CONFIRM` on the left → the code, full screen, large, with
         its countdown bar and seconds;
      6. no press after `K_OATH_CODE` → the prompt expires after 15 s and
         NO code ever shows; `K_OATH_CODE` alone never shows a code;
      7. the code disappears at the end of its window (countdown reaches
         the end, the panel clears even with no key touched — the half does
         not sleep meanwhile, HB `vetos=…code`), and at once on
         `K_OATH_NEXT`/`K_OATH_PREV`; it is never refreshed without a new
         `K_OATH_CODE` + `K_SEC_CONFIRM`;
      8. after a chest replug and BEFORE `niphar-oath set-time`:
         `NO TIME` in the browser, and `K_OATH_CODE` sends nothing (diag:
         no WRDMA CODE);
      9. a RESET armed from the host → the prompt shows `RESET!`, the
         label, `N ACCTS` with the number of accounts and `PRESS`;
      10. `K_SEC_CONFIRM` placed on the right half does nothing: the prompt
          stays, the chest's counter does not move;
      11. typing the code (left on USB, a text editor focused on the host):
          `K_OATH_CODE` → `K_SEC_CONFIRM` → the code shows → a SECOND
          `K_OATH_CODE` types its 6 digits (then an 8-digit account: its 8
          digits) into the editor, NO Enter, and the code disappears from
          the screen at once (back to the browser); a third press is a new
          request (the prompt again), it types nothing; the console shows
          `TOTP code typed (6 digits)` and never the digits; with
          `K_OATH_CODE` ALSO mapped on the right half: a right-half press
          while the code shows types NOTHING and the code stays (then the
          left key types it), and a right-half press with no code shows the
          prompt as usual;
      12. let a code run out (countdown to the end, panel back to the
          browser), then `K_OATH_CODE` → nothing is typed (the editor stays
          as it was); after `K_OATH_NEXT` hid a code, `K_OATH_CODE` is a
          request, never a type;
      13. cancelling a prompt (chest at 3da17cc or later): note the cursor
          position, `K_OATH_CODE` → the prompt → `K_OATH_NEXT` → the prompt
          is gone within ~1 s, back to the browser at the SAME cursor
          position, no code ever shows; the console shows `prompt cancel
          written (instance N)`; then a host request (`echo t | gpg --sign`,
          or an OATH calculate from the host) → prompt → `K_OATH_PREV` from
          the RIGHT half → prompt gone, the host gets 6985 (gpg: card
          error / operation cancelled) at once instead of after 15 s.

## BLE (relevant boards)
- [ ] Host pairing OK, types with no drop for 1 min
- [ ] BT slot switch OK

## Boards — tooling

- [ ] New board from the template: `scripts/new-board.sh demo` creates
      `boards/demo/` and `test/test_board_contract_demo.c`; without any other
      edit `./scripts/check.sh --fast` runs its pin contract and
      `./scripts/check.sh` builds it (8 boards). Then delete the folder, the
      test unit and its two registration lines. Done 2026-09-20.

