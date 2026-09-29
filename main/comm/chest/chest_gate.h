#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "chest_proto.h"                        /* CHEST_TAG */
/* Hand-off between the keyboard engine and the chest link task.
 * The link task publishes the chest's pending op AND the instance it was
 * seen on (protocol v2, spec 2026-09-29 §5); key_processor asks whether a
 * K_SEC_CONFIRM press is for the chest. Lock-free (one writer per field,
 * __atomic builtins), compiled on every keyboard board — without a chest,
 * nothing is ever published and every press stays local. */
void     chest_gate_publish(uint16_t pending_op, uint8_t instance);   /* link task only; op 0 = none */
uint16_t chest_gate_pending(void);             /* op only (0 = none) */
/* SECURITY: called ONLY by key_processor.c on a new physical press
 * (scripts/tripwire.d/chest-confirm.sh). Stores the tag (op, instance) that
 * was pending AT PRESS TIME (0 = none), not a live reference to it: a press
 * seen for (op A, instance N) must confirm only that pairing, even if the
 * chest's pending op changes — or drops out, or the link misses several
 * rounds (chest reboot, CRC noise, a busy bus) — before the link task gets
 * to consume it. Returns true iff that op was non-zero (unchanged meaning
 * for key_processor). */
bool     chest_gate_press(void);
/* link task: consume the tag stored by chest_gate_press() (0 = none) and
 * clear it (atomic exchange). The caller must still check the returned tag
 * against the chest's CURRENT pending op and instance (chest_proto.h's
 * chest_press_matches) before treating it as a confirmation — a stale press
 * that no longer matches the operation on screen is dropped, never applied
 * to a different one. */
uint32_t chest_gate_take_press(void);
/* key_processor: a new K_CHEST_NEXT press (not a security gesture — no
 * arming to record, just a request to cycle the USB mode). */
void     chest_gate_mode_next(void);
/* link task: consume the request (false = none pending). */
bool     chest_gate_take_mode_next(void);
/* key_processor: a new K_OATH_PREV/K_OATH_NEXT press — accumulate the cursor
 * move (delta = -1 or +1), saturating at +-16 pending steps. Not security-
 * bound: browsing the account list carries no authority (Mae, 2026-09-29). */
void     chest_gate_oath_nav(int8_t delta);
/* link task: consume the accumulated delta (0 = none) and clear it. */
int8_t   chest_gate_take_oath_nav(void);
/* key_processor: a new K_OATH_CODE press (a request for the account under
 * the cursor's code — not an arming, the chest still needs K_SEC_CONFIRM). */
void     chest_gate_oath_code(void);
/* link task: consume the request (false = none pending). */
bool     chest_gate_take_oath_code(void);
/* Pure: is this keymap column one of the board's LOCAL (left) columns?
 * On a split master (keymap_cols > local_cols) the columns >= local_cols come
 * over the inter-half radio, which is unauthenticated: K_SEC_CONFIRM there is
 * ignored (Mae, 2026-09-29). Always true on a non-split board. */
bool     sec_confirm_from_local(uint8_t col, uint8_t local_cols, uint8_t keymap_cols);
