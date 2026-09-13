/* Les deux slots RF du dongle.
 *
 * Historiquement c'étaient les deux moitiés d'un même clavier, et le dongle
 * réconciliait leurs demi-matrices. Ce n'est plus le cas : la moitié maître du
 * Niphargus lui envoie du HID déjà fini, et le second slot appartient à la
 * souris Conchodytes. Les deux appareils n'ont plus rien en commun.
 *
 * D'où ce module minuscule, dont l'unique raison d'être est de rendre cette
 * séparation impossible à oublier : **la perte d'un slot ne relâche que ce que
 * ce slot tenait**. Sous l'ancienne lecture, relâcher tout le clavier à la perte
 * de l'une ou l'autre moitié était juste ; aujourd'hui ce serait un bug — une
 * souris qui sort de portée effacerait la frappe en cours.
 *
 * Le repli lui-même reste nécessaire : un lien qui se tait laisse l'hôte sur le
 * dernier rapport reçu. Si c'était « Super enfoncé », il le reste.
 *
 * Les clés NVS d'appairage gardent leurs noms d'origine (`mac_left` /
 * `mac_right`) : les renommer désapparierait le matériel déjà appairé pour un
 * gain purement cosmétique. Le slot 0x01 y désigne le clavier, le 0x02 la souris.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RF_SLOT_KBD    0u   /* moitié maître Niphargus — envoie du HID fini */
#define RF_SLOT_MOUSE  1u   /* Conchodytes */
#define RF_SLOT_COUNT  2u

/* ── Plan de canaux 2,4 GHz ───────────────────────────────────────────────────
 *
 * Quatre liens coexistent. Ils étaient définis chacun dans son coin — le canal
 * d'appairage dans rf_pairing.h, ceux du dongle dans les board.h — et rien
 * n'empêchait deux d'entre eux de tomber sur la même fréquence. Une collision
 * ne casse aucune compilation : elle se manifeste par un lien qui se tait, ou
 * par des paquets qui passent par intermittence selon le trafic.
 *
 * Ils sont donc rassemblés ici, et verrouillés par test/test_rf_channel_plan.c.
 *
 * Espacement : le driver émet à 1 Mbps (RF_SETUP = 0x06), et le nRF24L01+
 * Product Specification §6.3 p.25 précise qu'à ce débit « the channel occupies
 * a bandwidth of less than 1MHz » — 1 MHz d'écart suffit donc. La contrainte de
 * 2 MHz ne vaut qu'à 2 Mbps.
 *
 * Placement : le WiFi 2,4 GHz monte jusqu'à ~2473 MHz (canal 11). Les trois
 * liens de données se tiennent au-dessus, là où la bande est nettement plus
 * calme. Le canal d'appairage, lui, est en plein WiFi — c'est assumé : il ne
 * sert que quelques secondes, à courte distance, et sous ARC 15.
 *
 * La bande ISM s'arrête à 2483,5 MHz. La puce monterait à 2525 (§6.3) mais on
 * n'y va pas : au-delà on brouille d'autres services. */
#define RF_CH_KBD_DONGLE    0x4C   /* 2476 MHz — moitié gauche → dongle, slot 1 */
#define RF_CH_HALF_LINK     0x4F   /* 2479 MHz — moitié droite → moitié gauche  */
#define RF_CH_MOUSE_DONGLE  0x52   /* 2482 MHz — Conchodytes → dongle, slot 2   */
/* Le canal d'appairage vit dans rf_pairing.h (RF_PAIR_CHANNEL, 0x28 / 2440 MHz)
 * et reste là-bas : il appartient au protocole d'appairage, pas au plan de
 * fonctionnement. Le test de plan l'inclut malgré tout dans ses collisions. */

/* Suffixes d'adresse, 5e octet après la base "KaSe". 0x01 clavier et 0x02
 * souris sont ceux des slots du dongle (voir plus haut) ; 0x03 désigne le lien
 * inter-moitiés, qui ne passe pas par le dongle. */
#define RF_ADDR_KBD_DONGLE  0x01   /* slot clavier du dongle (les DEUX moitiés en fusion) */
#define RF_ADDR_HALF_LINK   0x03

/* ── Supervision du lien clavier → dongle ───────────────────────────────────
 *
 * Le dongle déclare un slot PERDU après RF_LINK_LOST_MS de silence total et
 * relâche ses touches — sans quoi un clavier disparu laisserait une touche
 * collée chez l'hôte. Il attend donc une trame d'état au repos.
 *
 * Or PKT_TYPE_STATUS n'était JAMAIS émis : il n'existait qu'en décodage. Tant
 * qu'on tape, les rapports HID entretiennent le lien par accident ; mais une
 * touche MAINTENUE ne produit aucun changement, donc plus aucun rapport, et le
 * dongle relâchait la touche au bout de ~2 s. Constaté au banc le 2026-09-08.
 *
 * Les deux constantes sont ici, et non chacune de son côté : c'est un contrat
 * entre deux firmwares, et la moitié qui émet doit connaître le budget de celle
 * qui écoute. Verrouillé par test/test_rf_status_cadence.c. */
#define RF_STATUS_PERIOD_MS  1000u   /* cadence de la trame d'état */
#define RF_REARM_SILENCE_MS  2000u   /* silence → réécrire la config RX (radio figée) */
#define RF_LINK_LOST_MS      2500u   /* silence → slot perdu, repli appliqué */

/* Faut-il émettre une trame d'état maintenant ? `dernier_ms` est la date de la
 * DERNIÈRE émission quelle qu'elle soit — un rapport HID entretient le lien
 * aussi bien qu'une trame d'état, inutile d'en ajouter pendant la frappe.
 * Écart en arithmétique non signée : le compteur de ms déborde vers 49 jours. */
static inline bool rf_status_doit_emettre(uint32_t now_ms, uint32_t dernier_ms,
                                          uint32_t periode_ms)
{
    return (uint32_t)(now_ms - dernier_ms) >= periode_ms;
}

typedef enum {
    RF_SAFE_NONE = 0,
    RF_SAFE_RELEASE_KEYS,      /* rapport clavier vide */
    RF_SAFE_RELEASE_BUTTONS,   /* rapport souris à zéro */
} rf_safe_action_t;

typedef struct {
    uint32_t last_rx_ms;
    bool     up;
} rf_slot_link_t;

/* Tout paquet reçu — battement, état, rapport HID — vaut preuve de vie. */
static inline void rf_slot_link_rx(rf_slot_link_t *l, uint32_t now_ms)
{
    if (l == NULL) return;
    l->last_rx_ms = now_ms;
    l->up = true;
}

/* À appeler à chaque tour de la boucle RF. Rend l'action de repli à effectuer,
 * une seule fois par perte : la boucle tourne toutes les 10 ms, un déclenchement
 * répété noierait l'endpoint HID de rapports vides.
 *
 * La soustraction est non signée à dessein : le compteur de millisecondes
 * repasse par zéro après ~49 jours, ce qu'un dongle branché en permanence
 * atteint. */
static inline rf_safe_action_t rf_slot_link_check(rf_slot_link_t *l, uint8_t slot,
                                                  uint32_t now_ms, uint32_t timeout_ms)
{
    if (l == NULL || !l->up) return RF_SAFE_NONE;
    if ((uint32_t)(now_ms - l->last_rx_ms) < timeout_ms) return RF_SAFE_NONE;
    l->up = false;
    return (slot == RF_SLOT_MOUSE) ? RF_SAFE_RELEASE_BUTTONS : RF_SAFE_RELEASE_KEYS;
}
