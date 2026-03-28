/* Tap/Hold engine: distinguishes taps from holds for LT and MT keycodes.
   - Tap: press + release within timeout, no other key interrupts
   - Hold: held past timeout OR interrupted by another key press */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef TAP_HOLD_TIMEOUT_MS
#define TAP_HOLD_TIMEOUT_MS 200
#endif

/* Maximum simultaneous tap/hold keys being tracked */
#define TAP_HOLD_MAX_PENDING 4

typedef enum {
    TH_IDLE,        /* No tap/hold in progress */
    TH_PENDING,     /* Key pressed, waiting for tap or hold decision */
    TH_HOLD,        /* Decided: it's a hold */
    TH_TAP,         /* Decided: it's a tap */
} tap_hold_state_t;

typedef struct {
    tap_hold_state_t state;
    uint16_t keycode;       /* The LT/MT keycode */
    uint8_t row;            /* Matrix position */
    uint8_t col;
    uint32_t press_time_ms; /* When the key was pressed */
} tap_hold_entry_t;

/* Initialize the tap/hold engine */
void tap_hold_init(void);

/* Called when a key is pressed. Returns true if it's a tap/hold key (absorbed). */
bool tap_hold_on_press(uint16_t keycode, uint8_t row, uint8_t col);

/* Called when a key is released. Returns true if it was a tracked tap/hold key. */
bool tap_hold_on_release(uint8_t row, uint8_t col);

/* Called every scan cycle to check timeouts. May resolve pending keys to HOLD. */
void tap_hold_tick(void);

/* Called when a non-tap-hold key is pressed (interrupts pending tap/holds → HOLD). */
void tap_hold_interrupt(void);

/* Get the resolved action for a position. Returns 0 if nothing resolved. */
uint16_t tap_hold_get_resolved(uint8_t row, uint8_t col, bool *is_hold);

/* Get active hold modifier mask (for MT keys currently held) */
uint8_t tap_hold_get_active_mods(void);

/* Get active hold layer (for LT keys currently held), -1 if none */
int8_t tap_hold_get_active_layer(void);

/* Consume one resolved tap. Returns the HID tap keycode, 0 if none pending. */
uint8_t tap_hold_consume_tap(void);

/* Returns true if tick() just resolved a pending key to HOLD (need to send report) */
bool tap_hold_hold_just_activated(void);
