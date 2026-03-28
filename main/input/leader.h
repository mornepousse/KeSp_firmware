/* Leader Key: press Leader, then a sequence of keys to trigger an action.
   Example: Leader → F → S = Ctrl+S
   Timeout between keys: LEADER_TIMEOUT_MS */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef LEADER_TIMEOUT_MS
#define LEADER_TIMEOUT_MS 1000
#endif

#define LEADER_MAX_SEQ_LEN   4
#define LEADER_MAX_ENTRIES   16
#define K_LEADER             0x3400  /* Leader key keycode */

typedef struct {
    uint8_t sequence[LEADER_MAX_SEQ_LEN]; /* HID keycodes in order (0 = end) */
    uint8_t result;                        /* HID keycode to emit */
    uint8_t result_mod;                    /* modifier mask to apply with result */
} leader_entry_t;

void leader_init(void);
void leader_set(uint8_t index, const leader_entry_t *entry);
const leader_entry_t *leader_get(uint8_t index);

/* Start leader mode (called when leader key is pressed) */
void leader_start(void);

/* Feed a keycode into the leader sequence. Returns true if absorbed. */
bool leader_feed(uint8_t keycode);

/* Tick — check timeout. Returns true if leader sequence just resolved. */
bool leader_tick(void);

/* Get resolved action (0 if none). Resets after consume. */
uint8_t leader_consume(uint8_t *out_mod);

/* Check if leader mode is active */
bool leader_is_active(void);

/* NVS persistence */
void leader_save(void);
void leader_load(void);
