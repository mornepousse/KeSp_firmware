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

/* Check if a key should be deferred (it belongs to a combo) */
bool combo_should_defer(uint8_t row, uint8_t col);

/* Defer a key press — hold it back while waiting for combo partner */
void combo_defer_key(uint8_t row, uint8_t col, uint8_t keycode);

/* Check if a position is currently deferred or part of an active combo */
bool combo_is_suppressed(uint8_t row, uint8_t col);

/* Called each scan cycle with current pressed keys.
   Returns: number of combo results ready to consume. */
int combo_process(const uint8_t press_row[6], const uint8_t press_col[6]);

/* Consume one resolved combo keycode (0 if none).
   If out_r1/c1/r2/c2 are non-NULL, returns the positions of the combo keys to suppress. */
uint8_t combo_consume(uint8_t *out_r1, uint8_t *out_c1, uint8_t *out_r2, uint8_t *out_c2);

/* Consume one expired deferred key (timeout, no combo). Returns HID keycode or 0. */
uint8_t combo_consume_expired(void);

/* NVS persistence */
void combo_save(void);
void combo_load(void);
