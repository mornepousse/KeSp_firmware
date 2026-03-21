/*
 * KaSe split-ergo physical key layout — shared across board variants.
 * 64 keys, column stagger with per-column rotation and thumb clusters.
 * Column 6 row 1 has no physical key.
 *
 * Hierarchy: Group > Line > Keycap
 *   - Line default orientation is vertical.
 *   - Margin = "left,top,right,bottom" in pixels.
 *   - Angle = rotation in degrees (positive = clockwise).
 *   - Width = key width in pixels (omitted = default 1u).
 *
 * Requires: PRODUCT_NAME, MATRIX_ROWS, MATRIX_COLS, STR() defined.
 */
const char board_layout_json[] =
"{"
  "\"name\":\"" PRODUCT_NAME "\","
  "\"rows\":" STR(MATRIX_ROWS) ","
  "\"cols\":" STR(MATRIX_COLS) ","
  "\"Group\":{\"Children\":[{\"Line\":{\"Orientation\":\"Horizontal\",\"Children\":["

    /* ═══════════════════════════════════════════════════════════ */
    /*                        LEFT HALF                           */
    /* ═══════════════════════════════════════════════════════════ */
    "{\"Group\":{\"Children\":["

      /* ── Main key columns (horizontal row of vertical columns) ── */
      "{\"Line\":{\"Orientation\":\"Horizontal\",\"Children\":["

        /* Col 0 — no rotation */
        "{\"Line\":{\"Children\":["
          "{\"Keycap\":{\"Column\":0,\"Row\":4}},"
          "{\"Keycap\":{\"Column\":0,\"Row\":0}},"
          "{\"Keycap\":{\"Column\":0,\"Row\":1}},"
          "{\"Keycap\":{\"Column\":0,\"Row\":2}},"
          "{\"Keycap\":{\"Column\":0,\"Row\":3}}"
        "]}},"

        /* Col 1 — no rotation */
        "{\"Line\":{\"Children\":["
          "{\"Keycap\":{\"Column\":1,\"Row\":4}},"
          "{\"Keycap\":{\"Column\":1,\"Row\":0}},"
          "{\"Keycap\":{\"Column\":1,\"Row\":1}},"
          "{\"Keycap\":{\"Column\":1,\"Row\":2}},"
          "{\"Keycap\":{\"Column\":1,\"Row\":3}}"
        "]}},"

        /* Col 2 — 5° */
        "{\"Line\":{\"Margin\":\"8,25,0,0\","
          "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":5}},"
          "\"Children\":["
            "{\"Keycap\":{\"Column\":2,\"Row\":0}},"
            "{\"Keycap\":{\"Column\":2,\"Row\":1}},"
            "{\"Keycap\":{\"Column\":2,\"Row\":2}},"
            "{\"Keycap\":{\"Column\":2,\"Row\":3}}"
        "]}},"

        /* Col 3 — 10° */
        "{\"Line\":{\"Margin\":\"10,5,0,0\","
          "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":10}},"
          "\"Children\":["
            "{\"Keycap\":{\"Column\":3,\"Row\":0}},"
            "{\"Keycap\":{\"Column\":3,\"Row\":1}},"
            "{\"Keycap\":{\"Column\":3,\"Row\":2}},"
            "{\"Keycap\":{\"Column\":3,\"Row\":3}}"
        "]}},"

        /* Col 4 — 10° */
        "{\"Line\":{\"Margin\":\"0,25,0,0\","
          "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":10}},"
          "\"Children\":["
            "{\"Keycap\":{\"Column\":4,\"Row\":0}},"
            "{\"Keycap\":{\"Column\":4,\"Row\":1}},"
            "{\"Keycap\":{\"Column\":4,\"Row\":2}},"
            "{\"Keycap\":{\"Column\":4,\"Row\":3}}"
        "]}},"

        /* Col 5 — 10° */
        "{\"Line\":{\"Margin\":\"0,38,0,0\","
          "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":10}},"
          "\"Children\":["
            "{\"Keycap\":{\"Column\":5,\"Row\":0}},"
            "{\"Keycap\":{\"Column\":5,\"Row\":1}},"
            "{\"Keycap\":{\"Column\":5,\"Row\":2}},"
            "{\"Keycap\":{\"Column\":5,\"Row\":3}}"
        "]}},"

        /* Col 6 row 0 — inner top key */
        "{\"Keycap\":{\"Column\":6,\"Row\":0,"
          "\"Margin\":\"-8,170,0,0\","
          "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":10}}}}"

      "]}},"  /* end left main columns Line */

      /* ── Left thumb cluster ── */
      "{\"Keycap\":{\"Column\":2,\"Row\":4,\"Width\":60,\"Margin\":\"110,228,0,0\"}},"
      "{\"Keycap\":{\"Column\":3,\"Row\":4,"
        "\"Margin\":\"174,225,0,0\","
        "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":10}}}},"
      "{\"Keycap\":{\"Column\":4,\"Row\":4,"
        "\"Margin\":\"228,240,0,0\","
        "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":20}}}},"
      "{\"Keycap\":{\"Column\":5,\"Row\":4,"
        "\"Margin\":\"280,270,0,0\","
        "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":40}}}},"
      "{\"Keycap\":{\"Column\":6,\"Row\":4,"
        "\"Margin\":\"320,305,0,0\","
        "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":40}}}}"

    "]}},"  /* end left Group */

    /* ═══════════════════════════════════════════════════════════ */
    /*                       RIGHT HALF                           */
    /* ═══════════════════════════════════════════════════════════ */
    "{\"Group\":{\"Margin\":\"420,0,0,0\",\"HorizontalAlignment\":\"Right\",\"Children\":["

      /* Inner thumb pair — -40° */
      "{\"Line\":{\"Orientation\":\"Horizontal\","
        "\"Margin\":\"0,290,0,0\","
        "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":-40}},"
        "\"Children\":["
          "{\"Keycap\":{\"Column\":6,\"Row\":3}},"
          "{\"Keycap\":{\"Column\":7,\"Row\":4}}"
      "]}},"

      /* Right thumb keys */
      "{\"Keycap\":{\"Column\":8,\"Row\":4,"
        "\"Margin\":\"95,245,0,0\","
        "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":-20}}}},"
      "{\"Keycap\":{\"Column\":9,\"Row\":4,"
        "\"Margin\":\"145,230,0,0\","
        "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":-10}}}},"
      "{\"Keycap\":{\"Column\":10,\"Row\":4,\"Width\":60,\"Margin\":\"200,228,0,0\"}},"

      /* ── Right main key columns ── */
      "{\"Line\":{\"Orientation\":\"Horizontal\",\"Children\":["

        /* Col 6 row 2 — inner column */
        "{\"Keycap\":{\"Column\":6,\"Row\":2,"
          "\"Margin\":\"0,170,-8,0\","
          "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":-10}}}},"

        /* Col 7 — -10° */
        "{\"Line\":{\"Margin\":\"0,38,0,0\","
          "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":-10}},"
          "\"Children\":["
            "{\"Keycap\":{\"Column\":7,\"Row\":0}},"
            "{\"Keycap\":{\"Column\":7,\"Row\":1}},"
            "{\"Keycap\":{\"Column\":7,\"Row\":2}},"
            "{\"Keycap\":{\"Column\":7,\"Row\":3}}"
        "]}},"

        /* Col 8 — -10° */
        "{\"Line\":{\"Margin\":\"0,25,0,0\","
          "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":-10}},"
          "\"Children\":["
            "{\"Keycap\":{\"Column\":8,\"Row\":0}},"
            "{\"Keycap\":{\"Column\":8,\"Row\":1}},"
            "{\"Keycap\":{\"Column\":8,\"Row\":2}},"
            "{\"Keycap\":{\"Column\":8,\"Row\":3}}"
        "]}},"

        /* Col 9 — -10° */
        "{\"Line\":{\"Margin\":\"0,5,10,0\","
          "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":-10}},"
          "\"Children\":["
            "{\"Keycap\":{\"Column\":9,\"Row\":0}},"
            "{\"Keycap\":{\"Column\":9,\"Row\":1}},"
            "{\"Keycap\":{\"Column\":9,\"Row\":2}},"
            "{\"Keycap\":{\"Column\":9,\"Row\":3}}"
        "]}},"

        /* Col 10 — -5° */
        "{\"Line\":{\"Margin\":\"0,25,8,0\","
          "\"RenderTransform\":{\"RotateTransform\":{\"Angle\":-5}},"
          "\"Children\":["
            "{\"Keycap\":{\"Column\":10,\"Row\":0}},"
            "{\"Keycap\":{\"Column\":10,\"Row\":1}},"
            "{\"Keycap\":{\"Column\":10,\"Row\":2}},"
            "{\"Keycap\":{\"Column\":10,\"Row\":3}}"
        "]}},"

        /* Col 11 — no rotation */
        "{\"Line\":{\"Children\":["
          "{\"Keycap\":{\"Column\":11,\"Row\":4}},"
          "{\"Keycap\":{\"Column\":11,\"Row\":0}},"
          "{\"Keycap\":{\"Column\":11,\"Row\":1}},"
          "{\"Keycap\":{\"Column\":11,\"Row\":2}},"
          "{\"Keycap\":{\"Column\":11,\"Row\":3}}"
        "]}},"

        /* Col 12 — no rotation */
        "{\"Line\":{\"Children\":["
          "{\"Keycap\":{\"Column\":12,\"Row\":4}},"
          "{\"Keycap\":{\"Column\":12,\"Row\":0}},"
          "{\"Keycap\":{\"Column\":12,\"Row\":1}},"
          "{\"Keycap\":{\"Column\":12,\"Row\":2}},"
          "{\"Keycap\":{\"Column\":12,\"Row\":3}}"
        "]}}"

      "]}}"   /* end right main columns Line */

    "]}}"     /* end right Group */

  "]}}]}}"    /* end outer Line, outer Group, root */
;
