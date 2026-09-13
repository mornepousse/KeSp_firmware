#pragma once
#include "sdkconfig.h"
#if CONFIG_KASE_DONGLE_FUSION
#include "rf_packet.h"

/* Moteur keymap embarqué dans le dongle — mode FUSION uniquement.
 *
 * Le dongle reçoit les deux demi-matrices BRUTES (PKT_TYPE_MATRIX), les fusionne
 * et fait tourner le moteur keymap partagé (le même code que la gauche), puis
 * sort le HID par son USB. Voir
 * docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md et
 * docs/superpowers/plans/2026-09-13-dongle-fusion-runtime.md.
 *
 * Découpage des responsabilités face à la concurrence : rf_rx_task (writer)
 * dépose les trames via dongle_engine_on_matrix() ; la tâche moteur (reader)
 * consomme. Seul l'état de fusion est partagé — protégé par un mutex interne.
 * current_press_* et le cycle moteur ne sont touchés QUE par la tâche moteur. */

/* Initialise le moteur (tap-hold, tap-dance, combos, leader, overrides, HID) et
 * démarre la tâche moteur. À appeler une fois, depuis rf_rx_start(). */
void dongle_engine_start(void);

/* Une demi-matrice brute vient d'arriver. Route par identité de moitié et
 * signale un changement si le bitmap a bougé. Sûr à appeler depuis rf_rx_task. */
void dongle_engine_on_matrix(const rf_matrix_t *m);

#endif /* CONFIG_KASE_DONGLE_FUSION */
