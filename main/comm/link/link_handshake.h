/* Poignée de main du lien inter-moitiés Niphargus.
 *
 * LINK_5V_EN pilote un load switch SiP32431 avec un pull-down 100 k : le 5 V est
 * MORT par défaut. Émetteur et récepteur doivent tous deux fermer leur switch
 * pour qu'un courant passe, donc un branchement à chaud ne peut pas produire
 * d'étincelle. Une moitié à batterie vide n'est pas réveillable par le TRRS —
 * assumé au design matériel.
 *
 * ── L'invariant de sûreté ────────────────────────────────────────────────────
 *
 * en_5v ne passe à vrai qu'après un ÉCHANGE VÉRIFIÉ avec le pair : soit on a
 * sondé et reçu un ACK, soit on a été sondé et on a répondu. Le temps qui passe,
 * la seule présence de l'USB, ou un événement inattendu ne le lèvent jamais.
 *
 * Logique pure : l'horloge et les événements sont passés en argument, aucune
 * GPIO n'est touchée ici. L'appelant traduit les actions en gpio_set_level().
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LINK_HS_PROBE_TIMEOUT_MS  200   /* attente d'un ACK après la sonde */
#define LINK_HS_PEER_TIMEOUT_MS   500   /* silence du pair toléré, lien établi */
/* Intervalle entre deux sondes en IDLE tant que l'USB est présent. USB_PRESENT
 * est un événement de FRONT (pas un état relu ailleurs) : sans re-sonde
 * périodique, une seule sonde perdue au boot condamnerait le lien jusqu'au
 * débranchement/rebranchement du câble. Même ordre de grandeur que
 * LINK_HS_PROBE_TIMEOUT_MS pour ne pas spammer la ligne. */
#define LINK_HS_REPROBE_INTERVAL_MS  300
/* Entretien du lien en UP. Constaté au banc le 2026-09-11, à la première
 * fermeture du 5 V : le lien montait puis retombait 500 ms plus tard. En UP
 * l'émetteur ne sondait plus, le récepteur n'avait donc plus rien à acquitter,
 * et LINK_HS_PEER_TIMEOUT_MS expirait. Le design comptait sur les trames
 * MATRIX du chemin filaire pour entretenir le lien — chemin qui n'existe pas,
 * les moitiés parlent radio. Tant qu'on a du courant à donner, on le redit :
 * deux entretiens tiennent dans le délai d'expiration, une sonde perdue ne
 * rouvre pas le switch. */
#define LINK_HS_KEEPALIVE_MS         200

typedef enum {
    LINK_HS_IDLE = 0,   /* 5 V mort, rien en cours */
    LINK_HS_PROBING,    /* sonde envoyée, on attend l'ACK */
    LINK_HS_UP,         /* pair reconnu, 5 V fermé */
} link_hs_state_t;

typedef enum {
    LINK_HS_EV_TICK = 0,     /* passage du temps, rien d'autre */
    LINK_HS_EV_USB_PRESENT,  /* le câble hôte vient d'apparaître */
    LINK_HS_EV_USB_GONE,     /* le câble hôte a disparu */
    LINK_HS_EV_PEER_ACK,     /* la moitié d'en face a répondu à la sonde */
    LINK_HS_EV_PEER_FRAME,   /* trame valide reçue du pair (garde le lien vivant) */
    LINK_HS_EV_PROBED,       /* le pair NOUS sonde (trame PROBE reçue et vérifiée) */
} link_hs_event_t;

typedef enum {
    LINK_HS_ACT_NONE = 0,
    LINK_HS_ACT_SEND_PROBE,   /* émettre la sonde « t'es bien ma moitié ? » */
    LINK_HS_ACT_ENABLE_5V,    /* fermer le load switch */
    LINK_HS_ACT_DISABLE_5V,   /* rouvrir le load switch */
    /* Répondre à la sonde ET fermer notre switch. Les deux vont ensemble : le
     * contrat matériel exige que les DEUX moitiés ferment le leur pour qu'un
     * courant passe, donc répondre sans fermer ne servirait à rien. Fermer un
     * switch déjà fermé est sans effet, l'appelant n'a pas à s'en soucier. */
    LINK_HS_ACT_ACK_AND_ENABLE_5V,
} link_hs_action_t;

typedef struct {
    link_hs_state_t state;
    uint32_t        since_ms;    /* entrée dans l'état courant */
    uint32_t        last_peer_ms;/* dernier signe de vie du pair */
    uint32_t        last_probe_ms;/* dernière sonde émise (entretien en UP) */
    bool            usb;         /* câble hôte présent */
    bool            en_5v;       /* état commandé du load switch */
} link_hs_t;

static inline void link_hs_init(link_hs_t *h)
{
    h->state = LINK_HS_IDLE;
    h->since_ms = 0;
    h->last_peer_ms = 0;
    h->last_probe_ms = 0;
    h->usb = false;
    h->en_5v = false;
}

static inline bool link_hs_5v_enabled(const link_hs_t *h) { return h->en_5v; }

