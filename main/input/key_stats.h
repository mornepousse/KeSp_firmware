/* Keystroke statistics and bigram tracking with NVS persistence.
   Reusable — depends only on keyboard_config.h for matrix dimensions. */
#pragma once

#include <stdint.h>
#include "keyboard_config.h"

/* key_stats_total stays declared on every role: the CDC monitor exposes it,
 * and the dongle keeps it at zero (comm/rf/dongle_state.c) rather than
 * amputating the frame format. */
extern uint32_t key_stats_total;

/* Neither the dongle nor the mouse has a matrix: no keymap, no per-position
 * statistics, no matrix state. Declaring these symbols for them would force
 * their board to invent dimensions — which their board.h does not do. */
#if !CONFIG_KASE_NO_KEYMAP_ENGINE
/* Key press counts per position */
extern uint32_t key_stats[MATRIX_ROWS][MATRIX_COLS];

/* Sequential key pair (bigram) counts */
#define NUM_KEYS (MATRIX_ROWS * MATRIX_COLS)
extern uint16_t bigram_stats[NUM_KEYS][NUM_KEYS];
extern uint32_t bigram_total;
#endif

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
