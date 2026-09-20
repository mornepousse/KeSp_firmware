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

/* ── Fusion de DEUX demi-matrices au dongle (brick « dongle fusion ») ────────
 *
 * Testée host dans test/test_fuse_halves.c. Design :
 * docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md.
 *
 * En mode sans fil, les deux moitiés émettent leur matrice BRUTE ; le dongle
 * fusionne et fait tourner le moteur. Aucune moitié n'est « locale » ici,
 * contrairement à matrix_apply_remote : on reçoit deux bitmaps et on produit les
 * positions (row, colonne keymap) que le moteur indexe. Gauche = colonnes 0..cols-1
 * en direct ; droite = colonnes hautes via half_col_to_keymap (miroir du PCB).
 *
 * Pure : pas d'I/O, pas d'état global. Écrit dans out_row/out_col, rend le nombre
 * de touches, borné à max. */
static inline uint8_t fuse_halves(const uint8_t *left_bm, const uint8_t *right_bm,
                                  uint8_t cols, bool right_mirror,
                                  uint8_t *out_row, uint8_t *out_col, uint8_t max)
{
    uint8_t n = 0;
    for (uint8_t r = 0; r < RF_HALF_ROWS && n < max; r++)
        for (uint8_t c = 0; c < cols && n < max; c++)
            if (rf_bitmap_get(left_bm, r, c)) {
                out_row[n] = r; out_col[n] = c; n++;   /* gauche : colonne directe */
            }
    for (uint8_t r = 0; r < RF_HALF_ROWS && n < max; r++)
        for (uint8_t c = 0; c < cols && n < max; c++)
            if (rf_bitmap_get(right_bm, r, c)) {
                out_row[n] = r;
                out_col[n] = half_col_to_keymap(c, cols, right_mirror);
                n++;
            }
    return n;
}

/* ── État de fusion au dongle : DEUX demi-matrices routées par identité ──────
 *
 * Testée host dans test/test_fusion_state.c.
 *
 * Sur le maître, une seule moitié était distante (half_state_t unique) : l'autre
 * était sa propre matrice locale. Au dongle il n'y a AUCUNE matrice locale — les
 * deux moitiés sont distantes et arrivent sur le même slot clavier, distinguées
 * par l'octet d'identité de PKT_TYPE_MATRIX. On tient donc deux demi-états, on
 * route chaque trame vers le bon, et on expire chacun indépendamment : une
 * moitié muette ne doit pas relâcher ce que l'autre tient (même prudence que
 * rf_slot.h côté supervision).
 *
 * Pure : pas d'I/O, pas d'état global. Réutilise half_state_* et fuse_halves. */
typedef struct {
    half_state_t left;
    half_state_t right;
} fusion_state_t;

/* Range une trame décodée dans le demi-état de sa moitié. Retourne false si
 * l'identité n'est ni gauche ni droite — une trame corrompue ne doit rien
 * écrire. */
static inline bool fusion_apply(fusion_state_t *fs, const rf_matrix_t *m,
                                uint32_t now_ms)
{
    if (m->half == RF_HALF_LEFT)  { half_state_recu(&fs->left,  m->bitmap, now_ms); return true; }
    if (m->half == RF_HALF_RIGHT) { half_state_recu(&fs->right, m->bitmap, now_ms); return true; }
    return false;
}

/* Expire les deux demi-états. Retourne true si au moins un vient de tomber en
 * silence (signal « recalcule le rapport »). Les DEUX sont évalués — pas de
 * court-circuit —, sinon une moitié ne serait jamais expirée quand l'autre
 * l'est déjà. Comme half_state_timeout, ne signale qu'une fois par silence. */
static inline bool fusion_timeout(fusion_state_t *fs, uint32_t now_ms,
                                  uint32_t delai_ms)
{
    bool l = half_state_timeout(&fs->left,  now_ms, delai_ms);
    bool r = half_state_timeout(&fs->right, now_ms, delai_ms);
    return l || r;
}

/* Produit la liste fusionnée (row, colonne keymap) que le moteur indexera.
 * Gauche en colonnes directes, droite en colonnes hautes via le miroir du PCB. */
