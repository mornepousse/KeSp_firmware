#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "keyboard_config.h"

#ifndef MATRIX_IRQ_ENABLED
#define MATRIX_IRQ_ENABLED 1
#endif

/* Sentinel value for "no key at this position" */
#define INVALID_KEY_POS  0xFF


extern uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t (*matrix_states[])[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t keycodes[6];
extern uint8_t current_press_row[6];
extern uint8_t current_press_col[6];
extern uint8_t current_press_stat[6];
extern volatile uint8_t stat_matrix_changed; /* written in ISR, read in task */
extern uint8_t last_layer;
/* current_layout: declared in keyboard_config.h */
extern volatile uint8_t is_layer_changed;    /* written in ISR, read in display task */
extern volatile uint32_t last_activity_time_ms;


/*
 * @brief deinitialize rtc pins
 */
void rtc_matrix_deinit(void);

/* Note: rtc_matrix_setup and IRQ setup/deinit were removed —
 * matrix setup/deinit are handled by the keyboard_button shim.
 */

/*
 * @brief initialize matrix
 */
void matrix_setup(void);

/*
 * Matrix scanning is handled asynchronously by the keyboard_button component.
 * The legacy `scan_matrix` and `scan_matrix_full_once` functions were removed.
 */

/* IRQ setup/deinit removed (handled by keyboard_button) */

void layer_changed(void);

uint32_t get_last_activity_time_ms(void);


