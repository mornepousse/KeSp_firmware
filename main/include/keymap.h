#pragma once
#include "keyboard_config.h"
extern uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS];

void save_keymaps(uint16_t *data, size_t size_bytes);
void load_keymaps(uint16_t *data, size_t size_bytes);
void keymap_init_nvs(void);
