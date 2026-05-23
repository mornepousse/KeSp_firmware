/*
 * test_rf_signal_bars.c — Host tests for rf_signal_bars().
 *
 * Validates all threshold boundaries from spec §4.2 (normative).
 */
#include "test_framework.h"
#include "../main/comm/rf/rf_rx_task.h"
#include <stdbool.h>
#include <stdint.h>

void test_rf_signal_bars(void)
{
    TEST_SUITE("rf_signal_bars");

    /* ── Link down → always 0 ──────────────────────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(false,    0, 0), 0, "link_down age=0 q=0 to 0");
    TEST_ASSERT_EQ(rf_signal_bars(false, 5000, 0), 0, "link_down age=5000 to 0");
    TEST_ASSERT_EQ(rf_signal_bars(false,  100, 0), 0, "link_down perfect age to 0");

    /* ── Age timeout → always 0 ─────────────────────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 1500, 0), 0, "age=1500 boundary to 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 2000, 0), 0, "age=2000 to 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1500, 5), 0, "age=1500 q=5 to 0");

    /* ── Excellent: age < 200, q == 0 → min(4,4) = 4 ───────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true,   0, 0), 4, "age=0 q=0 to 4");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 0), 4, "age=100 q=0 to 4");
    TEST_ASSERT_EQ(rf_signal_bars(true, 199, 0), 4, "age=199 q=0 to 4");

    /* ── Good: age < 200, q 1..2 → min(4,3) = 3 ────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 1), 3, "age=100 q=1 to 3");
    TEST_ASSERT_EQ(rf_signal_bars(true, 150, 2), 3, "age=150 q=2 to 3");

    /* ── age < 200, q 3..5 → min(4,2) = 2 ──────────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 3), 2, "age=100 q=3 to 2");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 5), 2, "age=100 q=5 to 2");

    /* ── age < 200, q 6..10 → min(4,1) = 1 ─────────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 6),  1, "age=100 q=6 to 1");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 10), 1, "age=100 q=10 to 1");

    /* ── age < 200, q > 10 → min(4,0) = 0 ──────────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 11),  0, "age=100 q=11 to 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 15),  0, "age=100 q=15 to 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 255), 0, "age=100 q=255 to 0");

    /* ── age 200..399, q == 0 → min(3,4) = 3 ───────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 200, 0), 3, "age=200 q=0 to 3");
    TEST_ASSERT_EQ(rf_signal_bars(true, 300, 0), 3, "age=300 q=0 to 3");
    TEST_ASSERT_EQ(rf_signal_bars(true, 399, 0), 3, "age=399 q=0 to 3");

    /* ── age 400..699, q == 0 → min(2,4) = 2 ───────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 400, 0), 2, "age=400 q=0 to 2");
    TEST_ASSERT_EQ(rf_signal_bars(true, 500, 0), 2, "age=500 q=0 to 2");
    TEST_ASSERT_EQ(rf_signal_bars(true, 699, 0), 2, "age=699 q=0 to 2");

    /* ── age 400..699, q 3..5 → min(2,2) = 2 ───────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 500, 4), 2, "age=500 q=4 to 2");

    /* ── age 700..1199, q == 0 → min(1,4) = 1 ──────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true,  700, 0), 1, "age=700 q=0 to 1");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1000, 0), 1, "age=1000 q=0 to 1");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1199, 0), 1, "age=1199 q=0 to 1");

    /* ── age 1200..1499: age_score=0 (else branch), link still up → min(0,*)=0 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 1200, 0), 0, "age=1200 age_score=0 to 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1499, 0), 0, "age=1499 age_score=0 to 0");

    /* ── Minimum rule: both must be good ────────────────────────── */
    /* age=300 (age_score=3), q=5 (retry_score=2) → min=2 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 300, 5), 2, "age=300 q=5 min(3,2)=2");
    /* age=400 (age_score=2), q=0 (retry_score=4) → min=2 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 400, 0), 2, "age=400 q=0 min(2,4)=2");
    /* age=200 (age_score=3), q=10 (retry_score=1) → min=1 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 200, 10), 1, "age=200 q=10 min(3,1)=1");
}
