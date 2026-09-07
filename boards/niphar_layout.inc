/*
 * Niphargus moitié gauche — physical key layout.
 * 4 rows × 7 cols electrical grid; 2 electrical positions unused (row2/col6,
 * row3/col6) — see boards/niphar_left/board_keymap.c.
 *
 * Format v2: direct x/y positioning with optional rotation (r) and width (w).
 * Top-level "keys" = individually positioned keys (thumb cluster + the two
 * bracket keys of the pinky-extension column, which don't span the full
 * column height). "groups" = vertical finger columns (rows 0..2) sharing a
 * common origin and rotation.
 *
 * Requires: PRODUCT_NAME, MATRIX_ROWS, MATRIX_COLS, STR() defined.
 */
/* Sur une moitié esclave, KEYMAP_COLS n'est pas défini (board.h ne le pose que
 * sur le maître) et vaut alors MATRIX_COLS : le layout décrit sa seule moitié.
 * Sur le maître il vaut 14, et le bloc miroir ci-dessous ajoute la droite. */
#ifndef KEYMAP_COLS
#define KEYMAP_COLS MATRIX_COLS
#endif

/* ── Moitié droite : miroir géométrique de la gauche ────────────────────────
 *
 * Ajoutée le 2026-09-07. Le layout annonçait "cols": MATRIX_COLS = 7 et ne
 * décrivait que les colonnes 0-6 : le logiciel de remappage ne voyait qu'une
 * demi-clavier et les colonnes 7-13, pourtant stockées en NVS, étaient
 * INADRESSABLES. La moitié droite n'était tout simplement pas remappable.
 *
 * Les deux moitiés sont le même PCB retourné. Une touche de la droite se place
 * donc au miroir de son homologue gauche, à un écart d'une unité entre les
 * deux moitiés : demi-largeur W = 7, écart G = 1, d'où
 *     x' = 2W + G - x - largeur = 15 - x - w
 * et les rotations changent de signe. La colonne physique c de la droite porte
 * la colonne keymap 13 - c (cf. half_col_to_keymap), donc la géométrie de la
 * colonne gauche c décrit la colonne keymap 13 - c. */

const char board_layout_json[] =
"{"
  "\"name\":\"" PRODUCT_NAME "\","
  "\"rows\":" STR(MATRIX_ROWS) ","
  "\"cols\":" STR(KEYMAP_COLS) ","
  "\"keys\":["
    "{\"row\":3,\"col\":0,\"x\":0.3,\"y\":3.6,\"r\":20.0},"
    "{\"row\":3,\"col\":1,\"x\":1.3,\"y\":3.8,\"r\":10.0},"
    "{\"row\":3,\"col\":2,\"x\":2.3,\"y\":3.9,\"w\":1.2},"
    "{\"row\":3,\"col\":3,\"x\":3.6,\"y\":3.9,\"r\":-10.0},"
    "{\"row\":3,\"col\":4,\"x\":4.6,\"y\":3.7,\"r\":-20.0},"
    "{\"row\":3,\"col\":5,\"x\":5.6,\"y\":3.8,\"r\":-10.0},"
    "{\"row\":0,\"col\":6,\"x\":6.0,\"y\":0.0},"
    "{\"row\":1,\"col\":6,\"x\":6.0,\"y\":1.0}"
#if KEYMAP_COLS > MATRIX_COLS
    ",{\"row\":3,\"col\":13,\"x\":13.7,\"y\":3.6,\"r\":-20.0},"
    "{\"row\":3,\"col\":12,\"x\":12.7,\"y\":3.8,\"r\":-10.0},"
    "{\"row\":3,\"col\":11,\"x\":11.5,\"y\":3.9,\"w\":1.2},"
    "{\"row\":3,\"col\":10,\"x\":10.4,\"y\":3.9,\"r\":10.0},"
    "{\"row\":3,\"col\":9,\"x\":9.4,\"y\":3.7,\"r\":20.0},"
    "{\"row\":3,\"col\":8,\"x\":8.4,\"y\":3.8,\"r\":10.0},"
    "{\"row\":0,\"col\":7,\"x\":8.0,\"y\":0.0},"
    "{\"row\":1,\"col\":7,\"x\":8.0,\"y\":1.0}"
#endif
  "],"
  "\"groups\":["
    "{\"x\":0.0,\"y\":0.3,\"keys\":["
      "{\"row\":0,\"col\":0,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":0,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":0,\"x\":0,\"y\":2}"
    "]},"
    "{\"x\":1.0,\"y\":0.15,\"keys\":["
      "{\"row\":0,\"col\":1,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":1,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":1,\"x\":0,\"y\":2}"
    "]},"
    "{\"x\":2.0,\"y\":0.0,\"keys\":["
      "{\"row\":0,\"col\":2,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":2,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":2,\"x\":0,\"y\":2}"
    "]},"
    "{\"x\":3.0,\"y\":0.05,\"keys\":["
      "{\"row\":0,\"col\":3,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":3,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":3,\"x\":0,\"y\":2}"
    "]},"
    "{\"x\":4.0,\"y\":0.2,\"keys\":["
      "{\"row\":0,\"col\":4,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":4,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":4,\"x\":0,\"y\":2}"
    "]},"
    "{\"x\":5.0,\"y\":0.4,\"keys\":["
      "{\"row\":0,\"col\":5,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":5,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":5,\"x\":0,\"y\":2}"
    "]}"
#if KEYMAP_COLS > MATRIX_COLS
    ","
    "{\"x\":14.0,\"y\":0.3,\"keys\":["
      "{\"row\":0,\"col\":13,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":13,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":13,\"x\":0,\"y\":2}"
    "]},"
    "{\"x\":13.0,\"y\":0.15,\"keys\":["
      "{\"row\":0,\"col\":12,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":12,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":12,\"x\":0,\"y\":2}"
    "]},"
    "{\"x\":12.0,\"y\":0.0,\"keys\":["
      "{\"row\":0,\"col\":11,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":11,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":11,\"x\":0,\"y\":2}"
    "]},"
    "{\"x\":11.0,\"y\":0.05,\"keys\":["
      "{\"row\":0,\"col\":10,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":10,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":10,\"x\":0,\"y\":2}"
    "]},"
    "{\"x\":10.0,\"y\":0.2,\"keys\":["
      "{\"row\":0,\"col\":9,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":9,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":9,\"x\":0,\"y\":2}"
    "]},"
    "{\"x\":9.0,\"y\":0.4,\"keys\":["
      "{\"row\":0,\"col\":8,\"x\":0,\"y\":0},"
      "{\"row\":1,\"col\":8,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":8,\"x\":0,\"y\":2}"
    "]}"
#endif
  "]"
"}"
;
