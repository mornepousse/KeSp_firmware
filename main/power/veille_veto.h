#pragma once
/* Vetos de veille — logique pure, testée host (test/test_veille_veto.c).
 *
 * Un veto est un verrou NOMMÉ, sur le modèle des verrous esp_pm : le module
 * qui a une raison d'empêcher la veille la déclare, la veille n'interroge
 * personne. Jusqu'au 2026-09-18 la décision était prise dans deux tâches avec
 * deux règles (gauche : usb || lien ; droite : lien) — chaque nouveau blocage
 * exigeait de retrouver les deux endroits.
 *
 * C'est un ÉTAT par nom, pas un compteur : chaque module ne pose que le sien
 * et le lève quand sa raison disparaît ; poser deux fois puis lever une fois
 * = levé. Le câblage FreeRTOS (section critique, tâche) est dans
 * veille_task.c ; ici rien que la table de vérité. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    VEILLE_VETO_USB  = 1u << 0,   /* hôte USB prêt (gauche : clavier HID, l'hôte attend) */
    VEILLE_VETO_LIEN = 1u << 1,   /* 5 V du TRRS actif : une moitié charge l'autre */
    VEILLE_VETO_SYNC = 1u << 2,   /* tirage de keymap par ACK payload en cours */
    VEILLE_VETO_TEST = 1u << 3,   /* mode test matrice (CDC) */
    VEILLE_VETO_PAIR = 1u << 4,   /* appairage actif : radio_pair_round tient la puce ~150 ms par tour,
                                   * 30-40 s sans que personne ne tape — endormie, radio_sleep coupait
                                   * la puce sous la tâche d'appairage (revue 2026-09-20) */
} veille_veto_t;

typedef struct { uint32_t actifs; } veille_vetos_t;

static inline void veille_veto_poser(veille_vetos_t *v, veille_veto_t quoi, bool on)
{
    if (on) v->actifs |=  (uint32_t)quoi;
    else    v->actifs &= ~(uint32_t)quoi;
}

static inline bool veille_bloquee(const veille_vetos_t *v) { return v->actifs != 0; }

/* Noms des vetos actifs pour le battement de coeur : "usb+lien", "-" si
 * aucun. Borné à n octets (n ≥ 2), tronqué proprement au-delà — les cinq
 * tiennent dans les 24 octets du HB ("usb+lien+sync+test+pair" = 23). */
static inline const char *veille_vetos_str(const veille_vetos_t *v, char *out, size_t n)
{
    static const struct { veille_veto_t q; const char *nom; } noms[] = {
        { VEILLE_VETO_USB, "usb" }, { VEILLE_VETO_LIEN, "lien" },
        { VEILLE_VETO_SYNC, "sync" }, { VEILLE_VETO_TEST, "test" },
        { VEILLE_VETO_PAIR, "pair" },
    };
    if (n == 0) return "";
    out[0] = '\0';
    size_t len = 0;
    for (size_t i = 0; i < sizeof noms / sizeof noms[0]; i++) {
        if (!(v->actifs & (uint32_t)noms[i].q)) continue;
        const char *sep = len ? "+" : "";
        size_t need = strlen(sep) + strlen(noms[i].nom);
        if (len + need + 1 > n) break;
        memcpy(out + len, sep, strlen(sep)); len += strlen(sep);
        memcpy(out + len, noms[i].nom, strlen(noms[i].nom)); len += strlen(noms[i].nom);
        out[len] = '\0';
    }
    if (len == 0 && n >= 2) { out[0] = '-'; out[1] = '\0'; }
    return out;
}
