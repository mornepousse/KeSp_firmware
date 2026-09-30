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
    /* The dV entry point (the dongle's, from the STATUS batt_dV) reads the
     * SAME curve as the halves' mV one: one curve, two resolutions. */
    TEST_ASSERT_EQ(batt_soc_pct(0), 0xFF, "unknown -> 0xFF");
    TEST_ASSERT_EQ(batt_soc_pct(42), 100, "4.2 V -> 100 %");
    TEST_ASSERT_EQ(batt_soc_pct(30), 0,   "below the curve: 0, never negative");
    for (uint8_t d = 25; d <= 45; d++)
        TEST_ASSERT_EQ(batt_soc_pct(d), batt_soc_pct_mv((uint16_t)(d * 100u)), "dV reads the mV curve");
    for (uint8_t d = 30; d < 45; d++)
        TEST_ASSERT(batt_soc_pct(d + 1) >= batt_soc_pct(d), "SoC monotonic in voltage");
}

/* The mV curve: a generic single-cell LiPo resting-voltage table (see
 * batt_calc.h for the source) — bounded, monotonic, and fine enough in the
 * flat 3.7-3.9 V zone that a 30 mV change is visible. */
static void test_soc_mv_courbe(void)
{
    TEST_ASSERT_EQ(batt_soc_pct_mv(0), 0xFF, "0 mV = unknown -> 0xFF");
    TEST_ASSERT_EQ(batt_soc_pct_mv(4200), 100, "4.20 V -> 100 %");
    TEST_ASSERT_EQ(batt_soc_pct_mv(4400), 100, "above 4.2 V (charging): 100, never more");
    TEST_ASSERT_EQ(batt_soc_pct_mv(4110), 90,  "4.11 V -> 90 % (table point)");
    TEST_ASSERT_EQ(batt_soc_pct_mv(3840), 50,  "3.84 V -> 50 % (table point)");
    TEST_ASSERT_EQ(batt_soc_pct_mv(3690), 10,  "3.69 V -> 10 % (table point)");
    TEST_ASSERT_EQ(batt_soc_pct_mv(3270), 0,   "3.27 V -> 0 %");
    TEST_ASSERT_EQ(batt_soc_pct_mv(3000), 0,   "below the curve: 0");
    TEST_ASSERT_EQ(batt_soc_pct_mv(2500), 0,   "plausible floor: 0");
    TEST_ASSERT(batt_soc_pct_mv(3870) - batt_soc_pct_mv(3800) >= 15, "flat zone: 3.80 -> 3.87 V moves >= 15 points");
    TEST_ASSERT(batt_soc_pct_mv(4150) > 90 && batt_soc_pct_mv(4150) < 100, "4.15 V interpolated between 90 and 100");
    for (uint32_t mv = 2500; mv < 4500; mv += 5) {
        uint8_t a = batt_soc_pct_mv((uint16_t)mv), b = batt_soc_pct_mv((uint16_t)(mv + 5));
        TEST_ASSERT(b >= a && b <= 100, "mV curve monotonic and bounded");
    }
}

/* The filter: an EMA of the mV, 1/8 per 10 s sample (~80 s awake). First
 * sample taken as is; a rejected sample (0) teaches nothing. */
static void test_ema_mv(void)
{
    batt_ema_t e = {0};
    TEST_ASSERT_EQ(batt_ema_step(&e, 0), 0, "rejected sample before any valid one: unknown");
    TEST_ASSERT_EQ(batt_ema_step(&e, 3900), 3900, "first sample: taken as is (boot shows right away)");
    TEST_ASSERT_EQ(batt_ema_step(&e, 0), 3900, "rejected sample: the filter keeps its value");
    uint16_t f = batt_ema_step(&e, 3860);
    TEST_ASSERT(f >= 3894 && f <= 3896, "a -40 mV sag in one sample moves the filter by ~5 mV");
    for (int i = 0; i < 60; i++) f = batt_ema_step(&e, 3800);
    TEST_ASSERT(f >= 3799 && f <= 3801, "a real, lasting change is reached (10 min at 10 s)");
}

/* Drives measured mV through the whole display chain (EMA -> curve ->
 * displayed %), one sample every 10 s, as batt_sense does. */
typedef struct { batt_ema_t e; batt_affiche_t a; } chaine_t;
static uint8_t chaine(chaine_t *c, uint16_t mv, bool usb, uint32_t t)
{
    uint16_t f = batt_ema_step(&c->e, mv);
    return batt_affiche_step(&c->a, f ? batt_soc_pct_mv(f) : 0xFF, usb, t);
}

