#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "keyboard_config.h"

#ifndef MATRIX_IRQ_ENABLED
#define MATRIX_IRQ_ENABLED 1
#endif

/* Sentinel value for "no key at this position" */
#define INVALID_KEY_POS  0xFF


/* Le dongle n'a pas de matrice : ni keymap, ni statistiques par position, ni
 * état de matrice. Déclarer ces symboles chez lui obligeait sa carte à inventer
 * des dimensions — c'est ce que board.h ne fait plus. */
#if !CONFIG_KASE_NO_KEYMAP_ENGINE
extern uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t (*matrix_states[])[MATRIX_ROWS][MATRIX_COLS];
#endif
extern uint8_t keycodes[6];
extern uint8_t current_press_row[6];
extern uint8_t current_press_col[6];
extern uint8_t current_press_stat[6];

#if CONFIG_KASE_HALF_LINK_RX || (CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS)
/* Rejoue la fusion des touches reçues de la moitié distante. Idempotente :
 * la tâche clavier l'appelle à chaque cycle, car le callback de scan ne tourne
 * que sur activité locale. Source distante : half_link (maître) ou kbd_relay
 * (fusion, droite réémise par le dongle en mode USB). */
void matrix_apply_remote(void);
#endif
extern volatile uint8_t stat_matrix_changed; /* written in ISR, read in task */
extern uint8_t last_layer;
/* current_layout: declared in keyboard_config.h */
extern volatile uint8_t is_layer_changed;    /* written in ISR, read in display task */
extern volatile uint32_t last_activity_time_ms;


/*
 * @brief deinitialize rtc pins
 */
void rtc_matrix_deinit(void);

/* Note: rtc_matrix_setup and IRQ setup/deinit were removed —
 * matrix setup/deinit are handled by the keyboard_button shim.
 */

/*
 * @brief initialize matrix
 */
void matrix_setup(void);

/*
 * Matrix scanning is handled asynchronously by the keyboard_button component.
 * The legacy `scan_matrix` and `scan_matrix_full_once` functions were removed.
 */

/* IRQ setup/deinit removed (handled by keyboard_button) */

void layer_changed(void);

uint32_t get_last_activity_time_ms(void);

void rtc_matrix_deinit(void);
void matrix_setup(void);

/* Light-sleep matrix key-wake (V2D wireless): arm/disarm matrix GPIOs as a
 * keypress wake source around esp_light_sleep_start(). */
void matrix_arm_key_wake(void);
/* Tamponne l'activité à maintenant — à appeler au réveil, AVANT que la
 * boucle clavier ne réévalue l'inactivité. */
void matrix_mark_activity(void);
/* Balaie la matrice une fois à la main et publie ce qu'un callback aurait
 * publié. À appeler dès le retour de light sleep, AVANT matrix_setup() : la
 * touche qui a réveillé la carte est enfoncée à cet instant, plus tard elle ne
 * l'est peut-être plus. */
void matrix_wake_capture(void);
/* ~10 ms après matrix_setup() : si le pilote n'a rien signalé alors qu'une
 * touche avait été capturée, elle a été relâchée entre-temps — publie le
 * relâchement et retourne true (l'appelant l'émet). */
bool matrix_wake_reconcile(void);
void matrix_disarm_key_wake(void);

/* Matrix test mode: when true, scan callback sends key events
   via CDC binary protocol instead of filling HID reports */
extern volatile bool matrix_test_mode;


