/* See chest_gate.h. */
#include "chest_gate.h"

static uint16_t s_pending;   /* written by the link task */
static uint16_t s_press_op;  /* op stored by key_processor at press time; 0 = none */

void chest_gate_publish(uint16_t pending_op) { __atomic_store_n(&s_pending, pending_op, __ATOMIC_RELEASE); }
uint16_t chest_gate_pending(void) { return __atomic_load_n(&s_pending, __ATOMIC_ACQUIRE); }

bool chest_gate_press(void)
{
    uint16_t op = chest_gate_pending();
    __atomic_store_n(&s_press_op, op, __ATOMIC_RELEASE);
    return op != 0;
}

uint16_t chest_gate_take_press(void) { return __atomic_exchange_n(&s_press_op, 0, __ATOMIC_ACQ_REL); }
