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
extern uint8_t stat_matrix_changed;
extern uint8_t last_layer;
extern uint8_t current_layout;
extern uint8_t is_layer_changed;
extern uint32_t last_activity_time_ms;

/* Key usage statistics */
extern uint32_t key_stats[MATRIX_ROWS][MATRIX_COLS];  /* Press count per key */
extern uint32_t key_stats_total;  /* Total keypresses tracked */

/* Bigram tracking: bigram_stats[prev_key][curr_key] where key = row * MATRIX_COLS + col */
#define NUM_KEYS (MATRIX_ROWS * MATRIX_COLS)
extern uint16_t bigram_stats[NUM_KEYS][NUM_KEYS];
extern uint32_t bigram_total;

/**
 * @brief Get key usage statistics
 * @param row Row index
 * @param col Column index
 * @return Number of times this key was pressed
 */
uint32_t get_key_stats(uint8_t row, uint8_t col);

/**
 * @brief Reset all key statistics to zero
 */
void reset_key_stats(void);

/**
 * @brief Reset all bigram statistics to zero
 */
void reset_bigram_stats(void);

/**
 * @brief Save bigram statistics to NVS
 */
void save_bigram_stats(void);

/**
 * @brief Load bigram statistics from NVS
 */
void load_bigram_stats(void);

/**
 * @brief Get the maximum bigram count (for normalization)
 */
uint16_t get_bigram_stats_max(void);

/**
 * @brief Get the maximum press count (for normalization)
 */
uint32_t get_key_stats_max(void);

/**
 * @brief Save key statistics to NVS
 */
void save_key_stats(void);

/**
 * @brief Load key statistics from NVS
 */
void load_key_stats(void);

/**
 * @brief Check if stats need saving (call periodically)
 */
void key_stats_check_save(void);

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


