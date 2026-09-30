# memlcd_sim — host renderer of the left half's screen

The Niphargus halves carry a Sharp LS011B7DH03 memory LCD: 68 × 160 in
portrait, 1 bit, reflective. This tool renders the left half's screen on the
PC, with the REAL LVGL 8 the firmware compiles (`managed_components/lvgl__lvgl`),
the FIRMWARE's own engine (`main/display/memlcd/memlcd_cave.c`, compiled as is —
no copy of it lives here) and the SAME 1-bit threshold as `memlcd_backend.c`'s
`flush_cb`
(`lv_color_brightness(px) < 128` = ink), so what it shows is what the panel
shows, pixel for pixel — and it proves the screen's text rules before anything
is flashed.

## Run

Prerequisite: the managed components must have been fetched once (any build,
or `idf.py -B build_niphar_left -DBOARD=niphar_left
-DSDKCONFIG=build_niphar_left/sdkconfig reconfigure` inside the ESP-IDF
devshell). The simulator itself only needs `gcc` — not the devshell.

```bash
tools/memlcd_sim/build.sh          # ~20 s the first time (LVGL), then seconds
tools/memlcd_sim/build.sh --relib  # force a rebuild of liblvgl.a
```

Everything it builds goes to `tools/memlcd_sim/build/`, every image to
`tools/memlcd_sim/out/` (both gitignored).

## What it proves (gates — a failure exits non-zero)

`tools/memlcd_sim/build.sh --gates` runs these three and nothing else; the
tripwire brick `scripts/tripwire.d/memlcd-sim-gates.sh` calls it from every
`scripts/check.sh` (Stop hook, pre-push), and skips with a visible notice when
`managed_components/lvgl__lvgl` has not been fetched.

1. **Width oracle** (`check_widths.c`): the host tests lay the screen out with
   `memlcd_text_width()` (`memlcd_model.h` + the generated
   `memlcd_font_widths.h`, kerning included); LVGL draws with the fonts. Every
   printable pair, the reference labels and a 2000-string fuzz per font must
   measure the same both ways — the tests prove what the panel shows only
   while they agree.
2. **Glyph ink** (`check_glyph_ink.c`): every printable ASCII glyph of every
   font in the engine's own table (`memlcd_cave_fonts`, `memlcd_cave.c`) keeps
   at least one ink pixel through the threshold, and `:` stays distinguishable
   from `.`. Montserrat 8 and 10 are printed as references and fail (8 loses
   `' , . : ; _ \``, 10 the apostrophe): that is why the text floor is
   Montserrat 12. A font added to the engine is gated here.
3. **Safe wrap, measured by LVGL** (`test_safe_wrap.c`): the firmware's
   `memlcd_safe_wrap` never leaves a line with digits and no letter —
   `TEST:RFC6238` → `TEST:` / `RFC6238`, `BANQUE:4021` → `BANQU` / `E:4021`
   (bench incident 2026-09-29) — and every line it says fits, LVGL measures
   within the budget. The marked wrap of a prompt's label
   (`memlcd_safe_wrap_marque`) is re-measured the same way, each line after
   the first within the budget minus the continuation mark's 8 px.

## What it renders

With `memlcd_cave_build` / `memlcd_cave_draw`, exactly as `memlcd_backend.c`
calls them:

- `out/cave_dark/`: the 10 reference states of the design brief (`states.c`),
  one PNG each (×4) and a contact sheet; the water drop at 100/60/30/10 % and
  LOW; the countdown draining; the security proof sheet (the three
  adversarial labels — `TEST:RFC6238`, `BANQUE:4021`, `AWS:123456789012` —
  on a prompt and in the browser); bench extras (an 8-digit code, a 34-`W`
  label on the UNSCII last resort, a 34-character digit-heavy label behind
  its continuation marks, everything shown at once).
- `out/gauges/gauge_G3.png`: the round-4 gauge sheet, from the firmware.

## Files

| file | role |
|---|---|
| `lv_conf.h` | LVGL configuration of the simulator (16-bit colour, every Montserrat size) |
| `common.c/.h` | the thresholded 68 × 160 panel display, PNG and contact-sheet output |
| `states.c/.h` | the 10 reference states + 4 security states, as `memlcd_model_t` values |
| `main_cave.c` | the renderer (firmware engine) |
| `check_widths.c`, `check_glyph_ink.c`, `test_safe_wrap.c` | the three gates |
| `stb_image_write.h` | PNG writer (public domain, Sean Barrett) |

The engine, its pure view and its assets live in the firmware:
`main/display/memlcd/memlcd_cave.{c,h}`, `memlcd_safe_wrap.h`,
`memlcd_assets_cave.c` (`scripts/gen_memlcd_cave_assets.py`), the logos
`main/display/assets/img_niphargus_{28,56}.c` (`scripts/gen_logo_memlcd.sh`).
The design rounds (light palette, the other gauges and directions) stayed in
the session that produced them; the chosen design is what the firmware draws.

Design record: `docs/superpowers/specs/2026-09-29-left-screen-redesign.md`.
