/* Niphargus sleep — brick B7.
 *
 * Two stages, because neither one alone fits:
 *
 *   LIGHT    light sleep, ~240 µA (ESP32-S3 datasheet v2.2, table 5-10, p. 68)
 *            plus 4 µA for the HT7833. State is kept, wake takes
 *            ~1 ms. On a ~650 mAh 16340, four hours at this regime
 *            cost 1 mAh — that's what lets this stage be held for HOURS
 *            rather than minutes, and so have a keyboard that comes back
 *            instantly all day long.
 *
 *   DEEP     deep sleep, 8 µA (same table), wake by EXT1 on the rows.
 *            RAM is lost: waking is a full restart, measured at 683 ms
 *            on the bench. It is only reached after hours, where this delay
 *            goes unnoticed — and there, the cell's self-discharge dominates.
 *
 * The ULP is ruled out: 170 µA on its own (same table), more than three times
 * the 50 µA target. The "RTC scan" from the design can't hold it.
 *
 * ⚠ THE RADIO IS OFF AS SOON AS THE LIGHT STAGE STARTS. Listening costs 13.1 mA
 * (nRF24L01+ PS v1.0, table 4, p. 14), two orders of magnitude above
 * any sleep budget, and the nRF24 has no low-power listen mode. A sleeping
 * half therefore does NOT hear the other: each one wakes on
 * ITS OWN matrix. Accepted consequence — after a long absence, the
 * first keystroke must be on the left half, the one that talks to the host.
 * This is a chip constraint, not an implementation choice; polling
 * would cost more than not sleeping (a 3 ms CPU wake every
 * 500 ms already costs 240 µA).
 *
 * Pure logic, host-tested in test/test_veille.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VEILLE_AUCUNE = 0,   /* stay awake */
    VEILLE_LEGERE,       /* light sleep, matrix GPIO wake */
    VEILLE_PROFONDE,     /* deep sleep, EXT1 wake */
} veille_t;

/* Default thresholds. The light one is SHORT: awake and idle the board draws
 * ~28 mA at 160 MHz (ESP32-S3 datasheet v2.2, table 5-9, p. 67) versus 0.24 mA
 * asleep — every second of waiting is worth a hundred seconds of sleep, and at
 * 60 s a day of typing was losing ~0.2 V (2026-09-15). The deep one is counted
 * in hours, since the light stage costs almost nothing and avoids the restart. */
#define VEILLE_LEGERE_MS     15000u      /* 15 s   */
#define VEILLE_PROFONDE_MS   14400000u   /* 4 h    */

/* `bloque` forbids any sleep: USB plugged in, update in progress, or any
 * reason to stay awake. It overrides both thresholds — an inactivity
 * duration, however long, must never put to sleep a keyboard that is
 * being used some other way. */
#ifndef TEST_HOST
/* Firmware. veille_pas() applies the decision: it blocks until wake
 * for the light stage, and never returns for the deep one. */
void veille_liberer_gpio(void);   /* at boot, BEFORE matrix_setup() */
void veille_legere_entrer(void);
void veille_profonde_entrer(void);
void veille_pas(uint32_t inactif_ms, bool bloque);
/* Light stage threshold, in ms: CONFIG_KASE_VEILLE_LEGERE_S by default,
 * VEILLE_LEGERE_CRITIQUE_MS when the battery is critical (veille_task). */
uint32_t veille_seuil_legere_ms(void);
void     veille_seuil_legere_set(uint32_t ms);
#define VEILLE_LEGERE_CRITIQUE_MS 5000u   /* lower bound of test_veille: [5 ; 20] s */
/* Diagnostic: when inactivity exceeds the light threshold but sleep
 * is blocked, say BY WHAT, at most once per 30 s. A night at 20 mA
 * instead of 244 µA (0.2 V lost on the left, 2026-09-12) left no
 * trace because nothing logged a refused sleep. */
/* Tally since boot: number of light sleeps and total time slept (ms,
 * measured against esp_timer, which follows the RTC). Feeds the heartbeat. */
void veille_bilan(uint32_t *sommeils, uint32_t *dormi_ms);
#endif

/* Grace period after a GPIO wake: during VEILLE_GRACE_REVEIL_MS we do not go
 * back to sleep, even if inactivity (never refreshed by a wake without a key)
 * says otherwise. A key with a slow pre-contact wakes the board before
 * capture sees it; the recreated driver will see it within these 300 ms and
 * emit it. A glitch costs 300 ms of wake time (~2 µAh), not 15 s of radio.
 * dernier_reveil_ms = 0: never woken, no grace. Unsigned subtraction:
 * holds up against counter overflow. */
#define VEILLE_GRACE_REVEIL_MS 300u
static inline bool veille_en_grace(uint32_t now_ms, uint32_t dernier_reveil_ms, uint32_t grace_ms)
{
    if (dernier_reveil_ms == 0) return false;
    return (uint32_t)(now_ms - dernier_reveil_ms) < grace_ms;
}

static inline veille_t veille_niveau(uint32_t inactif_ms, bool bloque,
                                     uint32_t seuil_legere_ms,
                                     uint32_t seuil_profonde_ms)
{
    if (bloque)                               return VEILLE_AUCUNE;
    if (inactif_ms >= seuil_profonde_ms)      return VEILLE_PROFONDE;
    if (inactif_ms >= seuil_legere_ms)        return VEILLE_LEGERE;
    return VEILLE_AUCUNE;
}
