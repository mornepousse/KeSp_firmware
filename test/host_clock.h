/* Horloge monotone contrôlable pour les tests host. Sert de backend à
 * esp_timer_get_time() (défini dans host_clock.c → un seul symbole pour tout le
 * runner) afin que les modules à timing (tap_hold, tap_dance, …) soient pilotés
 * de façon déterministe : host_clock_reset() puis host_clock_advance_ms(). */
#pragma once
#include <stdint.h>

void host_clock_reset(void);              /* remet l'horloge à 0 */
void host_clock_advance_ms(uint32_t ms);  /* avance de ms millisecondes */
