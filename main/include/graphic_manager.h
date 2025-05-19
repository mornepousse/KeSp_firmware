#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct {
    uint16_t      x;
    uint16_t      y;
    uint16_t      color;
    uint16_t      bg_color;
    char          *text;
    unsigned char is_changed;
}gtext_t;
gtext_t gtext_list[10];
uint8_t gtext_count = 0;
//struct gtext_t *gtext_get(uint8_t index);

gtext_t* gtext_create(uint16_t x, uint16_t y, uint16_t color, uint16_t bg_color, char *text);

void gtext_set_text(gtext_t *gtext, char *text);

void graphic_update();