/*
 * KaSe split-wireless physical key layout — dongle (full keyboard view).
 * 66 keys: half-left = cols 0-6, half-right = cols 7-13 (14 cols total).
 *
 * Atreus62-derived column stagger. NOTE the dongle's per-column row order is
 * row4 (top, number row) → row3 → row2 → row1 → row0 (bottom/thumb row).
 * This differs from kase_layout.inc (V2), so this layout is built fresh.
 *
 * Inner columns (col6 left, col13 right) have only 3 keys (rows 4,3,2) and are
 * placed flanking the centre gap. Matrix positions (0,6)/(1,6)/(0,13)/(1,13)
 * are unpopulated (K_NONE) and therefore absent here.
 *
 * Geometry is a sensible default — refine visually in the controller software.
 *
 * Format v2: groups = vertical columns (origin x/y, optional rotation r);
 * top-level keys = individually placed (the two inner columns).
 * Requires: PRODUCT_NAME, MATRIX_ROWS, MATRIX_COLS, STR() defined.
 */
const char board_layout_json[] =
"{"
  "\"name\":\"" PRODUCT_NAME "\","
  "\"rows\":" STR(MATRIX_ROWS) ","
  "\"cols\":" STR(MATRIX_COLS) ","
  "\"keys\":["
    /* Left inner column (col6): row4=LBRC, row3=MO2, row2=Bksp */
    "{\"row\":4,\"col\":6,\"x\":6.1,\"y\":2.0},"
    "{\"row\":3,\"col\":6,\"x\":6.1,\"y\":3.0},"
    "{\"row\":2,\"col\":6,\"x\":6.1,\"y\":4.0},"
    /* Right inner column (col13): row4=RBRC, row3=MO2, row2=Enter */
    "{\"row\":4,\"col\":13,\"x\":8.4,\"y\":2.0},"
    "{\"row\":3,\"col\":13,\"x\":8.4,\"y\":3.0},"
    "{\"row\":2,\"col\":13,\"x\":8.4,\"y\":4.0}"
  "],"
  "\"groups\":["
    /* ── Half-left columns (col0 pinky-outer → col5 index-inner) ── */
    "{\"x\":0.0,\"y\":0.40,\"keys\":["
      "{\"row\":4,\"col\":0,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":0,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":0,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":0,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":0,\"x\":0,\"y\":4}"
    "]},"
    "{\"x\":1.0,\"y\":0.25,\"keys\":["
      "{\"row\":4,\"col\":1,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":1,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":1,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":1,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":1,\"x\":0,\"y\":4}"
    "]},"
    "{\"x\":2.0,\"y\":0.10,\"keys\":["
      "{\"row\":4,\"col\":2,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":2,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":2,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":2,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":2,\"x\":0,\"y\":4}"
    "]},"
    "{\"x\":3.0,\"y\":0.0,\"keys\":["
      "{\"row\":4,\"col\":3,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":3,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":3,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":3,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":3,\"x\":0,\"y\":4}"
    "]},"
    "{\"x\":4.0,\"y\":0.15,\"keys\":["
      "{\"row\":4,\"col\":4,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":4,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":4,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":4,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":4,\"x\":0,\"y\":4}"
    "]},"
    "{\"x\":5.0,\"y\":0.30,\"keys\":["
      "{\"row\":4,\"col\":5,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":5,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":5,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":5,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":5,\"x\":0,\"y\":4}"
    "]},"
    /* ── Half-right columns (col12 index-inner → col7 pinky-outer) ── */
    "{\"x\":9.5,\"y\":0.30,\"keys\":["
      "{\"row\":4,\"col\":12,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":12,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":12,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":12,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":12,\"x\":0,\"y\":4}"
    "]},"
    "{\"x\":10.5,\"y\":0.15,\"keys\":["
      "{\"row\":4,\"col\":11,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":11,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":11,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":11,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":11,\"x\":0,\"y\":4}"
    "]},"
    "{\"x\":11.5,\"y\":0.0,\"keys\":["
      "{\"row\":4,\"col\":10,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":10,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":10,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":10,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":10,\"x\":0,\"y\":4}"
    "]},"
    "{\"x\":12.5,\"y\":0.10,\"keys\":["
      "{\"row\":4,\"col\":9,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":9,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":9,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":9,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":9,\"x\":0,\"y\":4}"
    "]},"
    "{\"x\":13.5,\"y\":0.25,\"keys\":["
      "{\"row\":4,\"col\":8,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":8,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":8,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":8,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":8,\"x\":0,\"y\":4}"
    "]},"
    "{\"x\":14.5,\"y\":0.40,\"keys\":["
      "{\"row\":4,\"col\":7,\"x\":0,\"y\":0},"
      "{\"row\":3,\"col\":7,\"x\":0,\"y\":1},"
      "{\"row\":2,\"col\":7,\"x\":0,\"y\":2},"
      "{\"row\":1,\"col\":7,\"x\":0,\"y\":3},"
      "{\"row\":0,\"col\":7,\"x\":0,\"y\":4}"
    "]}"
  "]"
"}"
;
