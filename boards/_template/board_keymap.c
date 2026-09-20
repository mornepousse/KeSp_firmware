/* __BOARD_NAME__ — default keymap (compile-time defaults; the running keymap
 * lives in NVS and is edited with KeSp_controller over the binary CDC).
 * One layer filled, the rest transparent. Keycodes: main/input/key_definitions.h
 * (K_A…, 0x0100+layer*256 = momentary layer, K_TRNS = transparent, K_NO = nothing). */
#include "keymap.h"
#include "key_definitions.h"
#include "keyboard_config.h"

char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH] = {
    "MAIN", "LAYER 1", "LAYER 2", "LAYER 3", "LAYER 4",
    "LAYER 5", "LAYER 6", "LAYER 7", "LAYER 8", "LAYER 9",
};

#define _______ K_TRNS
#define XXXXXXX K_NO
#define MO_1    0x0200   /* momentary layer 1 (MO_L0 + 1*256, key_definitions.h) — a macro: MO_L1 is a const, not usable in an initializer */

uint16_t keymaps[LAYERS][MATRIX_ROWS][KEYMAP_COLS] = {
    { /* MAIN — 4 rows × 7 columns, edit to your switches */
        { K_Q,    K_W,    K_E,    K_R,    K_T,    K_Y,    K_U    },
        { K_A,    K_S,    K_D,    K_F,    K_G,    K_H,    K_J    },
        { K_Z,    K_X,    K_C,    K_V,    K_B,    K_N,    K_M    },
        { K_LCTRL,K_LGUI, K_LALT, MO_1,   K_SPC,  K_BSPC, K_ENT  },
    },
    { /* LAYER 1 */
        { K_1,    K_2,    K_3,    K_4,    K_5,    K_6,    K_7    },
        { _______,_______,_______,_______,_______,_______,_______},
        { _______,_______,_______,_______,_______,_______,_______},
        { _______,_______,_______,_______,_______,_______,_______},
    },
    /* layers 2..9: transparent (zero-initialised = K_NO; use _______ to fall through) */
};
