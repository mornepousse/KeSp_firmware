# Left-half screen redesign — "cave" (design record)

Status: implemented and validated on the panel by Mae (2026-09-30, 8f797a10); the
right half in the same style implemented the same day, still to be judged on its
panel ("Right half" below). Supersedes the layout of chest v3 Task 8
(commits d9de081b, 0616260b — their mode names and width oracle stay).

## Why

Mae, after flashing 0616260b: "pas très beau, il faut revoir complètement
l'interface". All four complaints: too cluttered, mixed fonts, layout (icon
column, separators, imbalance), ugly icons. Scope: the whole LEFT screen
(normal screen + every chest state). The right half was out of scope at
first; since 2026-09-30 it is IN scope (see "Right half" below).

## Panel and tooling

- Sharp LS011B7DH03, portrait 68 × 160, 1 bit, reflective (no backlight).
- Host simulator (not in the repo yet): the real LVGL 8 from
  `managed_components/lvgl__lvgl`, thresholded exactly like `flush_cb`
  (`lv_color_brightness < 128`), PNG ×4. Lives in the session scratchpad
  `memlcd_sim/` — to be moved under `tools/memlcd_sim/` when the design is
  chosen, so the firmware and the mockups share the same draw code.
- Each design is a module `build(lv_obj_t *scr)` / `draw(const memlcd_model_t *, batt_pct)`
  depending only on LVGL + `memlcd_model.h`.

## Decisions (Mae)

1. **Identity**: the keyboard sits in a TRANSPARENT case; Niphargus is a blind,
   pale cave shrimp. The UI must feel like a cave, visibly — not just a black
   background.
2. **Logo**: her real logo, `~/Documents/GitHub/Niphargus/images/niphargus_logo.svg`,
   rasterised like `scripts/gen_logo_memlcd.sh`. No drawn mascot (the lv_arc
   shrimp of round 1 was rejected: unrecognisable). 28 px is the smallest size
   that stays recognisable; 56 px for the sleep screen.
3. **Cave elements, all three**: a rocky edge (stalactites top, rock floor
   bottom, never touching text); underground water (battery gauge and code
   countdown are a water level with a wavy surface in an outlined vessel);
   ordered (Bayer) dithering for stone/shadow, never behind text.
4. **DARK chosen** (Mae, 2026-09-29) — inverted, pale ink on black; still to be checked on the real panel in low light. The light variant stays in the simulator only.
5. **Full screen** only for the confirmation prompt and the code; the browser
   stays in the lower part and the top status (route, battery) stays visible.
6. **One font family** (Montserrat), a size ladder chosen by width; words in
   place of cryptic abbreviations: `USB` / `RADIO` (+ `SEEN` for the dongle),
   modes `DISK PGP OTP FIDO TOTP` (lower case in flight, `ERR`), padlock for the
   chest, `NO CARD` only when the SD card is missing.
7. **CPU stays at 160 MHz**. Animation only on USB (LVGL 10–20 fps allowed
   there); on battery 1 Hz and every state must read correctly frozen.

## Security rules for text (hard)

- The chest's label on a prompt is shown WHOLE, never cut, nothing overlapping
  it; the font shrinks until it fits the band between the op and `PRESS`
  (asserted in code, tested).
- The prompt shows the CHEST's label, never the browser's copy.
- **A name line never looks like a code** (bench incident 2026-09-29: Mae read
  `TEST:RFC6238` pixel-cut to `TEST:RFC` / `6238` as a truncated code). Names
  break at `:` first, then spaces/punctuation, mid-word only as a last resort;
  no line may contain digits without a letter (`:4021` is as bad as `4021`);
  when the only break at `:` would strand the digits, the break moves back into
  the letters. Proof cases: `TEST:RFC6238` → `TEST:` / `RFC6238`;
  `BANQUE:4021` → `BANQU` / `E:4021` (safe, not pretty).
- **Continuation mark** (Mae, 2026-09-30): when a label cannot be both whole
  and free of digits-only lines (`AWS:123456789012`), it stays whole and every
  line that continues the previous one starts with a drawn `↳` mark (1-bit
  bitmap — the built-in fonts have no such glyph), so a line of digits reads as
  the rest of a name, never as a code. Implemented 2026-09-30 (fix round
  1): the mark is drawn on EVERY continuation line of a prompt's label, not
  only when digits force it — one meaning everywhere, and it fits the band
  (the 34-character brief label keeps Montserrat 12, at the price of one
  mid-word break `ALICE.M` / `↳ARTIN@`). 6 × 11 px, 1 px stem rising above
  the digits and an open arrow low on the line: a first 6 × 6 version read
  as a `4` in the simulator. Its 8 px (mark + gap) come off the line; the
  8-character raw UNSCII cut is gone. Only exception left: a label that
  STARTS with a digit run wider than a line has an unmarked first line of
  digits (nothing precedes it to continue). To judge on the panel.
