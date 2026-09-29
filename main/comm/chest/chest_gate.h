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
 * (scripts/tripwire.d/chest-confirm.sh). True = the press was for the chest. */
bool     chest_gate_press(void);
bool     chest_gate_take_press(void);              /* link task: consume one press */
