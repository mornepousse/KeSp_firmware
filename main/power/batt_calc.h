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
 * and the TP4056 has no STDBY wired. What is SHOWN or SENT is gated by USB
 * power (batt_chg_affiche, below); the deduction itself is from voltage alone:
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

/* State of charge from the battery mV, 0xFF if unknown (0 mV).
 *
 * The curve is a GENERIC single-cell LiPo resting-voltage (open-circuit)
 * chart, the one RC-hobby and battery vendors publish at 5-10 % steps (e.g.
 * "LiPo voltage chart", ampow.com / rchelicopterfun.com: 4.20 V = 100 %,
 * 3.84 V = 50 %, 3.27 V = 0 %). It is NOT measured on Mae's 16340 cell, and
 * it is a resting curve: under load (20-36 mA awake) the half reads a few mV
 * low, which the display's filter and hysteresis absorb. Twelve points, fine
 * in the flat 3.7-3.9 V zone where the old five-point dV table (3.3 / 3.5 /
 * 3.7 / 3.9 / 4.2 V) jumped 15 points per 0.1 V. Linear between points,
 * rounded; bounded [0; 100], monotonic (test_batt_calc). A comfort gauge,
 * not a coulomb counter. */
static inline uint8_t batt_soc_pct_mv(uint16_t mv)
{
    if (mv == 0) return 0xFF;
    static const struct { uint16_t mv; uint8_t pct; } t[] = {
        {3270,   0}, {3610,   5}, {3690,  10}, {3730,  20}, {3770,  30}, {3800,  40},
        {3840,  50}, {3870,  60}, {3950,  70}, {4020,  80}, {4110,  90}, {4200, 100},
    };
    const unsigned n = sizeof t / sizeof t[0];
    if (mv <= t[0].mv) return 0;
    for (unsigned i = 1; i < n; i++) {
        if (mv <= t[i].mv) {
            uint32_t span = t[i].mv - t[i - 1].mv;
            uint32_t off  = mv - t[i - 1].mv;
            return (uint8_t)(t[i - 1].pct + ((uint32_t)(t[i].pct - t[i - 1].pct) * off + span / 2) / span);
        }
    }
    return 100;
}

/* The same curve from a dV (the dongle only gets the STATUS batt_dV: one
 * curve, 0.1 V resolution there). 0xFF if unknown. */
static inline uint8_t batt_soc_pct(uint8_t dv)
{
    return dv ? batt_soc_pct_mv((uint16_t)(dv * 100u)) : 0xFF;
}

/* -- Displayed percentage (2026-09-30) --------------------------------------
 *
 * Bench 2026-09-30, left half on battery: 90 % / 100 % / FULL alternating.
 * The mV was rounded to dV before the curve (0.1 V = 10 % at the top), and
 * the display showed every reading.
 *
 * 1) Filter: an EMA of the MILLIVOLTS, weight 1/8 per sample. The gauge
 *    samples every 10 s awake (plus one on wake), so the time constant is
 *    ~80 s of awake time: a cell loses a percent in tens of minutes, so
 *    nothing real is hidden, while a sag (a keystroke, a radio burst, the
 *    ADC's own noise) is divided by 8 — a -40 mV sample moves it 5 mV.
 *    Filtering the mV rather than the % keeps the curve's non-linearity out
 *    of the average. Kept in 1/16 mV so the 1/8 steps do not truncate. The
 *    first sample is taken as is (boot shows right away); a rejected one (0)
 *    teaches nothing.
 * 2) Display: 5 % steps, hysteresis in time. Down only once the filtered SoC
 *    has been >= one step below for BATT_AFF_HOLD_MS (60 s) in a row. On
 *    battery it never goes up — except the escape hatch below. With USB power
 *    (the single rule, usb_presence_cable) it may go up, same 60 s hold.
 * 3) Escape hatch: the halves have no VBUS bridge and a wall charger does not
 *    enumerate, so a wall charge is invisible to the USB rule. A reading
 *    >= BATT_AFF_RECAL_PCT (20 %) above for BATT_AFF_RECAL_MS (5 min) on
 *    battery is a charge nobody saw: the display re-anchors. A load removed
 *    or a warm cell recovers far less than 20 %. */
