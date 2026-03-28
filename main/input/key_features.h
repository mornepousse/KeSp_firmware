/* Advanced key features: OSM, OSL, Caps Word, Repeat Key.
   Lightweight state machines called from key_processor. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ── One-Shot Modifier ───────────────────────────────────────────── */

/* Arm a one-shot modifier (will apply to next keypress) */
void osm_arm(uint8_t mod_mask);

/* Get and consume active one-shot mods (returns mask, resets after) */
uint8_t osm_consume(void);

/* Check if any OSM is armed */
bool osm_is_active(void);

/* ── One-Shot Layer ──────────────────────────────────────────────── */

/* Arm a one-shot layer (next keypress uses this layer, then returns) */
void osl_arm(uint8_t layer);

/* Get active one-shot layer (-1 if none). Consumed after one keypress. */
int8_t osl_get_layer(void);

/* Consume the one-shot layer (call after keypress processed) */
void osl_consume(void);

/* ── Caps Word ───────────────────────────────────────────────────── */

/* Toggle caps word mode */
void caps_word_toggle(void);

/* Check if caps word is active */
bool caps_word_is_active(void);

/* Process a keycode through caps word — returns modified keycode and modifier.
   Deactivates caps word on non-alpha keys (space, enter, etc.) */
void caps_word_process(uint8_t *keycode, uint8_t *modifier);

/* ── Repeat Key ──────────────────────────────────────────────────── */

/* Record the last non-modifier keypress */
void repeat_key_record(uint8_t keycode);

/* Get the keycode to repeat (0 if nothing recorded) */
uint8_t repeat_key_get(void);
