#pragma once
/* Sleep task for the Niphargus halves — ONE task, identical on left and
 * right (power/veille_task.c).
 *
 * It owns: the inactivity clock (matrix_scan), the veto registry
 * (veille_veto.h), the sleep/wake hooks, the bench heartbeat.
 * Modules no longer need to know about sleep, nor sleep about the modules:
 *   - a module with a reason to prevent sleep POSTS A VETO
 *     (veille_veto) and lifts it once the reason is gone;
 *   - a module with something to put to sleep REGISTERS A HOOK before the
 *     task starts; hooks are called in registration order on sleep
 *     and in REVERSE order on wake — the radio, registered first,
 *     is therefore up BEFORE the capture of the key that woke the board
 *     (it transmits).
 * Until 2026-09-18 the decision was made in two module tasks
 * (left: keyboard loop, right: radio refresh) with two rules
 * and two heartbeats. */
#include <stdbool.h>
#include "veille_veto.h"

typedef struct {
    const char *nom;              /* "radio", "ecran", "jauge" — for the log */
    void (*dormir)(void);         /* called BEFORE sleep, in registration order */
    void (*reveiller)(void);      /* called AFTER wake, in REVERSE order */
} veille_hook_t;
#define VEILLE_HOOKS_MAX 4

void veille_hook_enregistrer(const veille_hook_t *h);   /* before veille_task_start */
void veille_veto(veille_veto_t quoi, bool on);           /* from any task */
void veille_task_start(void);                            /* a "veille" task, prio 3, core 0 */

/* For veille.c: the sleep/wake sequence calls the hooks. */
void veille_hooks_dormir(void);
void veille_hooks_reveiller(void);

/* Role suffix of the heartbeat (" route=RF relais=actif" on the left,
 * " lien=0 batt=39 dV" on the right). Default: "" empty. */
const char *veille_hb_suffixe(void);
