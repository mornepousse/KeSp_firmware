/* "Matrix changed" flag — F3 audit, lost edge.
 *
 * The flag connects a high-priority producer (matrix scan callback at
 * priority 5, or RF reception on the dongle) to a lower-priority
 * consumer (the keyboard task, priority 3). The producer can preempt the
 * consumer at any time.
 *
 * The bug these tests forbid: clearing the flag AFTER reading the state.
 * A producer that fires between the read and the clear has its signal
 * overwritten, and its edge disappears — the key never reaches the host.
 */
#include "test_framework.h"
#include "../main/input/matrix_flag.h"

static void test_idle_flag_has_nothing_to_take(void)
{
    volatile uint8_t f = 0;
    TEST_ASSERT(!matrix_flag_take(&f), "nothing to take on an idle flag");
    TEST_ASSERT(!matrix_flag_take(&f), "and still nothing on the second call");
}

static void test_take_consumes_the_signal(void)
{
    volatile uint8_t f = 0;
    matrix_flag_signal(&f);
    TEST_ASSERT(matrix_flag_take(&f), "the signal is taken");
    /* If it were not consumed, the consumer would keep reprocessing the same
     * state in a loop and the flag would never come back down. */
    TEST_ASSERT(!matrix_flag_take(&f), "the signal was indeed consumed");
}

/* THE F3 regression test. */
static void test_signal_during_the_read_survives(void)
{
    volatile uint8_t f = 0;

    matrix_flag_signal(&f);                          /* the scan detects an edge */
    TEST_ASSERT(matrix_flag_take(&f), "first edge taken");

    /* Here the consumer reads the matrix state — it takes a while, and the
     * scan callback (prio 5) preempts it. A new edge arrives DURING the read. */
    matrix_flag_signal(&f);

    /* With a clear-after-read, the consumer would overwrite this signal while
     * finishing its round, and the edge would be lost forever: nothing
     * re-sends it. By clearing first, it survives. */
    TEST_ASSERT(matrix_flag_take(&f),
                "the edge that arrived during the read is not lost");
}

static void test_bursts_collapse_to_one_take(void)
{
    volatile uint8_t f = 0;
    /* Several edges before the consumer runs again: they merge.
     * This is intentional — the consumer re-reads the full matrix state, not an
     * event log, so a single pass is enough to see everything. */
    for (int i = 0; i < 5; i++) matrix_flag_signal(&f);
    TEST_ASSERT(matrix_flag_take(&f), "the burst gives one pass");
    TEST_ASSERT(!matrix_flag_take(&f), "and only one");
}

void test_matrix_flag(void)
{
    printf("\n-- matrix flag (F3: lost edge) --\n");
    test_idle_flag_has_nothing_to_take();
    test_take_consumes_the_signal();
    test_signal_during_the_read_survives();
    test_bursts_collapse_to_one_take();
}
