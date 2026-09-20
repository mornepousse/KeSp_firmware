/* Combo engine tests — REAL linked module (../main/input/combo.c).
 * Combo = 2 keys deferred then present together within the
 * COMBO_TIMEOUT_MS window -> result emitted, both source keys suppressed.
 * Shared host_clock clock to test the window / the timeout. */
#include "test_framework.h"
#include "combo.h"
#include "host_clock.h"

#define INVALID 0xFF

static void press2(uint8_t r1, uint8_t c1, uint8_t r2, uint8_t c2,
                   uint8_t pr[6], uint8_t pc[6]) {
    for (int i = 0; i < 6; i++) { pr[i] = INVALID; pc[i] = INVALID; }
    pr[0] = r1; pc[0] = c1; pr[1] = r2; pc[1] = c2;
}
static void press1(uint8_t r, uint8_t c, uint8_t pr[6], uint8_t pc[6]) {
    for (int i = 0; i < 6; i++) { pr[i] = INVALID; pc[i] = INVALID; }
    pr[0] = r; pc[0] = c;
}
static void press_none(uint8_t pr[6], uint8_t pc[6]) {
    for (int i = 0; i < 6; i++) { pr[i] = INVALID; pc[i] = INVALID; }
}

static void cb_reset(void) { combo_init(); host_clock_reset(); }

/* 1. Combo fires: 2 keys deferred + present -> result + suppression. */
static void test_cb_fires(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    TEST_ASSERT(combo_should_defer(0, 0), "member key -> should_defer");
    combo_defer_key(0, 0, 0x0A);
    combo_defer_key(0, 1, 0x0B);
    uint8_t pr[6], pc[6]; press2(0, 0, 0, 1, pr, pc);
    TEST_ASSERT_EQ(combo_process(pr, pc), 1, "both present+deferred -> 1 combo resolved");
    uint8_t r1, c1, r2, c2;
    TEST_ASSERT_EQ(combo_consume(&r1, &c1, &r2, &c2), 0x29, "consume -> result ESC");
    TEST_ASSERT(combo_is_suppressed(0, 0), "key of the active combo -> suppressed");
}

/* 2. should_defer: member yes / outside combo no / unconfigured slot no. */
static void test_cb_should_defer(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 1, .col1 = 2, .row2 = 1, .col2 = 3, .result = 0x28 };
    combo_set(0, &cfg);
    TEST_ASSERT(combo_should_defer(1, 2), "combo member -> defer");
    TEST_ASSERT(!combo_should_defer(5, 5), "key outside combo -> no defer");
    combo_config_t zero = { 0 };
    combo_set(1, &zero);
    TEST_ASSERT(!combo_should_defer(0, 0), "unconfigured slot (result=0) -> no defer");
}

/* 3. Only one key present -> no resolution. */
static void test_cb_one_key_no_fire(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    combo_defer_key(0, 0, 0x0A);
    uint8_t pr[6], pc[6]; press1(0, 0, pr, pc);
    TEST_ASSERT_EQ(combo_process(pr, pc), 0, "only one key -> 0 combo");
    uint8_t d;
    TEST_ASSERT_EQ(combo_consume(&d, &d, &d, &d), 0, "nothing to consume");
}

/* 4. Timeout exceeded, key still pressed alone -> comes back out as 'expired'. */
static void test_cb_timeout_expired(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    combo_defer_key(0, 0, 0x0A);       /* press_time = 0 */
    host_clock_advance_ms(51);         /* > COMBO_TIMEOUT_MS (50) */
    uint8_t pr[6], pc[6]; press1(0, 0, pr, pc);
    combo_process(pr, pc);
    TEST_ASSERT_EQ(combo_consume_expired(), 0x0A, "timeout -> deferred key comes back out as-is");
}

/* 5. The partner arrives within the window -> combo resolved. */
static void test_cb_partner_in_time(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    combo_defer_key(0, 0, 0x0A);       /* t = 0 */
    host_clock_advance_ms(30);         /* < 50ms */
    combo_defer_key(0, 1, 0x0B);
    uint8_t pr[6], pc[6]; press2(0, 0, 0, 1, pr, pc);
    TEST_ASSERT_EQ(combo_process(pr, pc), 1, "partner in time -> combo resolved");
    uint8_t d;
    TEST_ASSERT_EQ(combo_consume(&d, &d, &d, &d), 0x29, "result ESC");
}

/* 6. Deferred key released (absent from the report) -> comes back out, no combo. */
static void test_cb_released_expires(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    combo_defer_key(0, 0, 0x0A);
    uint8_t pr[6], pc[6]; press_none(pr, pc);   /* key released before the partner */
    combo_process(pr, pc);
    TEST_ASSERT_EQ(combo_consume_expired(), 0x0A, "released before partner -> comes back out");
}

/* Slot exhaustion (6KRO): unresolved combo -> keys NOT suppressed (M3).
 * The old code set combo_active=true before checking the defer -> both
 * keys of a non-deferrable combo (full slots) were suppressed = loss. */
static void test_cb_slot_exhaustion_no_suppress(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    /* Saturates the 4 deferred slots with other keys */
    combo_defer_key(2, 0, 0xA0);
    combo_defer_key(2, 1, 0xA1);
    combo_defer_key(2, 2, 0xA2);
    combo_defer_key(2, 3, 0xA3);
    /* Both keys of the combo can no longer be deferred (alloc fails) */
    combo_defer_key(0, 0, 0x0A);
    combo_defer_key(0, 1, 0x0B);
    uint8_t pr[6], pc[6]; press2(0, 0, 0, 1, pr, pc);
    combo_process(pr, pc);
    TEST_ASSERT(!combo_is_suppressed(0, 0),
                "full slots -> unresolved combo -> (0,0) NOT suppressed (no loss)");
    TEST_ASSERT(!combo_is_suppressed(0, 1),
                "full slots -> unresolved combo -> (0,1) NOT suppressed");
}

void test_combo(void) {
    TEST_SUITE("Combos — real module");
    TEST_RUN(test_cb_fires);
    TEST_RUN(test_cb_slot_exhaustion_no_suppress);
    TEST_RUN(test_cb_should_defer);
    TEST_RUN(test_cb_one_key_no_fire);
    TEST_RUN(test_cb_timeout_expired);
    TEST_RUN(test_cb_partner_in_time);
    TEST_RUN(test_cb_released_expires);
    combo_init();   /* leaves the module clean */
}
