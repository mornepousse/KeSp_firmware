/* See chest_gate.h. */
#include "chest_gate.h"

/* Test seam for the CAS loop below (I1, review 2026-09-29): a no-op in the
 * firmware; the host test build (test/CMakeLists.txt) compiles this file
 * ONLY with -DCHEST_GATE_TEST_SEAM=chest_gate_test_seam_hook, so a single
 * test can race a concurrent chest_gate_take_oath_nav() into the middle of
 * the loop without instrumenting every other call site or test. */
#ifdef CHEST_GATE_TEST_SEAM
void CHEST_GATE_TEST_SEAM(void);   /* defined by the host test only */
#else
#define CHEST_GATE_TEST_SEAM() ((void)0)
#endif

static uint32_t s_pending_tag;   /* CHEST_TAG(op, instance), written by the link task */
static uint32_t s_press_tag;     /* tag stored at press time; 0 = none */
static bool     s_mode_next;
static int8_t   s_oath_nav;      /* accumulated cursor delta, saturating at +-16 */
static bool     s_oath_code;
static chest_gate_notify_fn s_notify;   /* chest_link.c's task wake-up; NULL = none */

void chest_gate_set_notify(chest_gate_notify_fn fn) { __atomic_store_n(&s_notify, fn, __ATOMIC_RELEASE); }

static void notify(void)
{
    chest_gate_notify_fn fn = __atomic_load_n(&s_notify, __ATOMIC_ACQUIRE);
    if (fn) fn();
}

void chest_gate_publish(uint16_t pending_op, uint8_t instance)
{ __atomic_store_n(&s_pending_tag, pending_op ? CHEST_TAG(pending_op, instance) : 0u, __ATOMIC_RELEASE); }

uint16_t chest_gate_pending(void) { return CHEST_TAG_OP(__atomic_load_n(&s_pending_tag, __ATOMIC_ACQUIRE)); }

bool chest_gate_press(void)
{
    uint32_t tag = __atomic_load_n(&s_pending_tag, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s_press_tag, tag, __ATOMIC_RELEASE);
    if (CHEST_TAG_OP(tag) == 0) return false;
    notify();
    return true;
}

uint32_t chest_gate_take_press(void) { return __atomic_exchange_n(&s_press_tag, 0u, __ATOMIC_ACQ_REL); }

void chest_gate_mode_next(void) { __atomic_store_n(&s_mode_next, true, __ATOMIC_RELEASE); notify(); }
bool chest_gate_take_mode_next(void) { return __atomic_exchange_n(&s_mode_next, false, __ATOMIC_ACQ_REL); }

void chest_gate_oath_nav(int8_t delta)
{
    /* TWO writers, unlike the other gate fields: key_processor.c accumulates
     * here, AND the link task's chest_gate_take_oath_nav() resets it to 0
     * via exchange — a plain load+store loses a step whenever a take lands
     * between this function's load and its store (measured net drift over
     * 5M balanced pairs: -128655, +118369, -62439; a CAS loop nets 0,
     * review 2026-09-29). */
    int8_t cur = __atomic_load_n(&s_oath_nav, __ATOMIC_RELAXED), next;
    do {
        int v = cur + delta;
        CHEST_GATE_TEST_SEAM();   /* test-only hook, empty in the firmware */
        next = (int8_t)(v > 16 ? 16 : v < -16 ? -16 : v);
    } while (!__atomic_compare_exchange_n(&s_oath_nav, &cur, next, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    notify();
}
int8_t chest_gate_take_oath_nav(void) { return __atomic_exchange_n(&s_oath_nav, (int8_t)0, __ATOMIC_ACQ_REL); }

void chest_gate_oath_code(void) { __atomic_store_n(&s_oath_code, true, __ATOMIC_RELEASE); notify(); }
bool chest_gate_take_oath_code(void) { return __atomic_exchange_n(&s_oath_code, false, __ATOMIC_ACQ_REL); }

bool sec_confirm_from_local(uint8_t col, uint8_t local_cols, uint8_t keymap_cols)
{ return keymap_cols <= local_cols || col < local_cols; }
