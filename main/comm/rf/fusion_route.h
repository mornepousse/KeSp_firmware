#pragma once
#include <stdbool.h>

/* Routage de la fusion — quel moteur est actif (règle 3 du design deux-moteurs).
 *
 * Un seul moteur tourne à la fois, choisi par la présence d'un hôte USB à la
 * GAUCHE :
 *   - USB présent → la gauche est sur secteur, elle peut écouter la droite,
 *     fait tourner SON moteur et sort le HID par son propre USB ; le dongle se
 *     tait et se contente de réémettre la demi-matrice de la droite à la gauche.
 *   - pas d'USB → la gauche émet sa matrice BRUTE au dongle (elle n'écoute plus,
 *     autonomie) ; le dongle fusionne et tape.
 *
 * Ces prédicats sont la source unique de « qui fait quoi ». Ils prennent un
 * booléen (USB présent ?) plutôt que le type de route ESP, pour rester testables
 * host et découplés de usb_presence.h. L'appelant convertit
 * (kbd_active_route()==KBD_OUT_USB).
 *
 * Testés host (test/test_fusion_route.c), invariant central compris : gauche et
 * dongle ne tapent jamais en même temps.
 * Design : docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md
 */

/* Côté GAUCHE. */
static inline bool fusion_left_types_local(bool usb_present) { return usb_present; }
static inline bool fusion_left_emits_raw(bool usb_present)   { return !usb_present; }

/* Côté DONGLE, selon le mode USB de la gauche (qu'elle lui annonce — annonce RF
 * à venir ; par défaut la gauche est supposée sans-fil et le dongle tape). */
static inline bool fusion_dongle_types(bool left_usb_present)   { return !left_usb_present; }
static inline bool fusion_dongle_reemits(bool left_usb_present) { return left_usb_present; }
