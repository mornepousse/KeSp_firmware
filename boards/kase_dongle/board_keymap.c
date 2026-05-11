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
    /* Layer 0 — base */
    {
        { K_ESC,    K_1,    K_2,    K_3,    K_4,    K_5,    K_MINUS,
          K_EQL,    K_6,    K_7,    K_8,    K_9,    K_0,    K_BSPC },
        { K_TAB,    K_Q,    K_W,    K_E,    K_R,    K_T,    K_LBRC,
          K_RBRC,   K_Y,    K_U,    K_I,    K_O,    K_P,    K_BSLSH },
        { K_LCTRL,  K_A,    K_S,    K_D,    K_F,    K_G,    K_NONE,
          K_NONE,   K_H,    K_J,    K_K,    K_L,    K_SCLN, K_QUOT },
        { K_LSHIFT, K_Z,    K_X,    K_C,    K_V,    K_B,    MO_L1,
          MO_L2,    K_N,    K_M,    K_COMM, K_DOT,  K_SLSH, K_ENT },
        { K_NONE,   K_NONE, K_NONE, K_LGUI, K_LALT, K_SPC,  MO_L1,
          MO_L2,    K_SPC,  K_RIGHT,K_LEFT, K_UP,   K_DOWN, K_NONE },
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
