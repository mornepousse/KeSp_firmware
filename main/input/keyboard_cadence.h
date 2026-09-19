#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cadence.h"   /* KBD_CADENCE_* */

/* Cadence de la tâche clavier (logique pure, testée host).
 *
 * La boucle de vTaskKeyboard n'a besoin de ses 10 ms que pour faire vivre des
 * minuteries : tap-hold et tap-dance (200 ms), leader (1000 ms), le mode test
 * matrice, et la fusion distante quand la gauche tape en USB. Au repos, rien
 * de tout ça ne court — et pourtant une boucle de 10 ms (un tick à 100 Hz)
 * réveille le processeur cent fois par seconde et interdit le light sleep
 * automatique d'ESP-IDF, qui exige CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP
 * (3) ticks sans personne : mesuré au banc le 2026-09-16, mode SLEEP 92 % du
 * temps oisif et light_sleep_counts = 0.
 *
 * Un changement de matrice notifie la tâche (xTaskNotifyGive) : l'attente
 * longue ne coûte rien à la première touche, elle ne retarde que les
 * minuteries — d'où la fenêtre d'activité, plus large que la plus longue
 * d'entre elles. */


static inline uint32_t kbd_cadence_attente_ms(uint32_t now_ms, uint32_t derniere_activite_ms,
                                              bool usb_present, bool test_matrice)
{
    if (usb_present || test_matrice) return KBD_CADENCE_ACTIF_MS;
    if ((uint32_t)(now_ms - derniere_activite_ms) < KBD_CADENCE_FENETRE_MS) return KBD_CADENCE_ACTIF_MS;
    return KBD_CADENCE_REPOS_MS;
}
