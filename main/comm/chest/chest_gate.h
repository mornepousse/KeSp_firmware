#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Hand-off between the keyboard engine and the chest link task.
 * The link task publishes the chest's pending op; key_processor asks whether
 * a K_SEC_CONFIRM press is for the chest. Lock-free (one writer per field,
 * __atomic builtins), compiled on every keyboard board — without a chest,
 * nothing is ever published and every press stays local. */
void     chest_gate_publish(uint16_t pending_op);  /* link task only; 0 = none */
uint16_t chest_gate_pending(void);
/* SECURITY: called ONLY by key_processor.c on a new physical press
 * (scripts/tripwire.d/chest-confirm.sh). Stores the op code that was pending
 * AT PRESS TIME (0 = none), not a live reference to it: a press seen for op A
 * must confirm only op A, even if the chest's pending op changes — or drops
 * out, or the link misses several rounds (chest reboot, CRC noise, a busy
 * bus) — before the link task gets to consume it. Returns true iff that op
 * was non-zero (unchanged meaning for key_processor). */
bool     chest_gate_press(void);
/* link task: consume the op stored by chest_gate_press() (0 = none) and clear
 * it (atomic exchange). The caller must still check the returned op against
 * the chest's CURRENT pending op (chest_proto.h's chest_press_matches) before
 * treating it as a confirmation — a stale press that no longer matches the
 * operation on screen is dropped, never applied to a different one. */
uint16_t chest_gate_take_press(void);
