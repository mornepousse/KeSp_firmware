/* Combos: press two keys simultaneously to trigger a different keycode.
   Example: J + K pressed together = Escape.
   Both keys must be pressed within COMBO_TIMEOUT_MS of each other. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef COMBO_TIMEOUT_MS
#define COMBO_TIMEOUT_MS 50
#endif

#define COMBO_MAX_SLOTS 16

typedef struct {
    uint8_t row1, col1;  /* First key position (V1 internal coords) */
    uint8_t row2, col2;  /* Second key position */
    uint8_t result;      /* HID keycode to emit */
} combo_config_t;

void combo_init(void);
void combo_set(uint8_t index, const combo_config_t *cfg);
const combo_config_t *combo_get(uint8_t index);

/* Called each scan cycle with current pressed keys.
   Returns: number of combo results ready to consume. */
int combo_process(const uint8_t press_row[6], const uint8_t press_col[6]);

/* Consume one resolved combo keycode (0 if none) */
uint8_t combo_consume(void);

/* NVS persistence */
void combo_save(void);
void combo_load(void);