- **Text floor: Montserrat 12.** Below it the 1-bit threshold erases glyphs
  (at 8 px `:` `.` `,` `;` `_` have zero ink, `:` and `.` are identical; at
  10 px the apostrophe vanishes) — measured by the simulator's
  `check_glyph_ink.c`. No text is ever drawn smaller; room is made by shrinking
  the op title, the divider and `PRESS`, and by thinning the rock edges on
  full-screen states.
- The code has a treatment no name ever gets (large digits, the water
  countdown frame). No code without a press; it vanishes at the end of its
  window or on navigation (unchanged engagements).

## Battery gauge (settled 2026-09-30)

Rounds 3-4 rendered six shapes (vessel, horizontal/vertical battery, drop,
segments, full-width water table, enlarged battery). Mae was convinced by none
("pas ouf") and took **G3, the water drop** (13 × 16 px, teardrop rasterised on
an lv_canvas, filled from the bottom with a staggered wavy edge, outline 2 px
when low), simulator `gauge_lab.c`. Percent, not volts. To be judged again on
the real panel — a better idea may come from seeing it lit, not from more
mockups. Open: `NAVIGATION` still splits `NAVIG` / `ATION` at the 12 px floor.

## Status icons (settled 2026-09-30)

Mae on the panel: the drop was small, "100%" took room, words ("USB",
"RADIO", "SEEN") worse than icons, wants more integration. Chosen: variant
**V2** of the icon round — ONE band under the logo: a large drop (≈20 × 26)
and the route icon beside it (USB plug, or radio waves: 3 waves + filled dot =
dongle seen, 2 waves + hollow dot = not seen), TRRS ⇆ beside when linked.
**The percentage is shown only when low, inside the drop** (Mae: "n'afficher
le chiffre qu'à l'intérieur quand il est bas"); otherwise the drop's level
alone. Route icon sized to balance the drop. The TOTP browser must keep the
account name on two lines (the V2 mockup truncated `OVH:PERSO` to `OVH:~`).

Implemented 2026-09-30 (`memlcd_cave.h`): drop 20 × 26, route and caps icons
20 × 20 (drop x 2, route x 25, ⇆ x 48). "Low" = displayed % ≤ 15, not 20:
"20" (15 px) does not fit the dry part at 20 % (12 px with 1 px air). Unknown
= "?" in an empty drop; charging = a "+" in the drop (ink on the dry part or
cut out of the water), no number; FULL = drop full to the tip, no mark.
Addenda from Mae the same day: Caps Lock / Caps Word as icons (⇪ = hollow
arrow over a bar, Caps Word = the arrow alone) under the layer name, the
one-shots stay text; the layer name at one modest size, Montserrat 14 (two
balanced lines when it does not fit; letter spacing −1 cannot rescue
NAVIGATION: 78 − 9 = 69 px at 12 px, still over 66).

## Right half (2026-09-30)

Mae, once the left was validated on the panel: the right in the same style.
The right is a scanner — no keymap, no layer name, no chest — so its screen
is the cave with only what it knows:

- the rock edges (full, never thin: nothing needs the room), the SAME status
  band as the left at the top of the content box (`memlcd_cave_bande`: the
  drop with every rule above — percent inside only at <= 15, `+` / FULL
  only on USB, `?` unknown, thick outline when LOW — the route icon, the
  TRRS ⇆), and the LARGE 56 px logo centred in the room under it: the logo
  IS the right's identity (the 60 px asset of the old right screen is
  retired, the cave's 56 is shared with both sleep screens);
- the route icon from the right's own inputs (`memlcd_droite_route`): the
  USB plug while its USB is up (`usb_presence_cable` — the host or charger
  it is plugged into; its keys still go by radio, the plug says the cable,
  as on the left); otherwise the waves, FILLED (3 arcs) while the dongle
  acknowledges its frames — `half_link_tx_dongle_vu`, what the old right
  screen drew as "▲ dongle seen": sticky, drops after 3 unacknowledged
  sends, false once fallen back to the left half — HOLLOW (2 arcs) when not;
- asleep: the left's image, large logo + zZ (`memlcd_cave_veille`): the
  same backend hook (`memlcd_before_sleep`) drives both halves, so it fits
  as is.

Implementation: a pure view `memlcd_cave_vue_droite` (`memlcd_cave.h`,
host-tested `test_vue_droite`: band + logo fit 68 × 160 with >= 2 px gaps
and the rock edges for every battery / charge / link / route combination,
the route choice, chest fields ignored); the left's engine `memlcd_cave.c`
compiled for the right with `MEMLCD_CAVE_DROITE=1` (a per-file definition in
`main/CMakeLists.txt`) builds only the rock, the large logo, the drop, the
route, the ⇆ and two labels, with Montserrat 12 and 14 only — no chest,
prompt, code or browser path is compiled into the right. The left's objects
(`memlcd_cave.c.obj`, `memlcd_backend.c.obj`) disassemble identically before
and after. Rendered by `tools/memlcd_sim` (`main_right.c`, `out/cave_right/`).

## Next

A plan task
ports the chosen module into `main/display/memlcd/`, moves the simulator into
`tools/`, and keeps the host tests (safe wrap, label fit, width oracle).
