#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "chest_proto.h"                        /* CHEST_TAG */
/* Hand-off between the keyboard engine and the chest link task.
 * The link task publishes the chest's pending op AND the instance it was
 * seen on (protocol v2, spec 2026-09-29 §5); key_processor asks whether a
 * K_SEC_CONFIRM press is for the chest. Lock-free (one writer per field,
 * __atomic builtins), compiled on every keyboard board — without a chest,
 * nothing is ever published and every press stays local. Exception:
 * chest_gate_oath_nav()'s accumulator has TWO writers (see its comment) and
 * uses a CAS loop instead of a plain store. */
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
 * bound: browsing the account list carries no authority (Mae, 2026-09-29).
 * TWO writers on this one field (this function AND chest_gate_take_oath_nav's
 * exchange, from two different tasks): a CAS loop, not a plain store — a
 * plain load+store races the take and silently drops or duplicates a step
 * (review 2026-09-29, see chest_gate.c). */
void     chest_gate_oath_nav(int8_t delta);
/* key_processor: a new K_OATH_PREV/K_OATH_NEXT press. During a prompt (the
 * published pending op is non-zero) it is a CANCEL of that prompt: stores the
 * published tag (op, instance) AT PRESS TIME — like chest_gate_press() — and
 * queues NO cursor move. Otherwise it is chest_gate_oath_nav(delta). Accepted
 * from either half: a cancel can only refuse (contract §5, 0xC5). */
void     chest_gate_oath_key(int8_t delta);
/* link task: consume the cancel tag (0 = none) and clear it. Matched against
 * the CURRENT block by chest_cancel_request — a stale one is dropped. */
uint32_t chest_gate_take_cancel(void);
/* link task: consume the accumulated delta (0 = none) and clear it. The
 * other writer of s_oath_nav — see chest_gate_oath_nav()'s comment. */
int8_t   chest_gate_take_oath_nav(void);
/* link task: the epoch (chest_oath_visible_epoch) of the code visible on the
 * screen, 0 = none. Published after every round, 0 when the chest goes. */
void     chest_gate_publish_code(uint16_t epoch);
/* key_processor: a new K_OATH_CODE press. Stores a press record carrying the
 * code epoch published AT PRESS TIME — like chest_gate_press()'s tag, the
 * meaning of the press is what the screen showed when it was pressed:
 * epoch 0 = a request for the account under the cursor's code (not an
 * arming, the chest still needs K_SEC_CONFIRM); non-zero = type that code
 * if it is still the one visible (chest_oath_code_key, Mae 2026-10-01). A
 * second press before the take overwrites the record (one press, not a
 * counter). */
void     chest_gate_oath_code(bool local);
/* link task: consume the press record (0 = no press) and clear it. */
uint32_t chest_gate_take_oath_code(void);
#define CHEST_GATE_CODE_PRESS        0x10000u
#define CHEST_GATE_CODE_LOCAL_BIT    0x20000u
#define CHEST_GATE_CODE_LOCAL(r)     (((r) & CHEST_GATE_CODE_LOCAL_BIT) != 0u)
#define CHEST_GATE_CODE_PRESSED(r)   (((r) & CHEST_GATE_CODE_PRESS) != 0u)
#define CHEST_GATE_CODE_EPOCH(r)     ((uint16_t)((r) & 0xFFFFu))
/* Wake-up hook: chest_link.c registers a function that wakes its task
 * (xTaskNotifyGive) so a K_OATH_* / K_CHEST_NEXT press, or a K_SEC_CONFIRM
 * press that the chest takes, is served at once instead of at the next
 * 250 ms poll. Called from the keyboard task after the request is stored.
 * NULL (the default, and always on host builds and on boards without the
 * chest link): nothing is called — the gate stays free of FreeRTOS. */
typedef void (*chest_gate_notify_fn)(void);
void     chest_gate_set_notify(chest_gate_notify_fn fn);
/* Pure: is this keymap column one of the board's LOCAL (left) columns?
 * On a split master (keymap_cols > local_cols) the columns >= local_cols come
 * over the inter-half radio, which is unauthenticated: K_SEC_CONFIRM there is
 * ignored (Mae, 2026-09-29). Always true on a non-split board. */
bool     sec_confirm_from_local(uint8_t col, uint8_t local_cols, uint8_t keymap_cols);
