#pragma once
/* keymap_pull — tirage de la keymap du dongle par ACK payload (gauche, fusion).
 *
 * La gauche PILOTE : quand l'ACK d'une de ses émissions porte une BALISE (une
 * keymap d'empreinte ≠ la sienne l'attend), elle entame un pull et demande le
 * prochain chunk manquant (SYNC_REQ) toutes les 100 ms ; chaque chunk arrive
 * dans l'ACK de l'émission suivante. Quand les 40 sont là, la keymap est
 * copiée et enregistrée en NVS ; le STATUS suivant annonce la nouvelle
 * empreinte et le dongle coupe la balise. Un veto de veille tient pendant le
 * tirage. Logique pure dans keymap_sync.h (testée) ; ici le portage sur le
 * relais de la gauche. Extrait de kbd_relay_tx.c le 2026-09-19. */
#include <stdbool.h>
#include <stdint.h>

/* À appeler avec la charge de CHAQUE ACK reçu (balise ou chunk), depuis le
 * chemin d'émission — sous le verrou du propriétaire de la radio. */
void keymap_pull_on_ack(const uint8_t *ack, uint8_t n);

/* Un tirage est-il en cours ou une keymap à enregistrer ? (cadence rapide du
 * relais, priorité aux maintiens) */
bool keymap_pull_en_cours(void);

/* Tick du relais, APRÈS la réaffirmation des maintiens : enregistre en NVS
 * quand 40/40 sont là (copie sous le verrou radio, NVS hors verrou), puis
 * émet un REQ au plus toutes les 100 ms tant qu'on tire, via `emettre`.
 * Retourne true si un REQ est parti (le relais a fini son tick). */
bool keymap_pull_tick(void (*emettre)(const uint8_t *, uint8_t));
