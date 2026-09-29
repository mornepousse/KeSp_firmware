/* See chest_gate.h. */
#include "chest_gate.h"

static uint32_t s_pending_tag;   /* CHEST_TAG(op, instance), written by the link task */
static uint32_t s_press_tag;     /* tag stored at press time; 0 = none */
static bool     s_mode_next;
static int8_t   s_oath_nav;      /* accumulated cursor delta, saturating at +-16 */
static bool     s_oath_code;

void chest_gate_publish(uint16_t pending_op, uint8_t instance)
{ __atomic_store_n(&s_pending_tag, pending_op ? CHEST_TAG(pending_op, instance) : 0u, __ATOMIC_RELEASE); }

uint16_t chest_gate_pending(void) { return CHEST_TAG_OP(__atomic_load_n(&s_pending_tag, __ATOMIC_ACQUIRE)); }

bool chest_gate_press(void)
{
    uint32_t tag = __atomic_load_n(&s_pending_tag, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s_press_tag, tag, __ATOMIC_RELEASE);
    return CHEST_TAG_OP(tag) != 0;
}

uint32_t chest_gate_take_press(void) { return __atomic_exchange_n(&s_press_tag, 0u, __ATOMIC_ACQ_REL); }

void chest_gate_mode_next(void) { __atomic_store_n(&s_mode_next, true, __ATOMIC_RELEASE); }
bool chest_gate_take_mode_next(void) { return __atomic_exchange_n(&s_mode_next, false, __ATOMIC_ACQ_REL); }

void chest_gate_oath_nav(int8_t delta)
{
    /* Single writer (key_processor.c, one call per new press): a plain
     * load + saturate + store is enough, same discipline as s_pending_tag. */
    int32_t v = (int32_t)__atomic_load_n(&s_oath_nav, __ATOMIC_RELAXED) + delta;
    if (v > 16) v = 16;
    if (v < -16) v = -16;
    __atomic_store_n(&s_oath_nav, (int8_t)v, __ATOMIC_RELEASE);
}
int8_t chest_gate_take_oath_nav(void) { return __atomic_exchange_n(&s_oath_nav, (int8_t)0, __ATOMIC_ACQ_REL); }

void chest_gate_oath_code(void) { __atomic_store_n(&s_oath_code, true, __ATOMIC_RELEASE); }
bool chest_gate_take_oath_code(void) { return __atomic_exchange_n(&s_oath_code, false, __ATOMIC_ACQ_REL); }

bool sec_confirm_from_local(uint8_t col, uint8_t local_cols, uint8_t keymap_cols)
{ return keymap_cols <= local_cols || col < local_cols; }
