#pragma once
#include "keyboard_config.h"
extern uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS];
extern char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH];

/* Macro step: one keypress in a sequence */
#define MACRO_MAX_STEPS 24
typedef struct {
  uint8_t keycode;   /* HID keycode (0 = end, 0xFF = delay marker) */
  uint8_t modifier;  /* modifier mask, or delay in 10ms units if keycode=0xFF */
} macro_step_t;

#define MACRO_DELAY_MARKER 0xFF

/* Macro definition */
#define MAX_MACRO_NAME_LENGTH 16
#define MAX_MACROS 20
typedef struct {
  char name[MAX_MACRO_NAME_LENGTH];
  macro_step_t steps[MACRO_MAX_STEPS]; /* sequence of keypress/delay steps */
  uint8_t keys[6];                     /* legacy: simultaneous keys (backward compat) */
  uint16_t key_definition;
} macro_t;

void save_keymaps(uint16_t *data, size_t size_bytes);
void load_keymaps(uint16_t *data, size_t size_bytes);
void keymap_init_nvs(void);
void save_layout_names(char names[][MAX_LAYOUT_NAME_LENGTH], size_t layer_count);
void load_layout_names(char names[][MAX_LAYOUT_NAME_LENGTH], size_t layer_count);
void save_macros(macro_t *macros, size_t count);
void load_macros(macro_t *macros, size_t count);
void recalc_macros_count(void);

extern macro_t macros_list[MAX_MACROS];
extern size_t macros_count;
