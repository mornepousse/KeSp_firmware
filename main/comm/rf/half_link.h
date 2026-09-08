#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "rf_packet.h"   /* bitmap de demi-matrice + ses accesseurs */

/* Lien radio inter-moitiés du Niphargus — brick B3.
 *
 * La moitié DROITE émet sa demi-matrice, la GAUCHE l'écoute. La droite n'a ni
 * moteur keymap ni sortie HID : la spec la décrit comme « un scanner qui
 * remonte sa matrice brute ». La gauche fusionne ce qu'elle reçoit avec son
 * propre balayage — colonnes 0-6 pour elle, 7-13 pour la droite, d'où
 * KEYMAP_COLS = 2 × MATRIX_COLS sur le maître.
 *
 * Canal 0x4F (2479 MHz), adresse KaSe.03 — voir le plan de canaux dans
 * rf_slot.h. Trame : PKT_TYPE_HEARTBEAT, qui porte déjà le bitmap de
 * demi-matrice, la jauge batterie et un numéro de séquence.
 *
 * ⚠ Ce module ne traite PAS le risque R1 — la gauche ne peut pas écouter
 * pendant qu'elle émet vers le dongle. La bascule PRX/PTX est l'étape
 * suivante ; on prouve d'abord que le lien porte, sinon un échec de bascule
 * serait indiscernable d'un lien qui ne marche pas. */

/* ── État de la moitié distante, vu par le maître (logique pure) ────────────
 *
 * Testée host dans test/test_half_state.c, sur le modèle des inlines de
 * comm/usb/usb_presence.h.
 *
 * L'état est ABSOLU, pas différentiel : chaque trame porte la matrice entière,
 * donc une trame perdue se rattrape à la suivante sans accumulation ni dérive.
 * C'est ce qui rend le lien tolérant aux pertes que R1 mesure.
 *
 * ⚠ Le repli sur silence est la partie dangereuse. Une moitié qui sort de
 * portée ou dont la pile meurt laisserait l'hôte sur le dernier état reçu — et
 * si c'était « Maj enfoncée », il le reste. On relâche donc au bout d'un
 * silence, mais UNIQUEMENT ce que cette moitié tenait : rf_slot.h prévient
 * qu'un relâchement mal ciblé serait pire que le mal. */
typedef struct {
    uint8_t  bitmap[RF_HALF_BITMAP_BYTES];  /* dernier état reçu */
    uint32_t derniere_ms;                   /* quand il l'a été */
    bool     vivant;                        /* false = silence déjà constaté */
} half_state_t;

/* Une trame vient d'arriver : elle remplace l'état précédent. */
static inline void half_state_recu(half_state_t *st, const uint8_t *bitmap,
                                   uint32_t now_ms)
{
    memcpy(st->bitmap, bitmap, RF_HALF_BITMAP_BYTES);
    st->derniere_ms = now_ms;
    st->vivant = true;
}

/* Appelé périodiquement. Retourne true UNE SEULE FOIS, au moment où le silence
 * dépasse le délai : c'est le signal « relâche ce que cette moitié tenait ».
 * Les appels suivants retournent false tant qu'aucune trame n'est revenue —
 * sinon le moteur relâcherait à chaque cycle des touches déjà relâchées. */
static inline bool half_state_timeout(half_state_t *st, uint32_t now_ms,
                                      uint32_t delai_ms)
{
    if (!st->vivant) return false;
    if ((uint32_t)(now_ms - st->derniere_ms) < delai_ms) return false;
    memset(st->bitmap, 0, RF_HALF_BITMAP_BYTES);
    st->vivant = false;
    return true;
}

/* La touche (row, col) de la moitié distante est-elle enfoncée ? Coordonnées
 * LOCALES à cette moitié ; le décalage vers les colonnes 7-13 de la keymap est
 * la responsabilité de l'appelant. */
static inline bool half_state_pressed(const half_state_t *st, uint8_t row,
                                      uint8_t col)
{
    return rf_bitmap_get(st->bitmap, row, col);
}

/* ── Géométrie : où ranger les colonnes de la moitié distante ───────────────
 *
 * Testée host dans test/test_half_col_map.c.
 *
 * Les deux moitiés sont le même PCB retourné : la colonne 0 de la gauche est sa
 * touche la plus à GAUCHE, donc par symétrie la colonne 0 de la droite est sa
 * touche la plus à DROITE. Un simple décalage `col + 7` range alors la moitié
 * droite à l'envers — on tape la rangée de repos et il sort « ;lkjh ».
 *
 * Le miroir est une propriété du CÂBLAGE, pas du protocole : la droite émet ses
 * coordonnées physiques et n'a pas à savoir où elle est posée. La conversion
 * appartient donc au maître, et le drapeau vient de son board.h
 * (BOARD_REMOTE_COLS_MIRRORED). */
