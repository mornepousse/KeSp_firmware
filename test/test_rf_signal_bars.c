/*
 * test_rf_signal_bars.c — Host tests for rf_signal_bars().
 *
 * Validates all threshold boundaries from
 * docs/superpowers/specs/2026-05-23-finer-signal-gauge-arc-cnt-design.md (normative).
 *
 * link_q is a retry PERCENTAGE (0..100): 0 = pristine (no retransmits),
 * 100 = every packet exhausting all 3 ARC retries. Derived on the half from
 * OBSERVE_TX ARC_CNT (Σ retries × 100 / (tx_count × 3)).
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
    TEST_ASSERT_EQ(rf_signal_bars(true, 1500, 0),  0, "age=1500 boundary to 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 2000, 0),  0, "age=2000 to 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1500, 50), 0, "age=1500 q=50 to 0");

    /* ── retry_score boundaries (age<200 → age_score=4, isolates retry_score) ──
     * Thresholds: 0→4, ≤15→3, ≤33→2, ≤60→1, >60→0 (link_q = retry %). */
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 0),   4, "q=0%   to 4 (pristine)");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 1),   3, "q=1%   to 3");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 15),  3, "q=15%  boundary to 3");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 16),  2, "q=16%  to 2");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 33),  2, "q=33%  boundary to 2");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 34),  1, "q=34%  to 1");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 60),  1, "q=60%  boundary to 1");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 61),  0, "q=61%  to 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 100), 0, "q=100% to 0 (saturated)");

    /* ── age 200..399, q == 0 → min(3,4) = 3 ───────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 200, 0), 3, "age=200 q=0 to 3");
    TEST_ASSERT_EQ(rf_signal_bars(true, 300, 0), 3, "age=300 q=0 to 3");
    TEST_ASSERT_EQ(rf_signal_bars(true, 399, 0), 3, "age=399 q=0 to 3");

    /* ── age 400..699, q == 0 → min(2,4) = 2 ───────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 400, 0), 2, "age=400 q=0 to 2");
    TEST_ASSERT_EQ(rf_signal_bars(true, 500, 0), 2, "age=500 q=0 to 2");
    TEST_ASSERT_EQ(rf_signal_bars(true, 699, 0), 2, "age=699 q=0 to 2");

    /* ── age 400..699, q=4% (retry_score=3) → min(2,3) = 2 ──────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 500, 4), 2, "age=500 q=4% to 2");

    /* ── age 700..1199, q == 0 → min(1,4) = 1 ──────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true,  700, 0), 1, "age=700 q=0 to 1");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1000, 0), 1, "age=1000 q=0 to 1");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1199, 0), 1, "age=1199 q=0 to 1");

    /* ── age 1200..1499: age_score=0 (else branch), link still up → min(0,*)=0 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 1200, 0), 0, "age=1200 age_score=0 to 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1499, 0), 0, "age=1499 age_score=0 to 0");

    /* ── Minimum rule: both dimensions must be good ─────────────── */
    /* age=300 (age_score=3), q=25% (retry_score=2) → min(3,2)=2 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 300, 25), 2, "age=300 q=25% min(3,2)=2");
    /* age=400 (age_score=2), q=0 (retry_score=4) → min(2,4)=2 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 400, 0), 2, "age=400 q=0 min(2,4)=2");
    /* age=200 (age_score=3), q=50% (retry_score=1) → min(3,1)=1 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 200, 50), 1, "age=200 q=50% min(3,1)=1");
}
