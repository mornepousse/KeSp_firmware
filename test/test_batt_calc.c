/* Battery gauge — pure logic (calculations and deductions).
 *
 * Hardware: VBAT_SENSE = 1 MΩ/1 MΩ bridge + 100 nF → V_batt = 2 × V_adc.
 * Transport unit: dV (42 = 4.2 V), 0 = unknown (batt_dV convention).
 * "Charging" is NOT measurable (no VBUS, TP4056 without STDBY): we
 * DEDUCE — full = plateau ≥ 4.15 V held 2 min; charging probable = rise
 * ≥ 0.1 V (a discharge never rises). These deductions are what protect
 * the user from a gauge that lies; they are tested here.
 *
 * Design: docs/superpowers/specs/2026-09-14-batterie-jauge-design.md */
#include "test_framework.h"
#include "../main/power/batt_calc.h"

static void test_conversion_et_rejet(void)
{
    TEST_ASSERT_EQ(batt_mv_to_dv(2075), 42, "2075 mV ADC x 2 = 4.15 V -> 42 dV (rounded)");
    TEST_ASSERT_EQ(batt_mv_to_dv(1850), 37, "1850 x 2 = 3.70 V -> 37");
    TEST_ASSERT_EQ(batt_mv_to_dv(1200), 0,  "2.4 V battery: below 2.5 V -> unknown");
    TEST_ASSERT_EQ(batt_mv_to_dv(2300), 0,  "4.6 V battery: above 4.5 V -> unknown");
    TEST_ASSERT_EQ(batt_mv_to_dv(0), 0,     "ADC at 0 (open bridge) -> unknown");
}

