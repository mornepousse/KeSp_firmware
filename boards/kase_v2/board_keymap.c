#include "keymap.h"
#include "key_definitions.h"
#include "keyboard_config.h"

#define DEFAULT 0x100
#define LOWER 0x101
#define RAISE 0x102

// array to hold names of layouts for oled
char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH] = {
    "MAIN",
    "NAV",
    "LAYER 2",
    "LAYER 3",
    "LAYER 4",
    "LAYER 5",
    "LAYER 6",
    "LAYER 7",
    "LAYER 8",
    "LAYER 9",
};

// Fillers to make layering more clear
#define _______ K_TRNS
#define XXXXXXX K_NO

uint16_t keymaps[LAYERS][MATRIX_ROWS][MATRIX_COLS] = {

    {// QWERTY
     {K_DEL,   K_1,    K_2,    K_3,    K_4,      K_5,     K_LBRC,  K_6,     K_7,     K_8,     K_9,    K_0,    K_EQL},
     {K_TAB,   K_Q,    K_W,    K_E,    K_R,      K_T,     MO_L1,   K_Y,     K_U,     K_I,     K_O,    K_P,    K_SLSH},
     {K_RALT,  K_A,    K_S,    K_D,    K_F,      K_G,     K_RBRC,  K_H,     K_J,     K_K,     K_L,    K_SCLN, K_MINUS},
     {K_LCTRL, K_Z,    K_X,    K_C,    K_V,      K_B,     K_LWIN,  K_N,     K_M,     K_COMM,  K_DOT,  K_QUOT, K_GRV},
     {K_ESC,   K_ENT,  K_LALT, K_LWIN, K_LSHIFT, K_SPACE, MO_L1,   K_BSPACE,K_ENT,  K_BSLSH, K_RWIN, K_HELP, TO_L2}
    },

    {
      {K_NO, K_NO, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_NO, K_NO, K_L_PARENTHESIS},
     {K_NO, K_F1, K_HOME, K_NO, K_END, K_NO, K_NO, K_NO, K_NO, K_NO, K_F10, K_F11, K_NO},
     {K_NO, K_NO, K_LEFT, K_UP, K_RIGHT, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_F12, K_R_PARENTHESIS},
     {K_NO, K_NO, K_NO, K_DOWN, MACRO_2, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    }
    };
