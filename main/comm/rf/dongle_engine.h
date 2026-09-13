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

/* Phase 2 : la gauche annonce son mode. En USB, le dongle se tait (la gauche
 * tape en local) et réémet la droite ; sinon il tape. Appelé depuis rf_rx_task
 * (STATUS+USB → true ; matrice brute de la gauche → false). */
void dongle_engine_set_left_usb(bool usb);

/* Le dongle est-il en mode « gauche USB » ? Lu par drain_radio pour décider de
 * réémettre la droite → gauche. */
bool dongle_engine_left_usb(void);

/* Garde-fou de sync : la gauche a annoncé l'empreinte de sa keymap (champ
 * config_fp de PKT_TYPE_STATUS). Le dongle la mémorise et, au CHANGEMENT,
 * journalise l'accord ou la divergence avec la sienne. Appelé depuis
 * rf_rx_task (STATUS du slot clavier). */
void dongle_engine_note_left_fp(uint32_t fp);

/* Instantané de cohérence pour le contrôleur (CDC). own_fp = empreinte de la
 * keymap du dongle ; left_fp = dernière annoncée par la gauche (0 = jamais) ;
 * age_ms = ancienneté de cette annonce (0xFFFFFFFF = jamais) ; match = les deux
 * concordent (égales et non nulles). Chaque pointeur peut être NULL. */
void dongle_engine_get_coherence(uint32_t *own_fp, uint32_t *left_fp,
                                 uint32_t *age_ms, bool *match);

#endif /* CONFIG_KASE_DONGLE_FUSION */
