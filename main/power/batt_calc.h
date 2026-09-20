#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Battery gauge — pure logic, tested on host (test/test_batt_calc.c).
 *
 * Hardware (docs/NIPHARGUS_V2_HARDWARE.md): VBAT_SENSE = 1 MOhm / 1 MOhm
 * divider + 100 nF on ADC2_CH2 -> V_batt = 2 x V_adc. Transport unit: the dV
 * of the batt_dV field of PKT_TYPE_STATUS (42 = 4.2 V), 0 = unknown.
 *
 * Rejection: outside [2.5 V; 4.5 V] = sensor absent, open divider, ADC
 * error — we say "unknown" rather than a wrong figure. A Li-ion 16340 lives
 * between ~3.0 V (DW01A cutoff) and 4.2 V.
 *
 * "Charging" is NOT measurable: the VBUS divider (GPIO33) is not populated
 * and the TP4056 has no STDBY wired. It is DEDUCED from voltage alone:
 *   - FULL: plateau >= 4.15 V held >= 2 min (TP4056 end-of-charge), kept
 *     with hysteresis;
 *   - PROBABLY CHARGING: rise >= 0.1 V within a 5-min window — a
 *     discharge never rises; a slow drift (temperature, noise) does not
 *     cross the threshold within the window.
 * Design: docs/superpowers/specs/2026-09-14-batterie-jauge-design.md */

#define BATT_DIVIDER_NUM     2u
#define BATT_MIN_MV          2500u
#define BATT_MAX_MV          4500u

#define BATT_FULL_MV         4150u
#define BATT_FULL_HOLD_MS    120000u
#define BATT_HYST_MV         50u
#define BATT_RISE_MV         100u
#define BATT_RISE_WINDOW_MS  300000u

static inline uint32_t batt_mv_from_adc(uint32_t mv_adc) { return mv_adc * BATT_DIVIDER_NUM; }

static inline bool batt_mv_plausible(uint32_t mv_batt)
{
    return mv_batt >= BATT_MIN_MV && mv_batt <= BATT_MAX_MV;
}

/* Battery mV -> rounded dV; 0 if out of range. */
static inline uint8_t batt_mv_to_dv_batt(uint32_t mv_batt)
{
    return batt_mv_plausible(mv_batt) ? (uint8_t)((mv_batt + 50u) / 100u) : 0u;
}

/* mV read at the ADC (divider side) -> battery dV; 0 if out of range. */
static inline uint8_t batt_mv_to_dv(uint32_t mv_adc)
{
    return batt_mv_to_dv_batt(batt_mv_from_adc(mv_adc));
}

/* Average of n ADC readings (mV, divider side) -> battery mV; 0 if n == 0 or out of range. */
static inline uint32_t batt_mv_from_samples(const uint32_t *mv_adc, unsigned n)
{
    if (n == 0) return 0;
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; i++) sum += mv_adc[i];
    uint32_t mv = batt_mv_from_adc((uint32_t)(sum / n));
    return batt_mv_plausible(mv) ? mv : 0;
}

static inline uint8_t batt_dv_from_samples(const uint32_t *mv_adc, unsigned n)
{
    return batt_mv_to_dv_batt(batt_mv_from_samples(mv_adc, n));
}

/* Approximate SoC of a resting Li-ion 16340, piecewise:
 * 3.3 V -> 0%, 3.5 -> 15, 3.7 -> 40, 3.9 -> 70, 4.2 -> 100. 0xFF if unknown.
 * A comfort gauge, not a coulomb counter: load and temperature shift
 * it — that is accepted and documented on the CDC side. */
static inline uint8_t batt_soc_pct(uint8_t dv)
{
    if (dv == 0) return 0xFF;
    static const struct { uint8_t dv, pct; } t[] =
        { {33, 0}, {35, 15}, {37, 40}, {39, 70}, {42, 100} };
    const unsigned n = sizeof t / sizeof t[0];
    if (dv <= t[0].dv) return 0;
    for (unsigned i = 1; i < n; i++) {
        if (dv <= t[i].dv) {
            uint32_t span = t[i].dv - t[i - 1].dv;
            uint32_t off  = dv - t[i - 1].dv;
            return (uint8_t)(t[i - 1].pct + (uint32_t)(t[i].pct - t[i - 1].pct) * off / span);
        }
    }
    return 100;
}

typedef enum {
    BATT_CHG_UNKNOWN  = 0,   /* discharging, or nothing deducible */
    BATT_CHG_PROBABLE = 1,   /* voltage is rising: a charge is in progress */
    BATT_CHG_FULL     = 2,   /* end-of-charge plateau held */
} batt_chg_t;