static inline uint8_t fusion_collect(const fusion_state_t *fs, uint8_t cols,
                                     bool right_mirror, uint8_t *out_row,
                                     uint8_t *out_col, uint8_t max)
{
    return fuse_halves(fs->left.bitmap, fs->right.bitmap, cols, right_mirror,
                       out_row, out_col, max);
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

/* ── Réparation bornée après changement ─────────────────────────────────────
 *
 * Testée host dans test/test_half_tx_repeat.c.
 *
 * half_tx_doit_emettre ne rejoue l'état que pour un MAINTIEN. Une trame de
 * changement refusée par l'ESB (15 retransmissions épuisées, ~1 % au banc)
 * n'avait donc qu'une chance : un appui bref perdu n'était jamais réparé (banc
 * 2026-09-13, même cause que la gauche). Ici un changement ARME un nombre borné
 * de répétitions, consommées une par tick de la tâche de rafraîchissement
 * (20 ms) même si rien n'est tenu ; puis retour à la règle de maintien. Le
 * repos jamais armé reste muet — la prémisse §2.3 (R1) tient. Le dongle
 * déduplique par contenu : les répétitions lui sont gratuites. */
#define HALF_TX_REPEATS 3u   /* 3 × 20 ms ≈ la fenêtre 5 × 10 ms de la gauche */

typedef struct { uint8_t restantes; } half_tx_repeat_t;

static inline bool half_tx_doit_emettre_repare(half_tx_repeat_t *rep, bool change,
                                               bool tenu, uint32_t now_ms,
                                               uint32_t dernier_ms, uint32_t periode_ms)
{
    if (change) { rep->restantes = HALF_TX_REPEATS; return true; }
    if (rep->restantes) { rep->restantes--; return true; }
    return half_tx_doit_emettre(false, tenu, now_ms, dernier_ms, periode_ms);
}

/* ── Cible d'émission de la droite : dongle ↔ gauche directe (fusion) ────────
 *
 * En fusion, la droite parle au SLOT CLAVIER DU DONGLE (KaSe.01), qui fait
 * tourner le moteur. Mais si l'utilisateur DÉBRANCHE le dongle et tape sur la
 * gauche en USB, la droite n'a plus personne : ses trames au dongle ne sont plus
 * acquittées, et la gauche-USB — qui écoute pourtant déjà KaSe.03 pour la
 * réémission du dongle — n'entend plus rien. Repli : sur N envois consécutifs
 * SANS ACK, la droite RÉARME sa puce et BASCULE vers l'autre auditeur. La
 * gauche-USB écoute déjà KaSe.03 ; la droite y émet alors le HEARTBEAT
 * pré-fusion que kbd_relay décode (le MATRIX de fusion, lui, n'est décodé que
 * par le dongle). Si le dongle revient — ou si l'USB gauche part et la gauche
 * cesse d'écouter — la cible courante cesse d'acquitter et on rebascule :
 * auto-cicatrisant, jamais deux cibles à la fois, donc jamais de double frappe.
 *
 * ⚠ Muet au repos : la droite n'émet que sur changement/maintien (cf.
 * half_tx_doit_emettre), donc le compteur n'avance QUE quand il y a quelque
 * chose à router. La bascule réarme aussi une puce figée (clone nRF24) en
 * réécrivant la config PTX — elle subsume l'ancien chien de garde « réarmer sur
 * place ».
 *
 * Testée host dans test/test_half_tx_target.c. */
typedef enum {
    HALF_TX_TO_DONGLE = 0,   /* MATRIX vers KaSe.01 — le dongle fusionne et tape */
    HALF_TX_TO_LEFT   = 1,   /* HEARTBEAT vers KaSe.03 — la gauche-USB tape */
} half_tx_target_t;

typedef struct {
    half_tx_target_t cible;    /* auditeur courant */
    uint16_t         sans_ack; /* envois consécutifs non acquittés */
} half_tx_fsm_t;

/* 8 envois : chacun retransmis jusqu'à 15 fois en matériel (ESB), donc ~120
 * tentatives HW perdues avant de conclure « cet auditeur a disparu » — assez
 * confiant pour ne pas basculer sur un glitch, assez court pour que la bascule
 * se sente en <1 s sur un maintien (rafraîchi ~10/s). */
#define HALF_TX_SWITCH_FAILS 8u

/* Un envoi vient d'avoir lieu (ack = acquitté). Met à jour l'état et renvoie
 * true s'il faut RÉARMER la puce PTX sur la (nouvelle) cible `s->cible`. */
static inline bool half_tx_target_step(half_tx_fsm_t *s, bool ack, uint16_t seuil)
{
    if (ack) { s->sans_ack = 0; return false; }
    if (++s->sans_ack >= seuil) {
        s->sans_ack = 0;
        s->cible = (s->cible == HALF_TX_TO_DONGLE) ? HALF_TX_TO_LEFT
                                                   : HALF_TX_TO_DONGLE;
        return true;
    }
    return false;
}

/* Émetteur — moitié droite. Initialise la radio en PTX sur le canal du lien.
 * Retourne false si la radio ne répond pas. */
bool half_link_tx_init(void);

/* Émet l'état courant de la demi-matrice. Le numéro de séquence est géré en
 * interne. Retourne true si le paquet a été acquitté par la gauche. */
bool half_link_tx_matrix(const uint8_t *bitmap);
/* Même chose pour une RÉPÉTITION : `encore_valide` évalué sous le verrou radio,
 * rien ne part si l'état est périmé (voir radio_emettre). */
#include "radio_owner.h"
bool half_link_tx_matrix_si(const uint8_t *bitmap, radio_valide_cb_t encore_valide, void *ctx);
#if CONFIG_KASE_BATT_SENSE
/* Jauge : un STATUS (tension, état de charge, identité DROITE) vers la cible
 * courante. Appelé par la tâche de rafraîchissement toutes les RF_BATT_PERIOD_MS
 * et au réveil. Retourne l'ACK. */
bool half_link_tx_status(void);
/* Écran : la cible courante est le dongle et la DERNIÈRE émission a été
 * acquittée (collant : une moitié est muette au repos ; en repli vers la
 * gauche, faux). */
bool half_link_tx_dongle_vu(void);
#endif

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


