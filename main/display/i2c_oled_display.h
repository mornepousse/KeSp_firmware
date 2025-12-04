#pragma once
#include <stdbool.h>
#include "display_types.h"

extern bool display_available;

void display_set_hw_config(const display_hw_config_t *config);
const display_hw_config_t *display_get_hw_config(void);

void init_display(void);
void display_test_text(char* text);
void display_clear_screen(void);
void write_text_to_display_centre(const char *text, int x, int y);
void write_text_to_display(const char *text, int x, int y);
void draw_rectangle(int x, int y, int w, int h);
void draw_rectangle_White(int x, int y, int w, int h);