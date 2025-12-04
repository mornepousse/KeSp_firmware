#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "keyboard_config.h"


extern uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t (*matrix_states[])[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t keycodes[6];
extern uint8_t current_press_row[6];
extern uint8_t current_press_col[6];
extern uint8_t current_press_stat[6];
extern uint8_t stat_matrix_changed;
extern uint8_t last_layer;
extern uint8_t current_layout;
extern uint8_t is_layer_changed;
extern uint32_t last_activity_time_ms;
//extern
/*
 * @brief deinitialize rtc pins
 */
void rtc_matrix_deinit(void);

/*
 * @brief initialize rtc pins
 */
void rtc_matrix_setup(void);

/*
 * @brief initialize matrix
 */
void matrix_setup(void);

/*
 * @brief scan matrix
 */
void scan_matrix(void);


void layer_changed(void);

uint32_t get_last_activity_time_ms(void);


