/* Leader Key engine tests — the REAL linked module (../main/input/leader.c).
 * Drives the real internal matcher (try_match, notoriously convoluted) via the API:
 * leader_set -> leader_start -> leader_feed -> (tick) -> leader_consume.
 * host_clock for the LEADER_TIMEOUT_MS window. If the real matcher diverged
 * from the intent, one of these tests would go red (= a real prod bug to report). */
#include "test_framework.h"
#include "leader.h"
#include "host_clock.h"

static void ld_reset(void) { leader_init(); host_clock_reset(); }

static void set_entry(uint8_t idx, uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3,
                      uint8_t result, uint8_t mod) {
    leader_entry_t e = { .sequence = { s0, s1, s2, s3 }, .result = result, .result_mod = mod };
    leader_set(idx, &e);
}

/* 1. 1-key sequence [A] -> ESC. */
static void test_ld_single(void) {
    ld_reset();
    set_entry(0, 0x04, 0, 0, 0, 0x29, 0);
    leader_start();
    TEST_ASSERT(leader_feed(0x04), "feed A absorbed");
    uint8_t mod = 0xFF;
    TEST_ASSERT_EQ(leader_consume(&mod), 0x29, "[A] -> ESC");
    TEST_ASSERT_EQ(mod, 0, "no modifier");
}

/* 2. 2-key sequence [F,S] -> Ctrl+S. */
static void test_ld_two_key(void) {
    ld_reset();
    set_entry(0, 0x09, 0x16, 0, 0, 0x16, 0x01);
    leader_start();
    leader_feed(0x09);
    leader_feed(0x16);
    uint8_t mod = 0;
    TEST_ASSERT_EQ(leader_consume(&mod), 0x16, "[F,S] -> S");
    TEST_ASSERT_EQ(mod, 0x01, "modifier = Ctrl");
}

/* 3. Wrong key -> no match. */
static void test_ld_wrong_key(void) {
    ld_reset();
    set_entry(0, 0x04, 0, 0, 0, 0x29, 0);
    leader_start();
    leader_feed(0x05);            /* B, not A */
    uint8_t mod;
    TEST_ASSERT_EQ(leader_consume(&mod), 0, "wrong key -> nothing");
}

/* 4. Partial sequence -> no match, leader stays active. */
static void test_ld_partial(void) {
    ld_reset();
    set_entry(0, 0x04, 0x05, 0, 0, 0x29, 0);   /* [A,B] */
    leader_start();
    leader_feed(0x04);            /* A only */
    uint8_t mod;
    TEST_ASSERT_EQ(leader_consume(&mod), 0, "partial -> no match");
    TEST_ASSERT(leader_is_active(), "still active while waiting for B");
}

/* 5. Unconfigured entry (result=0) ignored. */
static void test_ld_unconfigured(void) {
    ld_reset();
    set_entry(0, 0x04, 0, 0, 0, 0, 0);   /* result = 0 */
    leader_start();
    leader_feed(0x04);
    uint8_t mod;
    TEST_ASSERT_EQ(leader_consume(&mod), 0, "result=0 -> entry ignored");
}

/* 6. Several entries: the right one matches based on length + content. */
static void test_ld_multiple(void) {
    ld_reset();
    set_entry(0, 0x04, 0, 0, 0, 0x29, 0);       /* [A]   -> ESC */
    set_entry(1, 0x05, 0, 0, 0, 0x28, 0);       /* [B]   -> Enter */
    set_entry(2, 0x04, 0x05, 0, 0, 0x2A, 0);    /* [A,B] -> Backspace */
    leader_start();
    leader_feed(0x05);                           /* B */
    uint8_t mod;
    TEST_ASSERT_EQ(leader_consume(&mod), 0x28, "[B] -> Enter");
    leader_start();
    leader_feed(0x04);
    leader_feed(0x05);
    TEST_ASSERT_EQ(leader_consume(&mod), 0x2A, "[A,B] -> Backspace");
}

/* 7. Full sequence (4 keys) -> match (exercises the sequence[j+1] path). */
static void test_ld_four_key(void) {
    ld_reset();
    set_entry(0, 0x04, 0x05, 0x06, 0x07, 0x29, 0x02);
    leader_start();
    leader_feed(0x04); leader_feed(0x05); leader_feed(0x06); leader_feed(0x07);
    uint8_t mod = 0;
    TEST_ASSERT_EQ(leader_consume(&mod), 0x29, "[A,B,C,D] -> ESC");
    TEST_ASSERT_EQ(mod, 0x02, "modifier = Shift");
}

/* 8. Timeout: uncompleted partial sequence -> tick cancels after the delay. */
static void test_ld_timeout_cancels(void) {
    ld_reset();
    set_entry(0, 0x04, 0x05, 0, 0, 0x29, 0);   /* [A,B] */
    leader_start();
    leader_feed(0x04);                           /* partial */
    host_clock_advance_ms(1000);                 /* == LEADER_TIMEOUT_MS */
    TEST_ASSERT(!leader_tick(), "timeout without full match -> no resolution");
    TEST_ASSERT(!leader_is_active(), "timeout -> leader disabled");
}

void test_leader(void) {
    TEST_SUITE("Leader Key — real module");
    TEST_RUN(test_ld_single);
    TEST_RUN(test_ld_two_key);
    TEST_RUN(test_ld_wrong_key);
    TEST_RUN(test_ld_partial);
    TEST_RUN(test_ld_unconfigured);
    TEST_RUN(test_ld_multiple);
    TEST_RUN(test_ld_four_key);
    TEST_RUN(test_ld_timeout_cancels);
    leader_init();   /* leaves the module clean */
}
