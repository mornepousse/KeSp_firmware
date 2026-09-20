#pragma once
/* Cadences of the Niphargus halves — ONE place, one rule, one guard.
 *
 * The rule: at rest, no periodic wait below CADENCE_REPOS_MIN_MS.
 * ESP-IDF only attempts automatic light sleep (tickless) if all tasks are
 * blocked for >= CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP ticks (3, at
 * 100 Hz = 30 ms). A 10 ms loop leaves ONE tick free: the profile said
 * "mode SLEEP 92%" and light_sleep_counts stayed at 0 (bench 2026-09-16,
 * keyboard task). An idle mode is not a sleep, and nothing signals it.
 *
 * The guard: every REST cadence is followed by a CADENCE_REPOS_OK — a
 * value that is too short does not compile. The ACTIVE cadences (key held,
 * bounded repair, handshake) stay <= 20 ms: that is what keeps the
 * reaffirmations at 100 ms and the repair at 5 x 10 ms.
 *
 * ⚠ slowing down a tick changes what it DRAINS, not just what it
 * emits: the left relay set to 100 ms at rest was losing the right half's
 * keys in USB mode, because that tick drains the nRF24 FIFO (3 frames) — see
 * kbd_relay_cadence_ms. List the consumers before touching a value.
 *
 * Tested on host: test/test_cadence.c. */
#include <stdint.h>

#define CADENCE_TICK_MS          10u   /* CONFIG_FREERTOS_HZ = 100 */
#define CADENCE_REPOS_MIN_MS     30u   /* 3 ticks: CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP */
#define CADENCE_REPOS_OK(ms) _Static_assert((ms) >= CADENCE_REPOS_MIN_MS, #ms " < 3 ticks : tue le light sleep automatique")

/* Keyboard task (left): tap-hold/tap-dance/leader timers, test mode,
 * remote fusion over USB. Notified by the scan on change. */
#define KBD_CADENCE_ACTIF_MS     10u
#define KBD_CADENCE_REPOS_MS     100u
#define KBD_CADENCE_FENETRE_MS   1500u   /* > LEADER_TIMEOUT_MS (1000) */
CADENCE_REPOS_OK(KBD_CADENCE_REPOS_MS);

/* Left radio relay (esp_timer timer): bounded repair, holds,
 * ACK sync, and over USB the draining of the right half's frame FIFO. */
#define KBD_RELAY_REFRESH_MS     10u
#define KBD_RELAY_REPOS_MS       100u
CADENCE_REPOS_OK(KBD_RELAY_REPOS_MS);

/* Right half refresh: reaffirmation of holds, repair. */
#define HALF_TX_TENU_MS          20u
#define HALF_TX_REPOS_MS         100u
CADENCE_REPOS_OK(HALF_TX_REPOS_MS);

/* TRRS link: state machine tick during handshake / established link;
 * at rest (5 V dead, no USB) the task is blocked on the UART queue and only
 * wakes on its own to probe USB (human event). */
#define LINK_TICK_MS             10u
#define LINK_REPOS_MS            1000u
CADENCE_REPOS_OK(LINK_REPOS_MS);

/* Right half screen (minimal task: update() + VCOM). */
#define MEMLCD_DROITE_PERIODE_MS 1000u  /* model: 30 s gauge, dongle seen; VCOM timestamped in update() */
CADENCE_REPOS_OK(MEMLCD_DROITE_PERIODE_MS);

/* LVGL (esp_lvgl_port): tick, refresh, max task sleep. */
#define LVGL_TICK_MS             50u
#define LVGL_REFR_MS             200u
#define LVGL_TASK_MAX_SLEEP_MS   500u
CADENCE_REPOS_OK(LVGL_TICK_MS);
CADENCE_REPOS_OK(LVGL_REFR_MS);

/* Sleep task (veille_task.c): evaluation of inactivity and vetoes.
 * Sleep only arrives at 15 s, a second of latency is not noticeable. */
#define VEILLE_TICK_MS           1000u
CADENCE_REPOS_OK(VEILLE_TICK_MS);

/* Bench heartbeat (carried by the sleep task). */
#define HB_PERIODE_MS            10000u
