#pragma once
/* Tâche de veille des moitiés Niphargus — UNE tâche, identique à gauche et à
 * droite (power/veille_task.c).
 *
 * Elle possède : l'horloge d'inactivité (matrix_scan), le registre de vetos
 * (veille_veto.h), les hooks sommeil/réveil, le battement de coeur de banc.
 * Les modules n'ont plus à connaître la veille, ni la veille les modules :
 *   - un module qui a une raison d'empêcher la veille POSE UN VETO
 *     (veille_veto) et le lève quand la raison disparaît ;
 *   - un module qui a quelque chose à endormir ENREGISTRE UN HOOK avant le
 *     démarrage de la tâche ; les hooks sont appelés dans l'ordre au sommeil
 *     et en ordre INVERSE au réveil — la radio, enregistrée la première,
 *     est donc debout AVANT la capture de la touche qui a réveillé la carte
 *     (elle émet).
 * Jusqu'au 2026-09-18 la décision était prise dans deux tâches de modules
 * (gauche : boucle clavier, droite : rafraîchissement radio) avec deux règles
 * et deux battements de coeur. */
#include <stdbool.h>
#include "veille_veto.h"

typedef struct {
    const char *nom;              /* "radio", "ecran", "jauge" — pour le journal */
    void (*dormir)(void);         /* appelé AVANT le sommeil, dans l'ordre d'enregistrement */
    void (*reveiller)(void);      /* appelé APRÈS le réveil, dans l'ordre INVERSE */
} veille_hook_t;
#define VEILLE_HOOKS_MAX 4

void veille_hook_enregistrer(const veille_hook_t *h);   /* avant veille_task_start */
void veille_veto(veille_veto_t quoi, bool on);           /* depuis n'importe quelle tâche */
void veille_task_start(void);                            /* une tâche "veille", prio 3, coeur 0 */

/* Pour veille.c : la séquence sommeil/réveil appelle les hooks. */
void veille_hooks_dormir(void);
void veille_hooks_reveiller(void);

/* Suffixe de rôle du battement de coeur (" route=RF relais=actif" à gauche,
 * " lien=0 batt=39 dV" à droite). Faible : "" par défaut. */
const char *veille_hb_suffixe(void);
