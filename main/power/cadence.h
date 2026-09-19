#pragma once
/* Cadences des moitiés Niphargus — UN lieu, une règle, une garde.
 *
 * La règle : au repos, aucune attente périodique sous CADENCE_REPOS_MIN_MS.
 * ESP-IDF ne tente le light sleep automatique (tickless) que si toutes les
 * tâches sont bloquées ≥ CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP ticks (3, à
 * 100 Hz = 30 ms). Une boucle à 10 ms laisse UN tick libre : le profil disait
 * « mode SLEEP 92 % » et light_sleep_counts restait à 0 (banc 2026-09-16, tâche
 * clavier). Un mode oisif n'est pas un sommeil, et rien ne le signale.
 *
 * La garde : chaque cadence de REPOS est suivie d'un CADENCE_REPOS_OK — une
 * valeur trop courte ne compile pas. Les cadences ACTIVES (touche tenue,
 * réparation bornée, poignée de main) restent ≤ 20 ms : c'est ce qui tient
 * les réaffirmations à 100 ms et la réparation 5 × 10 ms.
 *
 * ⚠ Ralentir un tick change ce qu'il VIDE, pas seulement ce qu'il émet : le
 * relais de la gauche passé à 100 ms au repos perdait les touches de la droite
 * en mode USB, parce que ce tick vide la FIFO nRF24 (3 trames) — voir
 * kbd_relay_cadence_ms. Lister les consommateurs avant de toucher une valeur.
 *
 * Testé host : test/test_cadence.c. */
#include <stdint.h>

#define CADENCE_TICK_MS          10u   /* CONFIG_FREERTOS_HZ = 100 */
#define CADENCE_REPOS_MIN_MS     30u   /* 3 ticks : CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP */
#define CADENCE_REPOS_OK(ms) _Static_assert((ms) >= CADENCE_REPOS_MIN_MS, #ms " < 3 ticks : tue le light sleep automatique")

/* Tâche clavier (gauche) : minuteries tap-hold/tap-dance/leader, mode test,
 * fusion distante en USB. Notifiée par le balayage sur changement. */
#define KBD_CADENCE_ACTIF_MS     10u
#define KBD_CADENCE_REPOS_MS     100u
#define KBD_CADENCE_FENETRE_MS   1500u   /* > LEADER_TIMEOUT_MS (1000) */
CADENCE_REPOS_OK(KBD_CADENCE_REPOS_MS);

/* Relais radio de la gauche (timer esp_timer) : réparation bornée, maintiens,
 * sync par ACK, et en USB la vidange de la FIFO des trames de la droite. */
#define KBD_RELAY_REFRESH_MS     10u
#define KBD_RELAY_REPOS_MS       100u
CADENCE_REPOS_OK(KBD_RELAY_REPOS_MS);

/* Rafraîchissement de la droite : réaffirmation des maintiens, réparation. */
#define HALF_TX_TENU_MS          20u
#define HALF_TX_REPOS_MS         100u
CADENCE_REPOS_OK(HALF_TX_REPOS_MS);

/* Lien TRRS : tick de la machine d'états en poignée de main / lien établi ;
 * au repos (5 V mort, pas d'USB) la tâche attend. */
#define LINK_TICK_MS             10u
#define LINK_REPOS_MS            100u
CADENCE_REPOS_OK(LINK_REPOS_MS);

/* Écran de la droite (tâche minimale : update() + VCOM). */
#define MEMLCD_DROITE_PERIODE_MS 100u
CADENCE_REPOS_OK(MEMLCD_DROITE_PERIODE_MS);

/* LVGL (esp_lvgl_port) : tick, rafraîchissement, sommeil max de la tâche. */
#define LVGL_TICK_MS             50u
#define LVGL_REFR_MS             200u
#define LVGL_TASK_MAX_SLEEP_MS   500u
CADENCE_REPOS_OK(LVGL_TICK_MS);
CADENCE_REPOS_OK(LVGL_REFR_MS);

/* Battement de coeur de banc. */
#define HB_PERIODE_MS            10000u
