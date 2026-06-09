/* main/security/sec_confirm.h — physical-keypress confirmation gate.
 * Pure (no NVS/HW); compiled for all roles so key_processor can call it. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SEC_CONFIRM_TIMEOUT_MS 15000u

typedef enum {
    SEC_CONFIRM_IDLE = 0,
    SEC_CONFIRM_PENDING,
    SEC_CONFIRM_AUTHORIZED,
    SEC_CONFIRM_TIMEDOUT,
} sec_confirm_state_t;

void sec_confirm_reset(void);
/* Arm a pending request for `slot`, stamped at now_ms. Overwrites any prior pending. */
void sec_confirm_arm(uint8_t slot, uint32_t now_ms);
/* Physical confirm key pressed: PENDING -> AUTHORIZED; no-op otherwise. */
void sec_confirm_authorize(void);
/* Poll at now_ms. PENDING past timeout -> returns TIMEDOUT once (then IDLE).
 * AUTHORIZED -> writes slot to *out_slot, consumes (-> IDLE), returns AUTHORIZED. */
sec_confirm_state_t sec_confirm_poll(uint32_t now_ms, uint8_t *out_slot);
