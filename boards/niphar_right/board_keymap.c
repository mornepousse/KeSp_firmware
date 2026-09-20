/* Default keymap — Niphargus right half.
 * 26 keys: rows of 7/7/6/6. The missing positions in the last two
 * rows are K_NO. Columns are a mirror of the left (right half of the
 * physical keyboard). The right side has no keymap engine (slave): keymaps[][]
 * exists here only because input/keymap.c is always compiled — nothing
 * uses it as long as the slave role only reports its matrix. */
#include "keymap.h"
#include "key_definitions.h"
#include "keyboard_config.h"

char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH] = {
    "MAIN", "NAV", "LAYER 2", "LAYER 3", "LAYER 4",
    "LAYER 5", "LAYER 6", "LAYER 7", "LAYER 8", "LAYER 9",
};

#define _______ K_TRNS
#define XXXXXXX K_NO

uint16_t keymaps[LAYERS][MATRIX_ROWS][MATRIX_COLS] = {
    {   /* 0 — MAIN */
        {K_Y, K_U, K_I,    K_O,    K_P,     K_LBRC,   K_RBRC},
        {K_H, K_J, K_K,    K_L,    K_SCLN,  K_QUOT,   K_BSLSH},
        {K_N, K_M, K_COMM, K_DOT,  K_SLSH,  K_RSHIFT, XXXXXXX},
        {MO_L1, K_SPACE, K_ENT, K_RALT, K_RWIN, K_RCTRL, XXXXXXX},
    },
    {   /* 1 — NAV */
        {K_6, K_7, K_8, K_9, K_0, XXXXXXX, XXXXXXX},
        {XXXXXXX, XXXXXXX, K_F2, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX},
        {XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX},
        {_______, _______, _______, _______, _______, XXXXXXX, XXXXXXX},
    },
    {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}},
    {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}},
};
