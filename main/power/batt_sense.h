#pragma once
#include <stdint.h>

/* Jauge batterie d'une moitié Niphargus — lecture ADC de VBAT_SENSE.
 * La logique (conversion, rejet, SoC, états) est pure et testée : batt_calc.h.
 *
 * Mesure toutes les 10 s éveillée, et une au réveil (veille.c) : le timer
 * esp_timer est gelé pendant le light sleep, et une valeur de plusieurs
 * minutes n'a aucun intérêt. Jamais de mesure PENDANT le sommeil.
 * 0 = inconnu, la convention de batt_dV. */
void     batt_sense_init(void);
void     batt_sense_sample_now(void);
uint8_t  batt_sense_dv(void);
uint8_t  batt_sense_charging(void);   /* batt_chg_t : 0 inconnu, 1 en charge probable, 2 pleine */
uint32_t batt_sense_age_ms(void);     /* 0xFFFFFFFF = jamais de mesure valide */
