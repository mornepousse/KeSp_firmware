/* See chest_gate.h. */
#include "chest_gate.h"

static uint16_t s_pending;   /* written by the link task */
static bool     s_press;     /* set by key_processor, taken by the link task */

void chest_gate_publish(uint16_t pending_op) { __atomic_store_n(&s_pending, pending_op, __ATOMIC_RELEASE); }
uint16_t chest_gate_pending(void) { return __atomic_load_n(&s_pending, __ATOMIC_ACQUIRE); }

bool chest_gate_press(void)
{
    if (chest_gate_pending() == 0) return false;
    __atomic_store_n(&s_press, true, __ATOMIC_RELEASE);
    return true;
}

bool chest_gate_take_press(void) { return __atomic_exchange_n(&s_press, false, __ATOMIC_ACQ_REL); }
