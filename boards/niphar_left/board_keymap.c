/* Default keymap — Niphargus, BOTH halves.
 *
 * The left half is the keyboard's only keymap engine: it carries the keycodes
 * for all 52 keys even though it only scans 26. Hence KEYMAP_COLS = 14 when
 * MATRIX_COLS is 7 — columns 0-6 for this half, 7-13 for the right, whose
 * radio-received coordinate is offset by MATRIX_COLS.
 *
 * 26 keys per half, in rows of 7/7/6/6: the missing positions of the last
 * two rows are K_NO, on each side.
 *
 * ⚠ ON THE RIGHT SIDE, THESE GAPS ARE IN COLUMN 7, NOT COLUMN 13. The two
 * halves are the same PCB flipped over: the right's short column is its
 * PHYSICAL column 6, which carries keymap column 13 - 6 = 7
 * (half_col_to_keymap). Until 2026-09-07 the K_NO were in column 13 and
 * rows 2 and 3 were shifted by one notch: K_N was unreachable, and the
 * outermost key on the right produced nothing. Rows 0 and 1, full across
 * all seven columns, gave nothing away — hence a fault that only showed
 * up on the bottom row.
 *
 * Two thumb positions on the right (columns 12 and 13) are left with no
 * factory assignment: they physically exist, to be provisioned over USB. The
 * real keymap is provisioned over USB (KS_CMD_*); this one is only the factory fallback. */
#include "keymap.h"
#include "key_definitions.h"
#include "keyboard_config.h"

char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH] = {
    "MAIN", "NAV", "LAYER 2", "LAYER 3", "LAYER 4",
    "LAYER 5", "LAYER 6", "LAYER 7", "LAYER 8", "LAYER 9",
};

#define _______ K_TRNS
#define XXXXXXX K_NO

uint16_t keymaps[LAYERS][MATRIX_ROWS][KEYMAP_COLS] = {
    {   /* 0 — MAIN            ── left (0-6) ──          ── right (7-13) ── */
        {K_TAB,   K_Q, K_W, K_E, K_R, K_T, K_LBRC,   K_Y, K_U, K_I, K_O, K_P,    K_BSPC,   K_DEL},
        {K_ESC,   K_A, K_S, K_D, K_F, K_G, K_RBRC,   K_H, K_J, K_K, K_L, K_SCLN, K_QUOT,   K_ENT},
        {K_LSHIFT,K_Z, K_X, K_C, K_V, K_B, XXXXXXX,  XXXXXXX, K_N, K_M, K_COMM, K_DOT, K_SLSH, K_RSHIFT},
        {K_LCTRL, K_LWIN, K_LALT, MO_L1, K_SPACE, K_ENT, XXXXXXX,
                                                    XXXXXXX, K_SPACE, MO_L1, K_RALT, K_RCTRL, XXXXXXX, XXXXXXX},
    },
    {   /* 1 — NAV */
        {_______, K_1, K_2, K_3, K_4, K_5, XXXXXXX,  K_6, K_7, K_8, K_9, K_0, _______, _______},
        {_______, K_F1, K_HOME, K_UP, K_END, XXXXXXX, XXXXXXX,
                                                    XXXXXXX, K_LEFT, K_DOWN, K_UP, K_RIGHT, XXXXXXX, XXXXXXX},
        {_______, XXXXXXX, K_LEFT, K_DOWN, K_RIGHT, XXXXXXX, XXXXXXX,
                                                    XXXXXXX, K_HOME, K_END, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX},
        {_______, _______, _______, _______, _______, XXXXXXX, XXXXXXX,
                                                    _______, _______, _______, _______, XXXXXXX, XXXXXXX, XXXXXXX},
    },
    {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}},
    {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}},
};
