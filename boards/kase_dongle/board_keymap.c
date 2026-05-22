/*
 * Default keymap for kase_dongle (5 rows × 14 cols = 70 keys).
 * Cols 0..6  = half_L (left), cols 7..13 = half_R (right).
 *
 * Layout — base layer (QWERTY split-ergo) :
 *
 *   ESC | 1  | 2  | 3  | 4  | 5  | -  ||  =  | 6  | 7  | 8  | 9  | 0  | BSP
 *   TAB | Q  | W  | E  | R  | T  | [  ||  ]  | Y  | U  | I  | O  | P  | \
 *   CTL | A  | S  | D  | F  | G  | -  ||  -  | H  | J  | K  | L  | ;  | '
 *   SHFT| Z  | X  | C  | V  | B  | LO ||  RA | N  | M  | ,  | .  | /  | ENT
 *   --  | -- | -- | GUI| ALT|SPC | LO ||  RA | SPC|RGT |LFT |UP  |DWN | --
 *
 * Symboles shifted (LPRN, TILD, EXLM, etc.) absents du jeu de base : laissés
 * en K_NONE pour la v1, à configurer via le controller app post-flash.
 */

#include "key_definitions.h"
#include "keymap.h"
#include "keyboard_config.h"

uint16_t keymaps[LAYERS][MATRIX_ROWS][MATRIX_COLS] = {
    /* Layer 0 — base.
     * half-left = cols 0..6 (mapped from bench matrix-discovery 2026-05-21).
     * half-right = cols 7..13 (TODO: map when right half is bench-tested).
     * Matrix-row meaning on the half PCB: row4=top letter row, row3, row2,
     * row1 = letter rows; row0 = thumb/function row; col6 = inner column. */
    {
      /*  half-left c0..c6                                    | half-right c7..c13 (local c0..c6) */
        { K_F5,    K_F10,  K_LGUI, K_LSHIFT,K_SPC,   MO_L1,  K_NONE,
          K_DEL,   K_BSPC, K_RGUI, K_RSHIFT,K_SPC,   MO_L1,  K_NONE },
        { K_LCTRL, K_Z,    K_X,    K_C,     K_V,     K_B,    K_NONE,
          K_RCTRL, K_SLSH, K_DOT,  K_COMM,  K_M,     K_N,    K_NONE },
        { K_RALT,  K_A,    K_S,    K_D,     K_F,     K_G,    K_BSPC,
          K_QUOT,  K_SCLN, K_L,    K_K,     K_J,     K_H,    K_ENT },
        { K_TAB,   K_Q,    K_W,    K_E,     K_R,     K_T,    MO_L2,
          K_BSLSH, K_P,    K_O,    K_I,     K_U,     K_Y,    MO_L2 },
        { K_ESC,   K_1,    K_2,    K_3,     K_4,     K_5,    K_LBRC,
          K_MINUS, K_0,    K_9,    K_8,     K_7,     K_6,    K_RBRC },
    },
    /* Layer 1 — lower (F-keys + nav) */
    {
        { K_GRV,    K_F1,   K_F2,   K_F3,   K_F4,   K_F5,   K_NONE,
          K_NONE,   K_F6,   K_F7,   K_F8,   K_F9,   K_F10,  K_DEL },
        { K_TAB,    K_1,    K_2,    K_3,    K_4,    K_5,    K_NONE,
          K_NONE,   K_6,    K_7,    K_8,    K_9,    K_0,    K_BSLSH },
        { K_LCTRL,  K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE,   K_LEFT, K_DOWN, K_UP,   K_RIGHT,K_NONE, K_NONE },
        { K_LSHIFT, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE,   K_NONE, K_NONE, K_HOME, K_END,  K_NONE, K_NONE },
        { K_NONE,   K_NONE, K_NONE, K_LGUI, K_LALT, K_SPC,  K_NONE,
          K_NONE,   K_SPC,  K_NONE, K_NONE, K_NONE, K_NONE, K_NONE },
    },
    /* Layer 2 — raise (placeholder, F11/F12 + à configurer via app) */
    {
        { K_NONE,   K_F11,  K_F12,  K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE,   K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_DEL },
        { K_TAB,    K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE,   K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_BSLSH },
        { K_LCTRL,  K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE,   K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE },
        { K_LSHIFT, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE,   K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE },
        { K_NONE,   K_NONE, K_NONE, K_LGUI, K_LALT, K_SPC,  K_NONE,
          K_NONE,   K_SPC,  K_NONE, K_NONE, K_NONE, K_NONE, K_NONE },
    },
    /* Layers 3..LAYERS-1: empty (filled by user via controller app) */
    [3] = {{0}}, [4] = {{0}}, [5] = {{0}}, [6] = {{0}},
    [7] = {{0}}, [8] = {{0}}, [9] = {{0}},
};
