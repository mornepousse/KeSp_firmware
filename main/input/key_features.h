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

/* ── Grave Escape ───────────────────────────────────────────────── */

/* Process grave escape: returns HID keycode based on active modifiers.
   If Shift or GUI held → grave (0x35), otherwise → ESC (0x29). */
uint8_t grave_esc_resolve(uint8_t active_mods);

/* ── Layer Lock ─────────────────────────────────────────────────── */

/* Toggle lock on the current momentary layer.
   If a MO layer is active, lock it (stays when MO key released).
   If already locked, unlock (return to base layer). */
void layer_lock_toggle(void);

/* Check if a layer is currently locked */
bool layer_lock_is_locked(void);

/* Get locked layer (-1 if none) */
int8_t layer_lock_get(void);

/* ── WPM Counter ────────────────────────────────────────────────── */

/* Record a keypress timestamp for WPM calculation */
void wpm_record_keypress(void);

/* Get current WPM (rolling average) */
uint16_t wpm_get(void);

/* Tick WPM sampling (call every ~1 second) */
void wpm_tick(void);

/* ── Double-Tap Shift → Caps Lock ──────────────────────────────── */

/* Call when a Shift key is newly pressed (LShift 0xE1 or RShift 0xE5).
   Returns true if double-tap detected → Caps Lock tap should be sent. */
bool shift_double_tap_press(void);

/* Tick the double-tap timer (call every ~10ms from keyboard task) */
void shift_double_tap_tick(void);

/* Check and consume pending Caps Lock tap (call from keyboard task) */
bool shift_double_tap_consume(void);

/* ── Key Override / Mod-Morph ───────────────────────────────────── */

#define KEY_OVERRIDE_MAX_SLOTS 16

typedef struct {
    uint8_t trigger_key;    /* HID keycode that triggers the override */
    uint8_t trigger_mod;    /* modifier mask that must be active */
    uint8_t result_key;     /* replacement HID keycode */
    uint8_t result_mod;     /* replacement modifier (0 = remove trigger mod) */
} key_override_t;

void key_override_init(void);
void key_override_set(uint8_t index, const key_override_t *cfg);
const key_override_t *key_override_get(uint8_t index);

/* Check if a key+mod combo should be overridden.
   Returns the override result keycode, or 0 if no override. */
uint8_t key_override_check(uint8_t keycode, uint8_t active_mods, uint8_t *out_mod);

void key_override_save(void);
void key_override_load(void);

/* ── Tri-Layer ──────────────────────────────────────────────────── */

/* Configure tri-layer: when layer1 AND layer2 are both active, activate result_layer.
   Set all to 0 to disable. */
void tri_layer_set(uint8_t layer1, uint8_t layer2, uint8_t result_layer);

/* Check tri-layer condition and return result layer, or -1 if not triggered.
   Call after layer changes. */
int8_t tri_layer_check(uint8_t active_layer, uint8_t last_layer);
