#pragma once
#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

/* The battery gauge chosen on 2026-09-30: G3, the water drop (spec
 * docs/superpowers/specs/2026-09-29-left-screen-redesign.md, "Battery
 * gauge"). Rounds 3-4 compared six shapes; only the chosen one is kept
 * here. gauge_lab_standalone() renders the drop alone at an explicit
 * percentage (the four-level strip); gauge_lab_mockup() lays out a full
 * top block (logo, route, SEEN, drop + reading, layer name, flags) around
 * it, with >= 2 px between every element. */

/* Fresh blank screen, drop centred, "N%" caption under it in Montserrat 14. */
void gauge_lab_standalone(lv_obj_t *scr, lv_color_t ink, lv_color_t paper, uint8_t pct, bool low);

/* Full top-block mockup: logo, ROUTE ("USB"/"RADIO"), SEEN (optional),
 * drop + voltage on one row, then a one-line hero (hero2 == NULL) or a
 * two-line one (e.g. "NAVIG"/"ATION"), then an optional flags line. dv is
 * decivolts (39 = 3.9 V), chg: 0 plain "V", 1 "+", 2 "#". low forces the
 * 2 px low-battery outline. */
void gauge_lab_mockup(lv_obj_t *scr, lv_color_t ink, lv_color_t paper,
                      bool radio, bool seen,
                      const char *hero1, const char *hero2, const char *flags,
                      uint8_t pct, uint16_t dv, uint8_t chg, bool low);
