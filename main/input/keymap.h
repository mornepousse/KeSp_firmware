#pragma once
#include <stddef.h>   /* size_t — needed for function signatures */
#include <stdbool.h>  /* bool — return of save_* (NVS persistence failure) */
#include "keyboard_config.h"
/* Neither the dongle nor the mouse has a matrix: no keymap, no per-position
 * statistics, no matrix state. Declaring these symbols for them would force
 * their board to invent dimensions — which their board.h files do not do. */
#if !CONFIG_KASE_NO_KEYMAP_ENGINE
/* KEYMAP_COLS, not MATRIX_COLS: on the master half of a split the keymap
 * covers both halves while the scan only covers one. */
extern uint16_t keymaps[][MATRIX_ROWS][KEYMAP_COLS];
extern char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH];
#endif

/* Macro step: one keypress in a sequence */
#define MACRO_MAX_STEPS 24
typedef struct {
  uint8_t keycode;   /* HID keycode (0 = end, 0xFF = delay marker) */
  uint8_t modifier;  /* modifier mask, or delay in 10ms units if keycode=0xFF */
} macro_step_t;

#define MACRO_DELAY_MARKER 0xFF

/* Macro definition */
#define MAX_MACRO_NAME_LENGTH 16
#define MAX_MACROS 20
typedef struct {
  char name[MAX_MACRO_NAME_LENGTH];
  macro_step_t steps[MACRO_MAX_STEPS]; /* sequence of keypress/delay steps */
  uint8_t keys[6];                     /* legacy: simultaneous keys (backward compat) */
  uint16_t key_definition;
} macro_t;

bool save_keymaps(uint16_t *data, size_t size_bytes);   /* true = persisted OK */
/* Size of the "keymaps" blob in NVS — THE single source, on both sides.
 *
 * It is computed from KEYMAP_COLS, NOT from MATRIX_COLS: on the master of a
 * split, the keymap covers both halves (14 columns) while the local matrix
 * only scans 7. The two formulas used to coexist — write in
 * KEYMAP_COLS (1120 bytes), read in MATRIX_COLS (560) — and load_keymaps'
 * size guard therefore rejected the blob at EVERY boot, keeping the factory
 * values. The user's remapping was written, never read back,
 * and nothing signaled it. Found on the bench on 2026-09-07.
 *
 * Locked in by test/test_keymap_blob_size.c. */
#define KEYMAP_BLOB_BYTES \
    ((size_t)LAYERS * MATRIX_ROWS * KEYMAP_COLS * sizeof(uint16_t))

void load_keymaps(uint16_t *data, size_t size_bytes);
void keymap_init_nvs(void);
bool save_layout_names(char names[][MAX_LAYOUT_NAME_LENGTH], size_t layer_count);
void load_layout_names(char names[][MAX_LAYOUT_NAME_LENGTH], size_t layer_count);
bool save_macros(macro_t *macros, size_t count);
void load_macros(macro_t *macros, size_t count);
void recalc_macros_count(void);

extern macro_t macros_list[MAX_MACROS];
extern size_t macros_count;
