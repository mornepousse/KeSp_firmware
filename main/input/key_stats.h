/* Keystroke statistics and bigram tracking with NVS persistence.
   Reusable — depends only on keyboard_config.h for matrix dimensions. */
#pragma once

#include <stdint.h>
#include "keyboard_config.h"

/* Key press counts per position */
extern uint32_t key_stats[MATRIX_ROWS][MATRIX_COLS];
extern uint32_t key_stats_total;

/* Sequential key pair (bigram) counts */
#define NUM_KEYS (MATRIX_ROWS * MATRIX_COLS)
extern uint16_t bigram_stats[NUM_KEYS][NUM_KEYS];
extern uint32_t bigram_total;

/* Record a new keypress at (row, col) — updates stats + bigrams */
void key_stats_record_press(uint8_t row, uint8_t col);

/* Query */
uint32_t get_key_stats_val(uint8_t row, uint8_t col);
uint32_t get_key_stats_max(void);
uint16_t get_bigram_stats_max(void);

/* Reset */
void reset_key_stats(void);
void reset_bigram_stats(void);

/* NVS persistence */
void save_key_stats(void);
void load_key_stats(void);
void save_bigram_stats(void);
void load_bigram_stats(void);
void key_stats_check_save(void);
