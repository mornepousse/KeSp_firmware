/* Key processing: converts matrix state to keycodes.
   Handles layer switching, macro expansion, internal functions. */
#pragma once

#include <stdint.h>

/* Build keycodes[6] from current_press_row/col using the active keymap layer.
   Applies momentary/toggle layers, macros, and internal function detection. */
void build_keycode_report(void);

/* Process pending internal functions (BT toggle, layer toggle, etc.) */
void process_matrix_changes(void);

/* State used by the processor */
extern uint16_t keypress_internal_function;
extern uint8_t current_row_layer_changer;
extern uint8_t current_col_layer_changer;
extern uint16_t extra_keycodes[6];
