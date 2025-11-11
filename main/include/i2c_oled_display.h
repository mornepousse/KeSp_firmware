#pragma once

void init_display(void);
void display_test_text(char* text);
void display_clear_screen(void);
void write_text_to_display_centre(const char *text, int x, int y);
void write_text_to_display(const char *text, int x, int y);
void draw_rectangle(int x, int y, int w, int h);