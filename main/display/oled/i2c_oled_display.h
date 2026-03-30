#pragma once
#include <stdbool.h>
#include "display_types.h"

extern bool display_available;

void display_set_hw_config(const display_hw_config_t *config);
const display_hw_config_t *display_get_hw_config(void);

void init_display(void);
void display_clear_screen(void);
