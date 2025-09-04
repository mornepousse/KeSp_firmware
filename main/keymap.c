#include "keymap.h"
#include "key_definitions.h"
#include "keyboard_config.h"

// A bit different from QMK, default returns you to the first layer, LOWER and
// raise increase/lower layer by order.
#define DEFAULT 0x100
#define LOWER 0x101
#define RAISE 0x102

// Keymaps are designed to be relatively interchangeable with QMK
enum custom_keycodes
{
  QWERTY,
  NUM,
  PLUGINS,
};

// Set these for each layer and use when layers are needed in a hold-to use
// layer
enum layer_holds
{
  QWERTY_H = LAYER_HOLD_BASE_VAL,
  NUM_H,
  FUNCS_H
};

// array to hold names of layouts for oled
char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH] = {
    "DVORAK",
    "EXTRA",
    "QWERTY",
};

// Fillers to make layering more clearaoeuouoeu
#define _______ K_TRNS
#define XXXXXXX K_NO
#ifdef VERSION_1

uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

    {// dvorak sous qwerty inter
     {K_ESC, K_ENT, /**/ K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, /**/ TO_L3,
      K_INT3, /**/ K_LBRC},
     {K_DEL, K_1, K_COMM, K_DOT, K_P, K_Y, K_F, K_G, K_C, K_R, K_0, K_EQUAL,
      /**/ MO_L2},
     {K_TAB, K_QUOT, K_O, K_E, K_U, K_I, K_D, K_H, K_T, K_N, K_L, K_SLSH,
      /**/ K_RBRC},
     {K_RALT, K_A, K_Q, K_J, K_K, K_X, K_B, K_M, K_W, K_V, K_S, K_MINUS,
      /**/ K_RSHIFT},
     {K_LCTRL, K_SCLN, K_LALT, K_LWIN, K_LSHIFT, K_SPACE, K_BSPACE, K_ENTER,
      K_BSLSH, K_Z, K_Z, K_GRV, /**/ K_NO}},
    {{K_NO, K_NO, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_NO, K_NO,
      K_L_PARENTHESIS},
     {K_NO, K_F1, K_HOME, K_NO, K_END, K_NO, K_NO, K_NO, K_NO, K_NO, K_F10, K_F11,
      K_NO},
     {K_NO, K_NO, K_LEFT, K_UP, K_RIGHT, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_F12,
      K_R_PARENTHESIS},
     {K_NO, K_NO, K_NO, K_DOWN, MACRO_2, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO,
      K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO,
      K_NO}},
    {// dvorak sous layeur custom pour windows/macos
     {K_ESC, K_ENT, /**/ K_2, K_3, K_4, K_5, K_6,
      K_7, K_8, K_9, /**/ TO_L1, K_INT3, /**/ K_MINUS},
     /////////////////
     /////////////////////////////////////////////
     {K_DEL, K_1, K_W, K_E, K_R, K_T, K_Y,
      K_U, K_I, K_O, K_0, K_RBRC, /**/ MO_L2},
     {K_TAB, K_Q, K_S, K_D, K_F, K_G, K_H,
      K_J, K_K, K_L, K_P, K_LBRC, /**/ K_EQUAL},
     {K_RALT, K_A, K_X, K_C, K_V, K_B, K_N,
      K_M, K_COMM, K_DOT, K_SCLN, K_QUOT, /**/ K_RSHIFT},
     {K_LCTRL, K_Z, K_LALT, K_LWIN, K_LSHIFT, K_SPACE, K_BSPACE, K_ENTER, K_BSLSH,
      K_DELETE, K_SLSH, K_GRV, /**/ K_NO}}};

#endif

#ifdef VERSION_2

uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

    {// dvorak sous qwerty inter
     {K_DEL,  K_1,    K_2,    K_3,    K_4,      K_5,     K_LBRC,   K_6,     K_7,     K_8,      K_9,   K_0,    K_EQL},
     {K_TAB,  K_QUOT, K_COMM, K_DOT,  K_P,      K_Y,      MO_L2,    K_F,     K_G,     K_C,      K_R,   K_L,    K_SLSH},
     {K_RALT,  K_A,    K_O,    K_E,    K_U,      K_I,     K_RBRC,  K_D,     K_H,     K_T,     K_N,   K_S,   K_MINUS},
     {K_LCTRL, K_SCLN, K_Q,    K_J,    K_K,      K_X,     K_LWIN,  K_B,     K_M,     K_W,     K_V,   K_Z,   K_GRV},
     {K_ESC,  K_ENT,   K_LALT, K_LWIN, K_LSHIFT, K_SPACE, MO_L2,    K_BSPACE,K_ENT, K_BSLSH, K_RWIN,K_HELP,TO_L3}
    },

    {
      {K_NO, K_NO, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_NO, K_NO, K_L_PARENTHESIS},
     {K_NO, K_F1, K_HOME, K_NO, K_END, K_NO, K_NO, K_NO, K_NO, K_NO, K_F10, K_F11, K_NO},
     {K_NO, K_NO, K_LEFT, K_UP, K_RIGHT, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_F12, K_R_PARENTHESIS},
     {K_NO, K_NO, K_NO, K_DOWN, MACRO_2, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {// dvorak sous layeur custom pour windows/macos
     {K_ESC, K_ENT, /**/ K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, /**/ TO_L1, K_INT3, /**/ K_MINUS},
     {K_DEL, K_1, K_W, K_E, K_R, K_T, K_Y, K_U, K_I, K_O, K_0, K_RBRC, /**/ MO_L2},
     {K_TAB, K_Q, K_S, K_D, K_F, K_G, K_H, K_J, K_K, K_L, K_P, K_LBRC, /**/ K_EQUAL},
     {K_RALT, K_A, K_X, K_C, K_V, K_B, K_N, K_M, K_COMM, K_DOT, K_SCLN, K_QUOT, /**/ K_RSHIFT},
     {K_LCTRL, K_Z, K_LALT, K_LWIN, K_LSHIFT, K_SPACE, K_BSPACE, K_ENTER, K_BSLSH, K_DELETE, K_SLSH, K_GRV, /**/ K_NO}
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

#endif