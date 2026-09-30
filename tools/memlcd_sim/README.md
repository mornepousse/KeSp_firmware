# memlcd_sim — host renderer of the left half's screen

The Niphargus halves carry a Sharp LS011B7DH03 memory LCD: 68 × 160 in
portrait, 1 bit, reflective. This tool renders the left half's screen on the
PC, with the REAL LVGL 8 the firmware compiles (`managed_components/lvgl__lvgl`)
and the SAME 1-bit threshold as `memlcd_backend.c`'s `flush_cb`
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

1. **Glyph ink** (`check_glyph_ink.c`): every printable ASCII glyph of every
   font the screen uses keeps at least one ink pixel through the threshold,
   and `:` stays distinguishable from `.`. Montserrat 8 and 10 are printed as
   references and fail (8 loses `' , . : ; _ \``, 10 loses the apostrophe):
   that is why the text floor is Montserrat 12.
2. **Safe wrap** (`test_safe_wrap.c`): a name broken over lines never leaves a
   line with digits and no letter — `TEST:RFC6238` → `TEST:` / `RFC6238`,
   `BANQUE:4021` → `BANQU` / `E:4021` (bench incident 2026-09-29: a pixel cut
   `TEST:RFC` / `6238` read as a code).

## What it renders

- `out/cave_dark/`: the 10 reference states of the design brief
  (`states.c`) one PNG each (×4) and a contact sheet, the battery and
  countdown water levels, and the security proof sheet (the two adversarial
  labels on a prompt and in the browser).
- `out/gauges/gauge_G3.png`: the water-drop battery gauge at 100/60/30/10 %
  and in three top-block mockups.

## Files

| file | role |
|---|---|
| `lv_conf.h` | LVGL configuration of the simulator (16-bit colour, every Montserrat size) |
| `common.c/.h` | the thresholded 68 × 160 panel display, PNG and contact-sheet output |
| `states.c/.h` | the 10 reference states + 4 security states, as `memlcd_model_t` values |
| `cave_ui.c/.h`, `dir_cave_dark.c/.h` | the cave engine (dark palette) |
| `safe_wrap.h`, `test_safe_wrap.c` | safe name breaking and its gate |
| `gauge_lab.c/.h`, `main_gauge_lab.c` | the G3 water drop |
| `check_glyph_ink.c` | the 12 px floor gate |
| `gen_logo.sh`, `gen_rock.py`, `assets/` | the Niphargus logo (28/56 px) and the rock / dither bitmaps |
| `stb_image_write.h` | PNG writer (public domain, Sean Barrett) |

Design record: `docs/superpowers/specs/2026-09-29-left-screen-redesign.md`.
