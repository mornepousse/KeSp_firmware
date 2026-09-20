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

/* Sync auto de la keymap par ACK payload (phase 3). Une divergence est CONNUE
 * quand la gauche a annoncé une empreinte (≠ 0) différente de la nôtre : c'est
 * le seul moment où le dongle glisse quelque chose dans les ACK. Dès que la
 * gauche annonce notre empreinte (match=1), silence — coût nul en régime
 * synchronisé. */
/* Compteur de diagnostic : trames qui ont changé l'état d'une moitié avant que
 * le moteur ait consommé le changement précédent (tap potentiellement perdu). */
uint32_t dongle_engine_transitions_ecrasees(void);
/* Écart max entre deux tours du moteur depuis la dernière lecture (ms), remis à 0. */
uint32_t dongle_engine_gap_max_ms(void);
/* Ré-appuis d'une même touche < 30 ms après son relâchement (répétition
 * périmée d'une moitié, ou rebond mécanique) — CDC RF_STATUS[43..46]. */
uint32_t dongle_engine_reappuis(void);
/* Le dernier ré-appui : moitié (RF_HALF_*), touche (row*7+col), délai en ms
 * après le relâchement — CDC RF_STATUS[47..50]. */
void dongle_engine_dernier_reappui(uint8_t *half, uint8_t *key, uint16_t *delta_ms);

bool dongle_sync_active(void);

/* Construit la charge d'ACK à charger pour la PROCHAINE trame de la gauche, à
 * partir de ce qu'elle a demandé : req_next < SYNC_N_CHUNKS → le CHUNK
 * req_next (tranché dans keymaps[]) ; sinon → la BEACON {empreinte, 40}.
 * Écrit dans out (≤ 32 o), rend la longueur (0 = rien). */
uint16_t dongle_sync_ack_for(uint8_t req_next, uint8_t *out);

#endif /* CONFIG_KASE_DONGLE_FUSION */