static void test_affiche_boot(void)
{
    batt_affiche_t a = {0};
    TEST_ASSERT_EQ(batt_affiche_step(&a, 0xFF, false, 0), 0xFF, "no reading yet: unknown");
    TEST_ASSERT_EQ(batt_affiche_step(&a, 87, false, 10), 85, "first reading: shown at once, in 5 % steps");
    TEST_ASSERT_EQ(batt_affiche_step(&a, 88, false, 20), 85, "same step: unchanged");
    batt_affiche_t b = {0};
    TEST_ASSERT_EQ(batt_affiche_step(&b, 100, false, 0), 100, "full cell at boot: 100");
    batt_affiche_t c = {0};
    TEST_ASSERT_EQ(batt_affiche_step(&c, 2, false, 0), 0, "2 % -> 0");
    TEST_ASSERT_EQ(batt_affiche_step(&c, 0xFF, false, 10), 0xFF, "unknown reading: ?");
}

/* The bench bug of 2026-09-30: 4.10 / 4.15 / 4.20 V readings, 90 / 100 /
 * FULL alternating. An hour of that noise on battery: at most one change,
 * never upward. */
static void test_affiche_bruit_haut_ne_bat_pas(void)
{
    static const uint16_t bruit[] = { 4200, 4100, 4150, 4200, 4100, 4200, 4150, 4100, 4200, 4150, 4200 };
    chaine_t c = {0};
    uint8_t prec = chaine(&c, 4200, false, 0), n_chg = 0;
    for (uint32_t i = 1; i < 360; i++) {
        uint8_t v = chaine(&c, bruit[i % (sizeof bruit / sizeof bruit[0])], false, i * 10000u);
        TEST_ASSERT(v <= prec, "noise at the top: the display never goes up on battery");
        if (v != prec) n_chg++;
        prec = v;
    }
    TEST_ASSERT(n_chg <= 1, "an hour of 4.10-4.20 V noise: one settle at most, no flapping");
    /* Even without the EMA (raw SoC 90/95/100): no flapping. */
    batt_affiche_t a = {0};
    static const uint8_t soc[] = { 100, 90, 95, 100, 90, 100 };
    uint8_t p = batt_affiche_step(&a, 100, false, 0);
    for (uint32_t i = 1; i < 360; i++) {
        uint8_t v = batt_affiche_step(&a, soc[i % 6], false, i * 10000u);
        TEST_ASSERT(v == p, "raw 90/95/100 every 10 s: never held 60 s below, never moves");
        p = v;
    }
}

/* A slow discharge (4.20 -> 3.30 V over 10 h): the display walks down in
 * 5 % steps, never up, and ends near 0. */
static void test_affiche_decharge_lente(void)
{
    chaine_t c = {0};
    uint8_t prec = chaine(&c, 4200, false, 0);
    TEST_ASSERT_EQ(prec, 100, "starts at 100");
    const uint32_t n = 3600;                    /* 10 h at 10 s */
    for (uint32_t i = 1; i <= n; i++) {
        uint16_t mv = (uint16_t)(4200 - (900u * i) / n);
        uint8_t v = chaine(&c, mv, false, i * 10000u);
        TEST_ASSERT(v <= prec, "slow discharge: never up");
        TEST_ASSERT(v == prec || prec - v == 5, "slow discharge: one 5 % step at a time");
        TEST_ASSERT(v % 5 == 0, "always a multiple of 5");
        prec = v;
    }
    TEST_ASSERT(prec <= 5, "10 h later, at 3.30 V: 0-5 %");
}

/* On battery the displayed value does not go back up: a load removed, a cell
 * warming, a sag recovered are not charge. */
static void test_affiche_jamais_monte_sur_batterie(void)
{
    batt_affiche_t a = {0};
    batt_affiche_step(&a, 50, false, 0);
    uint32_t t = 0;
    for (int i = 0; i < 30; i++) { t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&a, 64, false, t), 50, "+15 % held 5 min on battery: stays 50"); }
    for (int i = 0; i < 360; i++) { t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&a, 65, false, t), 50, "+15 % for an hour on battery: stays 50"); }
    /* A radio/keystroke sag: 50 s below, then back — no drop. */
    for (int i = 0; i < 5; i++) { t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&a, 40, false, t), 50, "sag shorter than 60 s: kept"); }
    t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&a, 50, false, t), 50, "back: no drop");
    for (int i = 0; i < 6; i++) { t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&a, 40, false, t), 50, "below for 0-50 s: kept"); }
    t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&a, 40, false, t), 40, "below for 60 s: the drop is shown");
}

