/* Tamagotchi game engine — display-agnostic logic.
   Stats are driven by keyboard usage, not real time.
   The pet never dies — it gets sad/sick but recovers when you type. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ── Stats ───────────────────────────────────────────────────────── */

#define TAMA_STAT_MAX 1000

typedef struct {
    uint16_t hunger;       /* 0 = starving, 1000 = full */
    uint16_t happiness;    /* 0 = sad, 1000 = ecstatic */
    uint16_t energy;       /* 0 = exhausted, 1000 = rested */
    uint16_t health;       /* computed: average of other 3 */
    uint32_t total_keys;   /* lifetime keypresses */
    uint32_t session_keys; /* keypresses this session */
    uint32_t max_kpm;      /* personal best KPM */
    uint16_t level;        /* evolution level (0-19) */
    uint16_t xp;           /* XP toward next level */
} tama_stats_t;

/* ── Emotional states ────────────────────────────────────────────── */

typedef enum {
    TAMA_IDLE,          /* no activity */
    TAMA_HAPPY,         /* good KPM */
    TAMA_EXCITED,       /* fast typing */
    TAMA_EATING,        /* just fed */
    TAMA_SLEEPY,        /* low energy */
    TAMA_SLEEPING,      /* screen sleep / long inactivity */
    TAMA_SICK,          /* health below 200 */
    TAMA_SAD,           /* happiness below 200 */
    TAMA_CELEBRATING,   /* milestone! */
    TAMA_STATE_COUNT
} tama_state_t;

/* ── Actions (direct interactions) ───────────────────────────────── */

typedef enum {
    TAMA_ACTION_FEED,
    TAMA_ACTION_PLAY,
    TAMA_ACTION_SLEEP,
    TAMA_ACTION_MEDICINE,
} tama_action_t;

/* ── API ─────────────────────────────────────────────────────────── */

/* Initialize engine (loads stats from NVS) */
void tama_engine_init(void);

/* Called on every keypress — drives "tama time" */
void tama_engine_keypress(uint32_t current_kpm);

/* Perform a direct action (from combo or layer) */
void tama_engine_action(tama_action_t action);

/* Get current state and stats */
tama_state_t tama_engine_get_state(void);
const tama_stats_t *tama_engine_get_stats(void);

/* Check if engine is enabled */
bool tama_engine_is_enabled(void);
void tama_engine_set_enabled(bool enabled);

/* Save stats to NVS (call periodically) */
void tama_engine_save(void);

/* Notify engine of session start/end */
void tama_engine_session_start(void);
void tama_engine_session_end(void);

/* Get critter index for current level (0-19) */
uint8_t tama_engine_get_critter(void);
