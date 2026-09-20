#pragma once
/* radio_owner — la puce nRF24 d'une moitié du Niphargus a UN propriétaire.
 *
 * Trois pannes « mauvais canal en silence » et une FIFO vidée au retour
 * d'excursion (CLAUDE.md « Une puce, un propriétaire », « Un acquittement ESB
 * ne prouve pas la réception logicielle ») : toutes des modules qui écrivaient
 * la config de la puce chacun de leur côté. Ici un seul état (mode + cible),
 * un seul verrou, et les invariants sont des fonctions :
 *   - un mode à la fois : PTX vers une cible, PRX à l'écoute d'une cible,
 *     éteinte ; changer de mode est idempotent ;
 *   - émettre en PRX est REFUSÉ — c'est l'excursion qu'il faut ;
 *   - une excursion VIDE la FIFO de réception dans le consommateur AVANT de
 *     partir (elle se termine par un FLUSH_RX) et revient écouter la cible
 *     d'avant ;
 *   - le réveil RÉARME le mode courant (rf_driver_power_up ne touche pas à
 *     CE : sans ça une PRX repart sourde) ;
 *   - le verrou est tenu pendant tout le sommeil.
 * Le module parle au matériel par une table d'opérations (radio_hw_t) :
 * rf_driver_* en production, un faux enregistreur dans les tests host — la
 * SÉQUENCE d'appels est l'oracle (test/test_radio_owner.c).
 *
 * Ce module ne connaît ni les trames ni les politiques : half_link.c (la
 * droite) et kbd_relay_tx.c (la gauche) décident QUOI émettre et VERS QUI ;
 * lui garantit COMMENT la puce est touchée. */
#include <stdbool.h>
#include <stdint.h>
#include "rf_driver.h"

typedef enum { RADIO_ETEINTE = 0, RADIO_PTX, RADIO_PRX } radio_mode_t;

/* Table d'opérations matérielles. NULL à l'init → rf_driver_*. */
typedef struct {
    esp_err_t (*init_tx)(rf_radio_t *, const rf_radio_cfg_t *);
    void      (*set_ptx)(rf_radio_t *, const rf_radio_cfg_t *);
    void      (*rearm_rx)(rf_radio_t *, const rf_radio_cfg_t *);
    bool      (*send)(rf_radio_t *, const uint8_t *, uint8_t);
    bool      (*send_ap)(rf_radio_t *, const uint8_t *, uint8_t, uint8_t *, uint8_t *);
    bool      (*oob_tx)(rf_radio_t *, uint8_t, const uint8_t[5], const uint8_t *, uint8_t,
                        uint8_t, const uint8_t[5]);
    bool      (*rx_available)(rf_radio_t *);
    uint16_t  (*read_rx)(rf_radio_t *, uint8_t *, uint16_t);
    void      (*power_down)(rf_radio_t *);
    void      (*power_up)(rf_radio_t *);
    void      (*set_tx_address)(rf_radio_t *, const uint8_t[5]);
    void      (*set_channel)(rf_radio_t *, uint8_t);
    uint16_t  (*pair_listen)(rf_radio_t *, uint8_t, const uint8_t[5], uint8_t *, uint16_t, uint32_t);
} radio_hw_t;

typedef void (*radio_rx_cb_t)(const uint8_t *trame, uint16_t n, void *ctx);

/* Init : la puce en PTX vers `cible`. Enregistre le hook de veille « radio »
 * (sommeil : power-down, verrou gardé ; réveil : power-up + réarmement).
 * Retourne false si la puce est absente (probe) : tout le reste refuse alors. */
bool radio_owner_init(const rf_radio_cfg_t *cible, const radio_hw_t *hw);
bool radio_presente(void);

/* Verrou de la puce : TOUTE transaction, CSN compris. C'est aussi le prêt du
 * bus SPI à l'écran — rf_bus_lock/unlock/host (rf_bus.h) sont implémentés ici,
 * une seule fois pour les deux moitiés. */
bool radio_lock(uint32_t timeout_ms);
void radio_unlock(void);

/* Mode. PTX vers `cfg` (canal/adresse) ou PRX à l'écoute de `cfg`. Idempotent
 * si déjà dans ce mode vers cette cible. Prend le verrou (50 ms). */
