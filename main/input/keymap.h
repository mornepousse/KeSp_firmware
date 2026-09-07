#pragma once
#include <stddef.h>   /* size_t — nécessaire pour les signatures de fonctions */
#include <stdbool.h>  /* bool — retour des save_* (échec de persistance NVS) */
#include "keyboard_config.h"
/* Ni le dongle ni la souris n'ont de matrice : ni keymap, ni statistiques par
 * position, ni état de matrice. Déclarer ces symboles chez eux obligerait leur
 * carte à inventer des dimensions — c'est ce que leurs board.h ne font pas. */
#if !CONFIG_KASE_NO_KEYMAP_ENGINE
/* KEYMAP_COLS, pas MATRIX_COLS : sur la moitié maître d'un split la keymap
 * couvre les deux moitiés alors que le balayage n'en couvre qu'une. */
extern uint16_t keymaps[][MATRIX_ROWS][KEYMAP_COLS];
extern char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH];
#endif

/* Macro step: one keypress in a sequence */
#define MACRO_MAX_STEPS 24
typedef struct {
  uint8_t keycode;   /* HID keycode (0 = end, 0xFF = delay marker) */
  uint8_t modifier;  /* modifier mask, or delay in 10ms units if keycode=0xFF */
} macro_step_t;

#define MACRO_DELAY_MARKER 0xFF

/* Macro definition */
#define MAX_MACRO_NAME_LENGTH 16
#define MAX_MACROS 20
typedef struct {
  char name[MAX_MACRO_NAME_LENGTH];
  macro_step_t steps[MACRO_MAX_STEPS]; /* sequence of keypress/delay steps */
  uint8_t keys[6];                     /* legacy: simultaneous keys (backward compat) */
  uint16_t key_definition;
} macro_t;

bool save_keymaps(uint16_t *data, size_t size_bytes);   /* true = persisté OK */
/* Taille du blob « keymaps » en NVS — LA source unique, des deux côtés.
 *
 * Elle se calcule sur KEYMAP_COLS, PAS sur MATRIX_COLS : sur le maître d'un
 * split, la keymap couvre les deux moitiés (14 colonnes) alors que la matrice
 * locale n'en balaie que 7. Les deux formules coexistaient — écriture en
 * KEYMAP_COLS (1120 octets), lecture en MATRIX_COLS (560) — et la garde de
 * taille de load_keymaps rejetait donc le blob à CHAQUE démarrage, en gardant
 * les valeurs d'usine. Le remappage de l'utilisateur était écrit, jamais relu,
 * et rien ne le signalait. Constaté au banc le 2026-09-07.
 *
 * Verrouillé par test/test_keymap_blob_size.c. */
#define KEYMAP_BLOB_BYTES \
    ((size_t)LAYERS * MATRIX_ROWS * KEYMAP_COLS * sizeof(uint16_t))

void load_keymaps(uint16_t *data, size_t size_bytes);
void keymap_init_nvs(void);
bool save_layout_names(char names[][MAX_LAYOUT_NAME_LENGTH], size_t layer_count);
void load_layout_names(char names[][MAX_LAYOUT_NAME_LENGTH], size_t layer_count);
bool save_macros(macro_t *macros, size_t count);
void load_macros(macro_t *macros, size_t count);
void recalc_macros_count(void);

extern macro_t macros_list[MAX_MACROS];
extern size_t macros_count;