typedef struct {
    uint32_t   ref_mv;            /* bottom of the rise window */
    uint32_t   ref_ms;
    bool       ref_valid;
    uint32_t   plateau_since_ms;  /* start of the high plateau */
    bool       plateau;
    batt_chg_t etat;
} batt_state_t;

/* A measurement (battery mV, 0 = unknown) at instant now_ms -> deduced state.
 * An unknown measurement resets everything to zero: a deduction is never
 * extended over a missing reading. */
static inline batt_chg_t batt_state_step(batt_state_t *s, uint32_t mv, uint32_t now_ms)
{
    if (mv == 0) {
        *s = (batt_state_t){0};
        return BATT_CHG_UNKNOWN;
    }

    /* High plateau -> FULL, with hysteresis on exit. */
    if (mv >= BATT_FULL_MV) {
        if (!s->plateau) { s->plateau = true; s->plateau_since_ms = now_ms; }
        if ((uint32_t)(now_ms - s->plateau_since_ms) >= BATT_FULL_HOLD_MS)
            s->etat = BATT_CHG_FULL;
    } else if (mv < BATT_FULL_MV - BATT_HYST_MV) {
        s->plateau = false;
        if (s->etat == BATT_CHG_FULL) s->etat = BATT_CHG_UNKNOWN;
    }
    if (s->etat == BATT_CHG_FULL) return s->etat;

    /* Rise within the window -> PROBABLY CHARGING. The reference is the low
     * point: it resets on every drop (a discharge can never accumulate)
     * and when the window expires (a slow drift does not accumulate either). */
    if (!s->ref_valid || mv < s->ref_mv ||
        (uint32_t)(now_ms - s->ref_ms) > BATT_RISE_WINDOW_MS) {
        if (s->ref_valid && mv < s->ref_mv && s->etat == BATT_CHG_PROBABLE)
            s->etat = BATT_CHG_UNKNOWN;          /* it's dropping: the charge has stopped */
        s->ref_mv = mv; s->ref_ms = now_ms; s->ref_valid = true;
    } else if (mv >= s->ref_mv + BATT_RISE_MV) {
        s->etat = BATT_CHG_PROBABLE;
    }
    return s->etat;
}

/* -- Battery level: NORMAL / LOW / CRITICAL (2026-09-19) --------------------
 *
 * LOW below BATT_FAIBLE_DV (3.5 V ~= 15% of the cell): inverted gauge on
 * screen, the TRRS's 5 V is refused (we don't charge the other half with a
 * flat cell). CRITICAL below BATT_CRITIQUE_DV (3.3 V): additionally, light
 * sleep at 5 s instead of 15. No forced shutdown: the DW01A cuts off at
 * 2.5 V, that is its job. Hysteresis BATT_NIVEAU_HYST_DV (0.1 V) on the way
 * back up: a keystroke drops 30-50 mV on a tired cell, without it the gauge
 * would flicker. dv = 0 (no valid measurement): level KEPT — a
 * rejected sample teaches nothing, and a gauge silent since boot
 * stays NORMAL (we don't throttle on it). Tested on host (test_batt_calc). */
#define BATT_FAIBLE_DV        35u
#define BATT_CRITIQUE_DV      33u
#define BATT_NIVEAU_HYST_DV   1u

typedef enum { BATT_NORMAL = 0, BATT_FAIBLE = 1, BATT_CRITIQUE = 2 } batt_niveau_t;

static inline batt_niveau_t batt_niveau_step(batt_niveau_t courant, uint8_t dv)
{
    if (dv == 0) return courant;   /* rejected: a forced NORMAL made LOW->normal->LOW for the duration of one measurement */
    /* Going down: strict thresholds. */
    if (dv < BATT_CRITIQUE_DV) return BATT_CRITIQUE;
    if (dv < BATT_FAIBLE_DV && courant != BATT_CRITIQUE) return BATT_FAIBLE;
    /* Going back up: the hysteresis threshold must be exceeded. */
    if (courant == BATT_CRITIQUE) return (dv >= BATT_CRITIQUE_DV + BATT_NIVEAU_HYST_DV) ? BATT_FAIBLE : BATT_CRITIQUE;
    if (courant == BATT_FAIBLE)   return (dv >= BATT_FAIBLE_DV + BATT_NIVEAU_HYST_DV) ? BATT_NORMAL : BATT_FAIBLE;
    return BATT_NORMAL;
}
