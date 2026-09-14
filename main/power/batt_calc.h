#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Jauge batterie — logique pure, testée host (test/test_batt_calc.c).
 *
 * Matériel (docs/NIPHARGUS_V2_HARDWARE.md) : VBAT_SENSE = pont 1 MΩ / 1 MΩ
 * + 100 nF sur ADC2_CH2 → V_batt = 2 × V_adc. Unité de transport : le dV du
 * champ batt_dV de PKT_TYPE_STATUS (42 = 4,2 V), 0 = inconnu.
 *
 * Rejet : hors [2,5 V ; 4,5 V] = capteur absent, pont ouvert, erreur ADC —
 * on dit « inconnu » plutôt qu'un chiffre faux. Une Li-ion 16340 vit entre
 * ~3,0 V (coupure DW01A) et 4,2 V.
 *
 * « En charge » n'est PAS mesurable : le pont VBUS (GPIO33) n'est pas peuplé
 * et le TP4056 n'a pas de STDBY câblé. On DÉDUIT de la tension seule :
 *   - PLEINE : plateau ≥ 4,15 V tenu ≥ 2 min (fin de charge du TP4056), gardée
 *     avec hystérésis ;
 *   - EN CHARGE PROBABLE : hausse ≥ 0,1 V dans une fenêtre de 5 min — une
 *     décharge ne monte jamais ; une dérive lente (température, bruit) ne
 *     franchit pas le seuil dans la fenêtre.
 * Design : docs/superpowers/specs/2026-09-14-batterie-jauge-design.md */

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

/* mV batterie → dV arrondi ; 0 si hors plage. */
static inline uint8_t batt_mv_to_dv_batt(uint32_t mv_batt)
{
    return batt_mv_plausible(mv_batt) ? (uint8_t)((mv_batt + 50u) / 100u) : 0u;
}

/* mV lus à l'ADC (côté pont) → dV batterie ; 0 si hors plage. */
static inline uint8_t batt_mv_to_dv(uint32_t mv_adc)
{
    return batt_mv_to_dv_batt(batt_mv_from_adc(mv_adc));
}

/* Moyenne de n lectures ADC (mV côté pont) → mV batterie ; 0 si n == 0 ou hors plage. */
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

/* SoC approché d'une Li-ion 16340 au repos, par morceaux :
 * 3,3 V → 0 %, 3,5 → 15, 3,7 → 40, 3,9 → 70, 4,2 → 100. 0xFF si inconnu.
 * Une jauge de confort, pas un coulomb-mètre : la charge et la température la
 * décalent — c'est assumé et documenté côté CDC. */
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
    BATT_CHG_UNKNOWN  = 0,   /* décharge, ou rien de déductible */
    BATT_CHG_PROBABLE = 1,   /* la tension monte : une charge est en cours */
    BATT_CHG_FULL     = 2,   /* plateau de fin de charge tenu */
} batt_chg_t;

typedef struct {
    uint32_t   ref_mv;            /* bas de la fenêtre de hausse */
    uint32_t   ref_ms;
    bool       ref_valid;
    uint32_t   plateau_since_ms;  /* début du plateau haut */
    bool       plateau;
    batt_chg_t etat;
} batt_state_t;

/* Une mesure (mV batterie, 0 = inconnue) à l'instant now_ms → état déduit.
 * Une mesure inconnue remet tout à zéro : on ne prolonge jamais une déduction
 * sur une lecture absente. */
static inline batt_chg_t batt_state_step(batt_state_t *s, uint32_t mv, uint32_t now_ms)
{
    if (mv == 0) {
        *s = (batt_state_t){0};
        return BATT_CHG_UNKNOWN;
    }

    /* Plateau haut → PLEINE, avec hystérésis à la sortie. */
    if (mv >= BATT_FULL_MV) {
        if (!s->plateau) { s->plateau = true; s->plateau_since_ms = now_ms; }
        if ((uint32_t)(now_ms - s->plateau_since_ms) >= BATT_FULL_HOLD_MS)
            s->etat = BATT_CHG_FULL;
    } else if (mv < BATT_FULL_MV - BATT_HYST_MV) {
        s->plateau = false;
        if (s->etat == BATT_CHG_FULL) s->etat = BATT_CHG_UNKNOWN;
    }
    if (s->etat == BATT_CHG_FULL) return s->etat;

    /* Hausse dans la fenêtre → EN CHARGE PROBABLE. La référence est le point
     * bas : elle repart à chaque baisse (une décharge ne peut jamais cumuler)
     * et à l'expiration de la fenêtre (une dérive lente ne cumule pas non plus). */
    if (!s->ref_valid || mv < s->ref_mv ||
        (uint32_t)(now_ms - s->ref_ms) > BATT_RISE_WINDOW_MS) {
        if (s->ref_valid && mv < s->ref_mv && s->etat == BATT_CHG_PROBABLE)
            s->etat = BATT_CHG_UNKNOWN;          /* ça baisse : la charge a cessé */
        s->ref_mv = mv; s->ref_ms = now_ms; s->ref_valid = true;
    } else if (mv >= s->ref_mv + BATT_RISE_MV) {
        s->etat = BATT_CHG_PROBABLE;
    }
    return s->etat;
}
