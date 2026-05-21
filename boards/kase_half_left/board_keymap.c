/*
 * Keymap placeholder for kase_half_left.
 * The half firmware does NOT use the keymap engine — key events are
 * transmitted raw to the dongle. This file satisfies linker symbol
 * requirements from engine code compiled unconditionally.
 */
#include "key_definitions.h"
#include "keymap.h"
#include "keyboard_config.h"

/* Minimal LAYERS-layer keymap (35 positions each, all transparent / 0). */
uint16_t keymaps[LAYERS][MATRIX_ROWS][MATRIX_COLS] = {
    /* Layer 0 — placeholder (half never processes keycodes locally) */
    {
        { 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0 },
    },
    /* Remaining LAYERS-1 layers: zero-init by C. */
};