/* A wall charger does not enumerate and the halves have no VBUS bridge: the
 * USB rule cannot see it. Escape hatch: >= 20 % above for 5 min on battery
 * is a charge nobody saw, the display re-anchors. */
static void test_affiche_recal_chargeur_mural(void)
{
    batt_affiche_t a = {0};
    batt_affiche_step(&a, 30, false, 0);
    uint32_t t = 0;
    for (int i = 0; i < 30; i++) { t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&a, 80, false, t), 30, "+50 % on battery, under 5 min: stays"); }
    t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&a, 80, false, t), 80, "+50 % held 5 min: re-anchored");
    batt_affiche_t b = {0};
    batt_affiche_step(&b, 30, false, 0);
    t = 0;
    for (int i = 0; i < 25; i++) { t += 10000; batt_affiche_step(&b, 80, false, t); }
    t += 10000; batt_affiche_step(&b, 45, false, t);   /* interrupted */
    for (int i = 0; i < 25; i++) { t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&b, 80, false, t), 30, "interrupted: the 5 min restart"); }
}

/* On USB the cell charges: the display follows up (60 s hold per move). */
static void test_affiche_charge_usb(void)
{
    batt_affiche_t a = {0};
    batt_affiche_step(&a, 50, true, 0);
    uint32_t t = 0;
    for (int i = 0; i < 6; i++) { t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&a, 60, true, t), 50, "USB, 60 % for < 60 s: not yet"); }
    t += 10000; TEST_ASSERT_EQ(batt_affiche_step(&a, 60, true, t), 60, "USB, 60 % held 60 s: up");
    uint8_t v = 60;
    for (int s = 60; s <= 100; s++) { t += 10000; v = batt_affiche_step(&a, (uint8_t)s, true, t); }
    TEST_ASSERT(v >= 90, "a charge on USB climbs toward 100");
    /* Unplugged: on battery again, the charge overvoltage falls away — down only. */
    uint8_t p = v;
    for (int i = 0; i < 30; i++) { t += 10000; uint8_t w = batt_affiche_step(&a, 100, false, t); TEST_ASSERT(w <= p, "unplugged: never up"); p = w; }
}

/* "+" and FULL only with USB power: on battery the rise heuristic (a
 * recovery after a load, a warm cell) and the 4.15 V plateau of a full cell
 * mean nothing. */
static void test_chg_affiche_exige_usb(void)
{
    for (int h = 0; h <= 2; h++)
        TEST_ASSERT_EQ(batt_chg_affiche((batt_chg_t)h, false), BATT_CHG_UNKNOWN, "no USB: never + nor FULL");
    TEST_ASSERT_EQ(batt_chg_affiche(BATT_CHG_FULL, true), BATT_CHG_FULL, "USB + plateau held: FULL");
    TEST_ASSERT_EQ(batt_chg_affiche(BATT_CHG_UNKNOWN, true), BATT_CHG_PROBABLE, "USB, not full yet: + (the charger is fed)");
    TEST_ASSERT_EQ(batt_chg_affiche(BATT_CHG_PROBABLE, true), BATT_CHG_PROBABLE, "USB, rising: +");
    /* The bench case: a full cell on battery, plateau held 2 min. */
    batt_state_t st = {0};
    batt_state_step(&st, 4180, 0);
    batt_chg_t h = batt_state_step(&st, 4180, 130000);
    TEST_ASSERT_EQ(h, BATT_CHG_FULL, "the heuristic alone says full...");
    TEST_ASSERT_EQ(batt_chg_affiche(h, false), BATT_CHG_UNKNOWN, "... but without USB nothing is shown");
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
    test_soc_mv_courbe();
    test_ema_mv();
    test_affiche_boot();
    test_affiche_bruit_haut_ne_bat_pas();
    test_affiche_decharge_lente();
    test_affiche_jamais_monte_sur_batterie();
    test_affiche_recal_chargeur_mural();
    test_affiche_charge_usb();
    test_chg_affiche_exige_usb();
    test_pleine_apres_plateau();
    test_en_charge_probable_si_ca_monte();
    test_lente_derive_ne_trompe_pas();
    test_inconnu_reset();
    test_niveau_batterie_avec_hysteresis();
}
