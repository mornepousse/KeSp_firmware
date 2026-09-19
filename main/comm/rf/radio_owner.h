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

/* Émission — PTX SEULEMENT (refusée en PRX : passer par l'excursion). Prend le
 * verrou (timeout_ms), retourne l'ACK. `_ap` rend la charge utile de l'ACK
 * (EN_ACK_PAY), *ack_len = 0 si l'ACK était nu ou l'envoi refusé. */
bool radio_send(const uint8_t *buf, uint8_t len, uint32_t timeout_ms);
bool radio_send_ap(const uint8_t *buf, uint8_t len, uint8_t *ack, uint8_t *ack_len, uint32_t timeout_ms);

/* PRX : lire tout ce qui attend et le livrer à `cb`. Rien en PTX. */
void radio_rx_drain(radio_rx_cb_t cb, void *ctx);

/* PRX : émettre AILLEURS (canal/adresse) puis revenir écouter la cible
 * courante. Vide la FIFO dans `cb` AVANT de partir (cb NULL = trames perdues,
 * en le disant). Refusée en PTX (radio_send suffit). */
bool radio_excursion_tx(uint8_t canal, const uint8_t addr[5], const uint8_t *buf, uint8_t len,
                        radio_rx_cb_t cb, void *ctx);

/* Veille : appelés par le hook enregistré à l'init (publics pour les tests). */
void radio_sleep(void);
void radio_wake(void);

/* Émissions acquittées / refusées depuis le boot (écran « dongle vu », banc). */
void radio_stats(uint32_t *ok, uint32_t *refus);
