/*
 * test_rf_signal_q255.c — Host tests for rf_signal_q255().
 *
 * Validates the 0..255 link-quality mapping from
 * docs/superpowers/specs/2026-05-23-signal-raw-255-display-design.md (normative).
 *
 * q255 = min(age_factor, retry_factor), 255 = best, 0 = link down / timed out.
 *   age_factor   = 255 * (1500 - hb_age_ms) / 1500   (fresh→255, ≥1500ms→0)
 *   retry_factor = 255 * (100 - link_q%)   / 100      (0%→255, 100%→0)
 * link_q is the retransmit percentage (0..100) from OBSERVE_TX ARC_CNT.
 */
#include "test_framework.h"
#include "../main/comm/rf/rf_rx_task.h"
#include <stdbool.h>
#include <stdint.h>

void test_rf_signal_q255(void)
{
    TEST_SUITE("rf_signal_q255");

    /* ── Link down → always 0 ──────────────────────────────────── */
    TEST_ASSERT_EQ(rf_signal_q255(false,    0, 0), 0, "link_down age=0 q=0 to 0");
    TEST_ASSERT_EQ(rf_signal_q255(false,  100, 0), 0, "link_down perfect age to 0");

    /* ── Age timeout (≥1500) → always 0 ─────────────────────────── */
    TEST_ASSERT_EQ(rf_signal_q255(true, 1500, 0),  0, "age=1500 boundary to 0");
    TEST_ASSERT_EQ(rf_signal_q255(true, 2000, 0),  0, "age=2000 to 0");
    TEST_ASSERT_EQ(rf_signal_q255(true, 1500, 50), 0, "age=1500 q=50 to 0");

    /* ── Perfect: age=0, q=0 → min(255,255) = 255 ──────────────── */
    TEST_ASSERT_EQ(rf_signal_q255(true, 0, 0), 255, "age=0 q=0 to 255");

    /* ── Fresh link, no retries (healthy steady state) ─────────── */
    TEST_ASSERT_EQ(rf_signal_q255(true, 100, 0), 238, "age=100 q=0 to 238 (healthy)");

    /* ── Age dimension alone (q=0) ─────────────────────────────── */
    TEST_ASSERT_EQ(rf_signal_q255(true,  750, 0), 127, "age=750 q=0 to 127 (half age)");
    TEST_ASSERT_EQ(rf_signal_q255(true, 1200, 0),  51, "age=1200 q=0 to 51");

    /* ── Retry dimension alone (age=0) ─────────────────────────── */
    TEST_ASSERT_EQ(rf_signal_q255(true, 0,  50), 127, "age=0 q=50% to 127 (half retries)");
    TEST_ASSERT_EQ(rf_signal_q255(true, 0,  75),  63, "age=0 q=75% to 63");
    TEST_ASSERT_EQ(rf_signal_q255(true, 0, 100),   0, "age=0 q=100% to 0 (saturated)");

    /* ── link_q clamp: >100 treated as 100 → 0 ─────────────────── */
    TEST_ASSERT_EQ(rf_signal_q255(true, 0, 200), 0, "age=0 q=200 clamped to 0");

    /* ── Minimum rule: both dimensions matter ──────────────────── */
    /* age=750 (127), q=50% (127) → min=127 */
    TEST_ASSERT_EQ(rf_signal_q255(true, 750, 50), 127, "age=750 q=50% min(127,127)=127");
    /* age=100 (238), q=75% (63) → min=63 */
    TEST_ASSERT_EQ(rf_signal_q255(true, 100, 75), 63, "age=100 q=75% min(238,63)=63");
}