bool radio_mode_set(radio_mode_t mode, const rf_radio_cfg_t *cfg);
/* Réécrit la config du mode courant même si rien n'a changé : le chien de
 * garde d'une puce figée (clone nRF24 qui n'acquitte plus rien jusqu'au reset). */
bool radio_rearmer(void);
radio_mode_t          radio_mode(void);
const rf_radio_cfg_t *radio_cible(void);

/* Émission — PTX SEULEMENT (refusée en PRX : passer par l'excursion).
 *   ACK      : partie et acquittée ;
 *   REFUS    : partie, l'ESB l'a refusée (MAX_RT) ;
 *   INDISPO  : rien n'est parti — verrou pris sous timeout_ms, mauvais mode,
 *              puce endormie ou absente ;
 *   PERIME   : rien n'est parti — `encore_valide(ctx)`, évalué UNE FOIS LE
 *              VERROU ACQUIS, a dit que l'état à émettre n'est plus le courant.
 * C'est ce dernier cas qui ferme la course « répétition d'un appui émise après
 * le relâchement » (double appui sur appui court, banc 2026-09-20) : une
 * répétition snapshotte l'état et sa génération, puis attend le verrou derrière
 * l'émission du relâchement ; sans ce contrôle elle émettait l'appui périmé.
 * `ack`/`ack_len` NULL → pas de charge d'ACK ; sinon EN_ACK_PAY, *ack_len = 0 si
 * l'ACK était nu ou rien n'est parti. `encore_valide` NULL → toujours valide. */
typedef enum { RADIO_TX_ACK = 0, RADIO_TX_REFUS, RADIO_TX_INDISPO, RADIO_TX_PERIME } radio_tx_t;
typedef bool (*radio_valide_cb_t)(void *ctx);
radio_tx_t radio_emettre(const uint8_t *buf, uint8_t len, uint8_t *ack, uint8_t *ack_len,
                         uint32_t timeout_ms, radio_valide_cb_t encore_valide, void *ctx);
/* Raccourcis : vrai si ACK. */
bool radio_send(const uint8_t *buf, uint8_t len, uint32_t timeout_ms);
bool radio_send_ap(const uint8_t *buf, uint8_t len, uint8_t *ack, uint8_t *ack_len, uint32_t timeout_ms);

/* PRX : lire tout ce qui attend et le livrer à `cb`. Rien en PTX. */
void radio_rx_drain(radio_rx_cb_t cb, void *ctx);

/* PRX : émettre AILLEURS (canal/adresse) puis revenir écouter la cible
 * courante. Vide la FIFO dans `cb` AVANT de partir (cb NULL = trames perdues,
 * en le disant). Refusée en PTX (radio_send suffit). */
bool radio_excursion_tx(uint8_t canal, const uint8_t addr[5], const uint8_t *buf, uint8_t len,
                        radio_rx_cb_t cb, void *ctx);

/* Un tour d'appairage : viser le rendez-vous (adresse + canal), émettre `req`,
 * écouter `listen_ms` la réponse (rendue dans rx/rx_n), puis REVENIR à la
 * cible courante — quoi qu'il arrive. Sous verrou. PTX seulement. */
bool radio_pair_round(const uint8_t rdv_addr[5], uint8_t rdv_ch, const uint8_t *req, uint8_t n,
                      uint8_t *rx, uint16_t rx_max, uint32_t listen_ms, uint16_t *rx_n);

#if CONFIG_KASE_RF_CE_SCAN
/* Banc V2D uniquement : essayer une autre broche CE (câblage incertain). */
void radio_ce_gpio(int gpio);
#endif

/* Veille : appelés par le hook enregistré à l'init (publics pour les tests).
 * Idempotents (le sommeil profond rappelle les hooks après le léger) ; le
 * verrou n'est rendu au réveil que s'il a été pris au sommeil. */
void radio_sleep(void);
void radio_wake(void);

/* Depuis le boot : émissions acquittées, refusées par l'ESB (MAX_RT),
 * INDISPONIBLES (verrou pris, mauvais mode, puce endormie : rien n'est parti)
 * et PÉRIMÉES (état dépassé au moment du verrou : rien n'est parti). NULL ok. */
void radio_stats(uint32_t *ok, uint32_t *refus, uint32_t *indispo, uint32_t *perimes);
