/* Dongle engine wake — pure decision (main/comm/rf/engine_wake.h).
 *
 * The engine task used to sleep a flat 10 ms between rounds: a fused state
 * pushed by the rx task waited 0-10 ms before being played. Now a new state
 * wakes the task at once (task notification) and the 10 ms timer round
 * (tap-hold, tap-dance, leader, release on silence) keeps its own deadline:
 * the task blocks for exactly what is left until that deadline, so frames
 * never delay the timers and timers never delay a frame. */
#include "test_framework.h"
#include "../main/comm/rf/engine_wake.h"

static void test_engine_wait_until_next_timer_round(void)
{
    TEST_ASSERT_EQ(engine_wait_ms(1000, 1000, 10), 10, "just ran: a full period to wait");
    TEST_ASSERT_EQ(engine_wait_ms(1000, 1003, 10), 7, "a frame woke us 3 ms in: 7 ms left");
    TEST_ASSERT_EQ(engine_wait_ms(1000, 1009, 10), 1, "1 ms left");
    TEST_ASSERT_EQ(engine_wait_ms(1000, 1010, 10), 0, "deadline reached: no wait");
    TEST_ASSERT_EQ(engine_wait_ms(1000, 1250, 10), 0, "engine blocked 250 ms (macro): no wait, no negative");
}

static void test_engine_timer_due_only_at_deadline(void)
{
    TEST_ASSERT(!engine_timer_due(1000, 1000, 10), "not due right after a round");
    TEST_ASSERT(!engine_timer_due(1000, 1009, 10), "a frame wake at 9 ms is not a timer round");
    TEST_ASSERT(engine_timer_due(1000, 1010, 10), "due at the deadline");
    TEST_ASSERT(engine_timer_due(1000, 1400, 10), "due when late");
    /* Never a busy loop: whenever the task is not due it has something to wait. */
    for (uint32_t d = 0; d < 30; d++)
        TEST_ASSERT(engine_timer_due(500, 500 + d, 10) == (engine_wait_ms(500, 500 + d, 10) == 0),
                    "due <=> nothing left to wait");
}

static void test_engine_wake_wraps_the_ms_clock(void)
{
    /* now_ms() is esp_timer/1000 truncated to 32 bits: wraps after 49.7 days. */
    uint32_t last = 0xFFFFFFFCu;   /* 4 ms before the wrap */
    TEST_ASSERT_EQ(engine_wait_ms(last, 2u, 10), 4, "6 ms elapsed across the wrap: 4 left");
    TEST_ASSERT(!engine_timer_due(last, 2u, 10), "not due across the wrap");
    TEST_ASSERT(engine_timer_due(last, 6u, 10), "due across the wrap");
    TEST_ASSERT_EQ(engine_wait_ms(last, 6u, 10), 0, "nothing to wait across the wrap");
}

void test_engine_wake(void)
{
    printf("\n[engine_wake]\n");
    test_engine_wait_until_next_timer_round();
    test_engine_timer_due_only_at_deadline();
    test_engine_wake_wraps_the_ms_clock();
}