typedef struct { uint32_t ema_x16; bool valide; } batt_ema_t;

static inline uint16_t batt_ema_step(batt_ema_t *e, uint16_t mv)
{
    if (mv == 0) return e->valide ? (uint16_t)((e->ema_x16 + 8u) >> 4) : 0;
    if (!e->valide) { e->valide = true; e->ema_x16 = (uint32_t)mv << 4; }
    else {
        int32_t d = (int32_t)((uint32_t)mv << 4) - (int32_t)e->ema_x16;
        e->ema_x16 = (uint32_t)((int32_t)e->ema_x16 + d / 8);
    }
    return (uint16_t)((e->ema_x16 + 8u) >> 4);
}

#define BATT_AFF_PAS          5u        /* displayed step, % */
#define BATT_AFF_HOLD_MS      60000u    /* a move must hold this long */
#define BATT_AFF_RECAL_PCT    20u       /* on battery: an unseen charge */
#define BATT_AFF_RECAL_MS     300000u

typedef struct {
    uint8_t  aff;              /* displayed %, valid when valide */
    bool     valide;
    bool     bas, haut;        /* a candidate below / above is being timed */
    bool     secteur;          /* USB power at the previous step */
    uint32_t bas_ms, haut_ms;
} batt_affiche_t;

static inline uint8_t batt_pct_quantifie(uint8_t soc)
{
    if (soc > 100) soc = 100;
    return (uint8_t)((soc + BATT_AFF_PAS / 2) / BATT_AFF_PAS * BATT_AFF_PAS);
}

/* soc: filtered SoC (0xFF = unknown); secteur: USB power present. Returns the
 * displayed %, 0xFF if unknown. An unknown reading shows "?" and stops the
 * timing (a move is never extended over a missing reading); the displayed
 * value itself is kept for the next valid one. */
static inline uint8_t batt_affiche_step(batt_affiche_t *s, uint8_t soc, bool secteur, uint32_t now_ms)
{
    if (soc == 0xFF) { s->bas = s->haut = false; return 0xFF; }
    uint8_t q = batt_pct_quantifie(soc);
    if (!s->valide) { s->valide = true; s->aff = q; s->bas = s->haut = false; s->secteur = secteur; return q; }
    if (secteur != s->secteur) { s->haut = false; s->secteur = secteur; }   /* the up rule changed */
    if (q + BATT_AFF_PAS <= s->aff) {
        s->haut = false;
        if (!s->bas) { s->bas = true; s->bas_ms = now_ms; }
        else if ((uint32_t)(now_ms - s->bas_ms) >= BATT_AFF_HOLD_MS) { s->aff = q; s->bas = false; }
    } else if (q >= s->aff + BATT_AFF_PAS) {
        s->bas = false;
        bool assez = secteur || q >= s->aff + BATT_AFF_RECAL_PCT;
        uint32_t hold = secteur ? BATT_AFF_HOLD_MS : BATT_AFF_RECAL_MS;
        if (!assez) s->haut = false;
        else if (!s->haut) { s->haut = true; s->haut_ms = now_ms; }
        else if ((uint32_t)(now_ms - s->haut_ms) >= hold) { s->aff = q; s->haut = false; }
    } else {
        s->bas = s->haut = false;
    }
    return s->aff;
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


/* The charge marker that may be SHOWN (screen) or SENT (STATUS): only with
 * USB power (usb_presence_cable, the single rule). With it, the charger is
 * fed: "+" until the end-of-charge plateau (batt_state_step) says FULL.
 * Without it, nothing — on battery the plateau of a full cell and the rise
 * after a load were showing FULL / "+" (bench 2026-09-30). */
static inline batt_chg_t batt_chg_affiche(batt_chg_t heur, bool usb)
{
    if (!usb) return BATT_CHG_UNKNOWN;
    return heur == BATT_CHG_FULL ? BATT_CHG_FULL : BATT_CHG_PROBABLE;
}
