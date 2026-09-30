#pragma once
#include "lvgl.h"
#include "memlcd_model.h"
#include <stdbool.h>

/* Cave engine v2 (round 2, 2026-09-29): Mae's feedback on round 1 was that
 * an all-black background alone does not read as a cave, and the lv_arc
 * mascot did not read as a shrimp. This version drops the mascot entirely
 * and makes the cave literal: a rocky ceiling/floor (procedural pixel
 * bitmaps, gen_rock.py), underground water (battery + TOTP countdown are
 * both a rising/falling water level with a wavy surface, not a bar), and
 * Bayer-dither shading on the rock — the real Niphargus logo replaces the
 * mascot as the identity mark. ONE layout, driven by a palette only:
 * light (dark ink / pale paper) and dark (inverted) are the two shipped
 * variants, built via the same cave_build/cave_draw pair. */
typedef struct {
    lv_obj_t *scr;
    lv_color_t ink, paper;

    lv_obj_t *rock_top, *rock_bottom;         /* fixed, never hidden */
    lv_obj_t *logo_small, *logo_large;        /* real Niphargus logo, two sizes */

    lv_obj_t *l_route;                        /* "USB" / "RADIO", full word */
    lv_obj_t *l_seen;                         /* "SEEN" — dongle-seen, its own line, never fused into the route word */

    /* water battery gauge: outline + fill + wavy surface + dither shadow */
    lv_obj_t *batt_box, *batt_fill, *batt_wave, *batt_shadow;
    lv_obj_t *l_volt;

    lv_obj_t *l_zz;

    lv_obj_t *l_big[3];                       /* hero: layer name / op / code halves — single font, no wrap (size chosen to fit) */
    lv_obj_t *l_line[6];                      /* generic secondary lines: mode word, status flags, browse pos, ACCTS, PRESS/NO TIME — never a label */
    /* 2026-09-29 fix: label_set used to write into l_line[start_idx..] too,
     * sharing the SAME objects as ACCTS/PRESS/NO TIME. That was safe only
     * as long as a label never needed more than 3-4 lines; raising the
     * text-size floor to 12px means state 7's 34-char label now needs 6
     * (see check_glyph_ink.c / K_LADDER_FLOOR in cave_ui.c) — the 6th line
     * was silently overwritten by the PRESS widget reusing l_line[5]
     * ("2FA01" vanished off the bottom of the prompt, an actual instance of
     * the exact "label shown WHOLE, never cut" rule this file exists to
     * enforce). Labels now get their OWN dedicated pool (8 lines — the
     * worst case rendered so far, the 34-char state 7 label, needs 6;
     * label_set itself still clamps and warns to stderr if a label ever
     * needs more than the pool holds). */
    lv_obj_t *l_label[8];                     /* safe_wrap label lines ONLY — dedicated, never shared with l_line */
    lv_obj_t *rule;                           /* wavy divider under the prompt's op word */

    /* water countdown gauge (TOTP): same idea, horizontal */
    lv_obj_t *code_box, *code_fill, *code_wave, *code_shadow;
} cave_ui_t;

void cave_build(cave_ui_t *u, lv_obj_t *scr, lv_color_t ink, lv_color_t paper);
void cave_draw(cave_ui_t *u, const memlcd_model_t *m, uint8_t batt_pct);

/* Demo-only entry points for the required proof renders (Mae, round 2):
 * a battery water level at an explicit percentage regardless of model
 * voltage, and a code countdown water level at an explicit seconds value —
 * both used only by the animation/proof strips, never by cave_draw itself. */
void cave_draw_battery_demo(cave_ui_t *u, uint8_t pct);
void cave_draw_code_demo(cave_ui_t *u, const char *name, const char *code, uint8_t secs);
