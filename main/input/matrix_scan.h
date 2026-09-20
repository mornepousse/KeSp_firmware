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


/* The dongle has no matrix: no keymap, no per-position statistics, no
 * matrix state. Declaring these symbols for it forced its board to invent
 * dimensions — which board.h no longer does. */
#if !CONFIG_KASE_NO_KEYMAP_ENGINE
extern uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t (*matrix_states[])[MATRIX_ROWS][MATRIX_COLS];
#endif
extern uint8_t keycodes[6];
extern uint8_t current_press_row[6];
extern uint8_t current_press_col[6];
extern uint8_t current_press_stat[6];

#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
/* Replays the fusion of keys received from the remote half. Idempotent:
 * the keyboard task calls it every cycle, because the scan callback only
 * runs on local activity. Remote source: half_link (master) or kbd_relay
 * (fusion, right re-sent by the dongle in USB mode). */
void matrix_apply_remote(void);
#endif
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

void rtc_matrix_deinit(void);
void matrix_setup(void);

/* Light-sleep matrix key-wake (V2D wireless): arm/disarm matrix GPIOs as a
 * keypress wake source around esp_light_sleep_start(). */
void matrix_arm_key_wake(void);
/* Stamps activity to now — call on wake-up, BEFORE the keyboard
 * loop re-evaluates inactivity. */
void matrix_mark_activity(void);
/* Scans the matrix once by hand and publishes what a callback would have
 * published. Call right after returning from light sleep, BEFORE matrix_setup():
 * the key that woke the board is pressed at that instant, later it may not
 * be anymore. */
void matrix_wake_capture(void);
/* Did the last capture find at least one key? Used by veille.c
 * to tell a real ghost apart from a first edge read during the debounce. */
bool matrix_wake_had_keys(void);
/* Call AFTER matrix_setup() and BEFORE matrix_wake_reconcile(): waits until
 * the recreated driver has spoken (first event) or the grace period derived
 * from its debounce has elapsed (wake_grace.h). Never a bare tick: vTaskDelay(1)
 * waits until the next tick boundary — between ~0 and 10 ms — and a
 * HELD key was wrongly released when the phase landed badly. */
void matrix_wake_wait_first_scan(void);
/* After matrix_wake_wait_first_scan(): if the driver reported nothing although a
 * key had been captured, it was released in the meantime — publishes the
 * release and returns true (the caller emits it). */
bool matrix_wake_reconcile(void);
void matrix_disarm_key_wake(void);

/* Matrix test mode: when true, scan callback sends key events
   via CDC binary protocol instead of filling HID reports */
extern volatile bool matrix_test_mode;


