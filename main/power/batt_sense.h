#pragma once
#include <stdint.h>

/* Battery gauge of a Niphargus half — ADC read of VBAT_SENSE.
 * The logic (conversion, rejection, SoC, states) is pure and tested: batt_calc.h.
 *
 * Sampled every 10 s while awake, and once on wake-up (veille.c): the
 * esp_timer clock is frozen during light sleep, and a value several
 * minutes old is of no interest. Never sample DURING sleep.
 * 0 = unknown, the batt_dV convention. */
void     batt_sense_init(void);
void     batt_sense_sample_now(void);
uint8_t  batt_sense_dv(void);
uint8_t  batt_sense_charging(void);   /* batt_chg_t: 0 unknown, 1 probably charging, 2 full */
uint8_t  batt_sense_niveau(void);     /* batt_niveau_t: 0 normal, 1 LOW (< 3.5 V), 2 CRITICAL (< 3.3 V) */
uint32_t batt_sense_age_ms(void);     /* 0xFFFFFFFF = no valid sample yet */
