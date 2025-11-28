#pragma once
#include "keyboard_config.h"
extern uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS];

// Structure d'une macro (liste de touches + longueur)
#define MAX_MACRO_NAME_LENGTH 16
#define MAX_MACROS 20
typedef struct {
  char name[MAX_MACRO_NAME_LENGTH];
  uint8_t keys[6]; // Jusqu'à 6 touches simultanées (HID limit)
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
