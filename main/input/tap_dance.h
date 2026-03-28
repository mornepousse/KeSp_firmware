/* Tap Dance: multiple tap count triggers different actions.
   - 1 tap = action[0]
   - 2 taps = action[1]
   - 3 taps = action[2]
   - hold = action[3]
   Taps must occur within TAP_DANCE_TIMEOUT_MS of each other. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef TAP_DANCE_TIMEOUT_MS
#define TAP_DANCE_TIMEOUT_MS 200
#endif

#define TAP_DANCE_MAX_SLOTS  16
#define TAP_DANCE_MAX_TAPS   3

typedef struct {
    uint8_t actions[4]; /* HID keycodes: [0]=1tap, [1]=2taps, [2]=3taps, [3]=hold */
} tap_dance_config_t;

/* Initialize tap dance engine */
void tap_dance_init(void);

/* Configure a dance slot. actions[0..3] = HID keycodes for 1tap/2tap/3tap/hold */
void tap_dance_set(uint8_t index, const uint8_t actions[4]);

/* Called when a TD key is pressed. Returns true if absorbed. */
bool tap_dance_on_press(uint8_t index, uint8_t row, uint8_t col);

/* Called when a TD key is released. */
void tap_dance_on_release(uint8_t row, uint8_t col);

/* Called every ~10ms. Resolves dances that timed out. */
void tap_dance_tick(void);

/* Get the resolved keycode (0 if nothing resolved yet) */
uint8_t tap_dance_consume(void);

/* Check if a dance just resolved (for keyboard_task to know when to send) */
bool tap_dance_just_resolved(void);
