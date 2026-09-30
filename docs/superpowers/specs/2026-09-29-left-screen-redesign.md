# Left-half screen redesign — "cave" (design record)

Status: design in progress (2026-09-29). Supersedes the layout of chest v3 Task 8
(commits d9de081b, 0616260b — their mode names and width oracle stay).

## Why

Mae, after flashing 0616260b: "pas très beau, il faut revoir complètement
l'interface". All four complaints: too cluttered, mixed fonts, layout (icon
column, separators, imbalance), ugly icons. Scope: the whole LEFT screen
(normal screen + every chest state). The right half is out of scope.

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

## Next

A plan task
ports the chosen module into `main/display/memlcd/`, moves the simulator into
`tools/`, and keeps the host tests (safe wrap, label fit, width oracle).