static inline link_hs_action_t link_hs_step(link_hs_t *h, link_hs_event_t ev,
                                            uint32_t now_ms)
{
    /* Perte de l'USB : on coupe partout, immédiatement. */
    if (ev == LINK_HS_EV_USB_GONE) {
        h->usb = false;
        if (h->en_5v) {
            h->en_5v = false;
            h->state = LINK_HS_IDLE;
            h->since_ms = now_ms;
            return LINK_HS_ACT_DISABLE_5V;
        }
        h->state = LINK_HS_IDLE;
        h->since_ms = now_ms;
        return LINK_HS_ACT_NONE;
    }

    if (ev == LINK_HS_EV_USB_PRESENT) h->usb = true;
    if (ev == LINK_HS_EV_PEER_ACK || ev == LINK_HS_EV_PEER_FRAME ||
        ev == LINK_HS_EV_PROBED)
        h->last_peer_ms = now_ms;

    /* Le pair nous sonde : c'est un échange vérifié (la trame PROBE a passé son
     * CRC avant d'arriver ici), donc on répond et on ferme notre côté. Traité
     * avant le switch parce que ça vaut depuis n'importe quel état — y compris
     * PROBING, quand les deux moitiés se sondent en même temps et resteraient
     * sinon bloquées à s'attendre.
     *
     * Décision assumée (2026-08-19) : la sondée ferme TOUJOURS, même quand elle
     * a son propre USB. Les deux rails 5 V se retrouvent alors reliés par le
     * jack quand les deux câbles sont branchés ; la limitation de courant et le
     * soft-start du load switch l'encaissent, mais ce n'est pas l'usage prévu —
     * à vérifier au banc. L'alternative écartée était de ne pas fermer quand on
     * est soi-même alimenté. */
    if (ev == LINK_HS_EV_PROBED) {
        h->state = LINK_HS_UP;
        h->since_ms = now_ms;
        h->en_5v = true;
        return LINK_HS_ACT_ACK_AND_ENABLE_5V;
    }

    switch (h->state) {
    case LINK_HS_IDLE:
        /* On ne sonde que si on a du courant à donner. */
        if (ev == LINK_HS_EV_USB_PRESENT) {
            h->state = LINK_HS_PROBING;
            h->since_ms = now_ms;
            h->last_probe_ms = now_ms;
            return LINK_HS_ACT_SEND_PROBE;
        }
        /* USB_PRESENT est un événement de front : si une sonde s'est perdue
         * (timeout de PROBING nous a ramenés ici avec h->usb toujours vrai),
         * rien ne le relèvera jamais tout seul. Re-sonder périodiquement tant
         * que l'USB reste là — sans ça, un seul ACK perdu au boot condamne le
         * lien jusqu'au débranchement du câble. Ne PAS lever en_5v ici : ceci
         * ne fait que renvoyer la sonde, la propriété de sûreté (5V seulement
         * après PEER_ACK) est inchangée. */
        if (ev == LINK_HS_EV_TICK && h->usb &&
            (uint32_t)(now_ms - h->since_ms) >= LINK_HS_REPROBE_INTERVAL_MS) {
            h->state = LINK_HS_PROBING;
            h->since_ms = now_ms;
            h->last_probe_ms = now_ms;
            return LINK_HS_ACT_SEND_PROBE;
        }
        return LINK_HS_ACT_NONE;

    case LINK_HS_PROBING:
        if (ev == LINK_HS_EV_PEER_ACK) {
            h->state = LINK_HS_UP;
            h->since_ms = now_ms;
            h->en_5v = true;
            return LINK_HS_ACT_ENABLE_5V;
        }
        if ((uint32_t)(now_ms - h->since_ms) >= LINK_HS_PROBE_TIMEOUT_MS) {
            h->state = LINK_HS_IDLE;
            h->since_ms = now_ms;
        }
        return LINK_HS_ACT_NONE;

    case LINK_HS_UP:
        if ((uint32_t)(now_ms - h->last_peer_ms) >= LINK_HS_PEER_TIMEOUT_MS) {
            h->en_5v = false;
            h->state = LINK_HS_IDLE;
            h->since_ms = now_ms;
            return LINK_HS_ACT_DISABLE_5V;
        }
        /* Entretien : celui qui a du courant à donner le redit périodiquement.
         * Le pair répond par un ACK, ce qui rafraîchit last_peer_ms des deux
         * côtés (PROBED chez lui, PEER_ACK chez nous). Le récepteur, sans USB,
         * ne sonde jamais — il n'a rien à donner. */
        if (ev == LINK_HS_EV_TICK && h->usb &&
            (uint32_t)(now_ms - h->last_probe_ms) >= LINK_HS_KEEPALIVE_MS) {
            h->last_probe_ms = now_ms;
            return LINK_HS_ACT_SEND_PROBE;
        }
        return LINK_HS_ACT_NONE;

    default:
        /* h->state hors énumération : contrat d'appel violé (struct sur pile
         * non passée par link_hs_init(), mémoire arbitraire). Ce module est le
         * dernier rempart avant le GPIO du load switch, et link_hs_5v_enabled()
         * est public — un appelant qui l'interroge dans cet état ne doit
         * jamais lire un `true` de poubelle alors qu'aucun ACT_ENABLE_5V n'a
         * été émis. Ici, la propriété « 5 V éteint sauf preuve du contraire »
         * prime sur le diagnostic : on retombe du côté sûr plutôt que de
         * préserver une valeur inconnue.
         */
        h->state = LINK_HS_IDLE;
        h->en_5v = false;
        return LINK_HS_ACT_NONE;
    }
    return LINK_HS_ACT_NONE;
}
