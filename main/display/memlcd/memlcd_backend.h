#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "display_backend.h"

/* Backend Sharp memory-LCD des moitiés Niphargus (portrait 68 × 160).
 * Spec : docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md */
extern const display_backend_t memlcd_display_backend;

/* Ce que l'écran ne sait pas tout seul : la couche courante (droite), la
 * batterie de l'AUTRE moitié et « dongle vu » vus du dongle. Alimenté par la
 * trame DISPLAY reçue dans l'ACK (Task 6) ; tant qu'elle n'est pas arrivée,
 * batt_autre_dv vaut 0xFF (« inconnue »). Appelable de n'importe quelle tâche
 * (copie de scalaires, consommée au prochain update()). */
void memlcd_backend_set_remote(uint8_t couche, uint8_t batt_autre_dv, uint8_t batt_autre_chg, bool dongle_ok);