static inline uint8_t half_col_to_keymap(uint8_t col, uint8_t cols, bool miroir)
{
    return miroir ? (uint8_t)(2u * cols - 1u - col)
                  : (uint8_t)(col + cols);
}

/* ── Cadence : quand la moitié droite doit-elle émettre ? ───────────────────
 *
 * Testée host dans test/test_half_tx_cadence.c.
 *
 * Deux règles écrites séparément se contredisaient : la droite n'émettait que
 * sur CHANGEMENT (prémisse §2.3 du design, et la seule qui rende R1 tenable),
 * tandis que la gauche RELÂCHE après un silence. Maintenir une touche ne
 * produit aucun changement, donc aucune trame — et la gauche relâchait une
 * touche pourtant enfoncée. Pas de répétition, et les modificateurs de la
 * droite lâchaient en pleine frappe.
 *
 * La règle correcte distingue le REPOS de l'INACTIVITÉ : muet quand rien n'est
 * enfoncé, rafraîchi tant que quelque chose l'est. Le repos ne coûte toujours
 * rien, mais un maintien est réaffirmé avant que la gauche ne puisse en douter.
 *
 * ⚠ Les deux constantes sont liées : il faut qu'AU MOINS un rafraîchissement
 * puisse se perdre sans que le délai tombe, sinon un seul paquet manqué relâche
 * une touche tenue. Le test le vérifie. */
#define HALF_TX_REFRESH_MS   100u
/* 400 ms, et non 250 : à 100 ms de rafraîchissement, la marge n'était que d'un
 * seul paquet. Il en faut désormais quatre consécutifs pour relâcher à tort.
 * Une moitié réellement morte se déverrouille toujours en moins d'une
 * demi-seconde, ce qui reste imperceptible. */
#define HALF_LINK_TIMEOUT_MS 400u

static inline bool half_tx_doit_emettre(bool change, bool tenu,
                                        uint32_t now_ms, uint32_t dernier_ms,
                                        uint32_t periode_ms)
{
    if (change) return true;
    if (!tenu)  return false;   /* repos : silence, c'est la prémisse de R1 */
    /* Écart en arithmétique non signée : le compteur de ms déborde à ~49 jours
     * et une soustraction signée figerait l'émission ce jour-là. */
    return (uint32_t)(now_ms - dernier_ms) >= periode_ms;
}

/* Émetteur — moitié droite. Initialise la radio en PTX sur le canal du lien.
 * Retourne false si la radio ne répond pas. */
bool half_link_tx_init(void);

/* Émet l'état courant de la demi-matrice. Le numéro de séquence est géré en
 * interne. Retourne true si le paquet a été acquitté par la gauche. */
bool half_link_tx_matrix(const uint8_t *bitmap);

/* Applique la règle de cadence puis émet s'il y a lieu.
 *
 * Le callback de scan appelle (bitmap, true) : du neuf, à publier tout de
 * suite. La tâche de rafraîchissement appelle (NULL, false) : elle ne fournit
 * PAS d'état, elle réaffirme celui qui est déjà enregistré. C'est ce qui garde
 * un écrivain unique — sinon elle réécrirait un état lu au tour précédent et
 * ressusciterait un relâchement publié entre-temps. */
void half_link_tx_update(const uint8_t *bitmap, bool change);

/* Démarre la tâche qui réaffirme les maintiens. Sans elle, une touche tenue
 * plus de HALF_LINK_TIMEOUT_MS est relâchée à tort par la gauche : le callback
 * du pilote ne se déclenche que sur changement, un maintien ne produit donc
 * aucune trame. */
bool half_link_tx_refresh_start(void);

/* La touche (row, col) de la moitié DISTANTE est-elle enfoncée ? Coordonnées
 * locales à cette moitié — l'appelant décale vers les colonnes 7-13. Retourne
 * toujours false si le lien n'est pas compilé ou s'est tu. */
bool half_link_remote_pressed(uint8_t row, uint8_t col);

/* L'état distant a-t-il changé depuis le dernier appel ? Retourne true UNE
 * fois par changement, et consomme le drapeau. Permet à la tâche clavier de
 * n'agir que sur du nouveau, sans jamais interférer avec le chemin local qui a
 * sa propre émission. */
bool half_link_remote_changed(void);

/* Émet une trame vers le dongle SANS cesser d'écouter la droite : excursion
 * PRX→PTX→PRX sur la radio du lien, qui revient d'elle-même sur
 * RF_CH_HALF_LINK. C'est le seul chemin d'émission autorisé quand HALF_LINK_RX
 * est actif — une moitié n'a qu'une puce, et deux modules qui l'initialisent
 * chacun de leur côté se sont déjà écrasés trois fois. */
bool half_link_excursion_tx(uint8_t canal, const uint8_t addr[5],
                            const uint8_t *payload, uint8_t len);

/* Récepteur — moitié gauche. Initialise la radio en PRX et démarre la tâche
 * d'écoute, qui journalise chaque matrice reçue. */
bool half_link_rx_start(void);