static void test_moyenne(void)
{
    uint32_t s[8] = { 1850, 1852, 1848, 1851, 1849, 1850, 1850, 1850 };
    TEST_ASSERT_EQ(batt_mv_from_samples(s, 8), 3700, "average x 2 = 3700 mV");
    TEST_ASSERT_EQ(batt_dv_from_samples(s, 8), 37, "-> 37 dV");
    TEST_ASSERT_EQ(batt_mv_from_samples(s, 0), 0, "no sample -> unknown");
    uint32_t bad[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    TEST_ASSERT_EQ(batt_dv_from_samples(bad, 8), 0, "all zero -> unknown");
}

static void test_soc(void)
{
    TEST_ASSERT_EQ(batt_soc_pct(0), 0xFF, "unknown -> 0xFF");
    TEST_ASSERT_EQ(batt_soc_pct(42), 100, "4.2 V -> 100 %");
    TEST_ASSERT_EQ(batt_soc_pct(39), 70,  "3.9 V -> 70 %");
    TEST_ASSERT_EQ(batt_soc_pct(37), 40,  "3.7 V -> 40 %");
    TEST_ASSERT_EQ(batt_soc_pct(35), 15,  "3.5 V -> 15 %");
    TEST_ASSERT_EQ(batt_soc_pct(33), 0,   "3.3 V -> 0 %");
    TEST_ASSERT_EQ(batt_soc_pct(30), 0,   "below 3.3 V: 0, never negative");
    TEST_ASSERT(batt_soc_pct(38) > 40 && batt_soc_pct(38) < 70, "3.8 V interpolated between 40 and 70");
    for (uint8_t d = 30; d < 45; d++)
        TEST_ASSERT(batt_soc_pct(d + 1) >= batt_soc_pct(d), "SoC monotonic in voltage");
}

static void test_pleine_apres_plateau(void)
{
    batt_state_t st = {0};
    TEST_ASSERT_EQ(batt_state_step(&st, 4160, 0), BATT_CHG_UNKNOWN, "first high reading: not full yet");
    TEST_ASSERT_EQ(batt_state_step(&st, 4160, 60000), BATT_CHG_UNKNOWN, "60 s of plateau: not yet");
    TEST_ASSERT_EQ(batt_state_step(&st, 4165, 121000), BATT_CHG_FULL, ">= 120 s >= 4.15 V -> FULL");
    TEST_ASSERT_EQ(batt_state_step(&st, 4120, 130000), BATT_CHG_FULL, "hysteresis: 4.12 V stays full");
    TEST_ASSERT_EQ(batt_state_step(&st, 4090, 140000), BATT_CHG_UNKNOWN, "below 4.10 V: no longer full");
}

static void test_en_charge_probable_si_ca_monte(void)
{
    batt_state_t st = {0};
    batt_state_step(&st, 3700, 0);
    TEST_ASSERT_EQ(batt_state_step(&st, 3750, 60000), BATT_CHG_UNKNOWN, "+50 mV: not enough");
    TEST_ASSERT_EQ(batt_state_step(&st, 3810, 120000), BATT_CHG_PROBABLE, "+110 mV in 2 min -> probably charging");
    /* A discharge never "rises": unknown. */
    batt_state_t d = {0};
    batt_state_step(&d, 3900, 0);
    TEST_ASSERT_EQ(batt_state_step(&d, 3850, 100000), BATT_CHG_UNKNOWN, "it's dropping: unknown");
    TEST_ASSERT_EQ(batt_state_step(&d, 3800, 200000), BATT_CHG_UNKNOWN, "still dropping: still unknown");
}

static void test_lente_derive_ne_trompe_pas(void)
{
    /* +30 mV every 5 min for a long time: each step is below the threshold and
     * the sliding window resets — this is NOT a charge (noise, temperature). */
    batt_state_t st = {0};
    uint32_t mv = 3700;
    for (uint32_t t = 0; t <= 3000000; t += 300001) {
        mv += 30;
        TEST_ASSERT_EQ(batt_state_step(&st, mv, t), BATT_CHG_UNKNOWN, "slow drift: never \"charging\"");
    }
}

static void test_inconnu_reset(void)
{
    batt_state_t st = {0};
    batt_state_step(&st, 4160, 0);
    batt_state_step(&st, 4160, 130000);   /* full */
    TEST_ASSERT_EQ(batt_state_step(&st, 0, 140000), BATT_CHG_UNKNOWN, "unknown reading -> unknown state, plateau forgotten");
    TEST_ASSERT_EQ(batt_state_step(&st, 4160, 150000), BATT_CHG_UNKNOWN, "must redo the 120 s plateau");
}

static void test_niveau_batterie_avec_hysteresis(void)
{
    /* Thresholds decided on 2026-09-19: LOW below 3.5 V (cell ~15 %), CRITICAL
     * below 3.3 V; it only recovers with a 0.1 V margin (a keystroke makes
     * the voltage drop 30-50 mV on a tired cell). 0 (no reading) = NORMAL:
     * we don't throttle on a mute gauge. */
    batt_niveau_t n = BATT_NORMAL;
    n = batt_niveau_step(n, 39); TEST_ASSERT(n == BATT_NORMAL, "3.9 V: normal");
    n = batt_niveau_step(n, 35); TEST_ASSERT(n == BATT_NORMAL, "3.5 V: still normal (strict)");
    n = batt_niveau_step(n, 34); TEST_ASSERT(n == BATT_FAIBLE, "3.4 V: low");
    n = batt_niveau_step(n, 35); TEST_ASSERT(n == BATT_FAIBLE, "3.5 V: stays low (hysteresis)");
    n = batt_niveau_step(n, 36); TEST_ASSERT(n == BATT_NORMAL, "3.6 V: normal");
    n = batt_niveau_step(n, 32); TEST_ASSERT(n == BATT_CRITIQUE, "3.2 V: critical");
    n = batt_niveau_step(n, 33); TEST_ASSERT(n == BATT_CRITIQUE, "3.3 V: stays critical");
    n = batt_niveau_step(n, 34); TEST_ASSERT(n == BATT_FAIBLE, "3.4 V: low");
    n = batt_niveau_step(n, 0);  TEST_ASSERT(n == BATT_FAIBLE, "rejected sample: level kept (no LOW->normal->LOW)");
    TEST_ASSERT(batt_niveau_step(BATT_CRITIQUE, 0) == BATT_CRITIQUE, "rejected sample while critical: stays critical");
    TEST_ASSERT(batt_niveau_step(BATT_NORMAL, 0) == BATT_NORMAL, "gauge mute since boot: normal");
    n = batt_niveau_step(BATT_NORMAL, 31); TEST_ASSERT(n == BATT_CRITIQUE, "direct drop: critical in one step");
}

void test_batt_calc(void)
{
    TEST_SUITE("Battery gauge: pure calculations");
    test_conversion_et_rejet();
    test_moyenne();
    test_soc();
    test_pleine_apres_plateau();
    test_en_charge_probable_si_ca_monte();
    test_lente_derive_ne_trompe_pas();
    test_inconnu_reset();
    test_niveau_batterie_avec_hysteresis();
}
