/* Tamagotchi game engine */
#include "tama_engine.h"
#include "nvs_utils.h"
#include "keyboard_config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TAMA";

static tama2_stats_t stats;
static tama2_state_t state = TAMA2_IDLE;
static bool enabled = true;

/* Keys between stat decay ticks */
#define HUNGER_DECAY_KEYS   500   /* lose 10 hunger every 500 keys */
#define ENERGY_DECAY_KEYS   800   /* lose 10 energy every 800 keys */
#define XP_PER_1000_KEYS    100   /* gain 100 XP per 1000 keys */
#define XP_PER_LEVEL        500   /* XP needed to level up */
#define MAX_LEVEL            19   /* 20 critters (0-19) */

static uint32_t keys_since_hunger_tick = 0;
static uint32_t keys_since_energy_tick = 0;

/* ── Helpers ─────────────────────────────────────────────────────── */

static void clamp_stats(void)
{
    if (stats.hunger > TAMA2_STAT_MAX) stats.hunger = TAMA2_STAT_MAX;
    if (stats.happiness > TAMA2_STAT_MAX) stats.happiness = TAMA2_STAT_MAX;
    if (stats.energy > TAMA2_STAT_MAX) stats.energy = TAMA2_STAT_MAX;
    stats.health = (stats.hunger + stats.happiness + stats.energy) / 3;
}

static uint32_t state_hold_keys = 0; /* keys since last state change */
#define STATE_HOLD_MIN 50  /* min keys before state can change again */

static void update_state(uint32_t kpm)
{
    tama2_state_t new_state;

    /* Priority: sick > sad > sleeping > sleepy > eating > excited > happy > idle */
    if (stats.health < 200)       new_state = TAMA2_SICK;
    else if (stats.happiness < 200) new_state = TAMA2_SAD;
    else if (stats.energy < 100)  new_state = TAMA2_SLEEPING;
    else if (stats.energy < 300)  new_state = TAMA2_SLEEPY;
    else if (kpm > 200)           new_state = TAMA2_EXCITED;
    else if (kpm > 80)            new_state = TAMA2_HAPPY;
    else                          new_state = TAMA2_IDLE;

    state_hold_keys++;
    /* Only change state after minimum keypresses to avoid flickering */
    if (new_state != state && state_hold_keys >= STATE_HOLD_MIN) {
        state = new_state;
        state_hold_keys = 0;
    }
}

static void check_level_up(void)
{
    while (stats.xp >= XP_PER_LEVEL && stats.level < MAX_LEVEL) {
        stats.xp -= XP_PER_LEVEL;
        stats.level++;
        state = TAMA2_CELEBRATING;
        ESP_LOGI(TAG, "Level up! Now level %d", stats.level);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

void tama_engine_init(void)
{
    /* Default stats for a new pet */
    memset(&stats, 0, sizeof(stats));
    stats.hunger = 800;
    stats.happiness = 800;
    stats.energy = TAMA2_STAT_MAX;
    stats.health = TAMA2_STAT_MAX;

    /* Try loading from NVS */
    uint32_t dummy = 0;
    esp_err_t err = nvs_load_blob_with_total(STORAGE_NAMESPACE, "tama_stats", &stats,
                                              sizeof(stats), "tama_ver", &dummy);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Tama loaded: level=%d keys=%lu", stats.level, (unsigned long)stats.total_keys);
    } else {
        ESP_LOGI(TAG, "New tama created");
    }

    stats.session_keys = 0;
    clamp_stats();
}

void tama_engine_keypress(uint32_t current_kpm)
{
    if (!enabled) return;

    stats.total_keys++;
    stats.session_keys++;

    /* Track personal best */
    if (current_kpm > stats.max_kpm)
        stats.max_kpm = current_kpm;

    /* Hunger decay */
    keys_since_hunger_tick++;
    if (keys_since_hunger_tick >= HUNGER_DECAY_KEYS) {
        keys_since_hunger_tick = 0;
        if (stats.hunger > 10)
            stats.hunger -= 10;
        else
            stats.hunger = 0;
    }

    /* Energy decay */
    keys_since_energy_tick++;
    if (keys_since_energy_tick >= ENERGY_DECAY_KEYS) {
        keys_since_energy_tick = 0;
        if (stats.energy > 10)
            stats.energy -= 10;
        else
            stats.energy = 0;
    }

    /* Happiness boost from KPM */
    if (current_kpm > 100 && stats.happiness < TAMA2_STAT_MAX)
        stats.happiness += 1;
    else if (current_kpm < 20 && stats.happiness > 0)
        stats.happiness -= 1;

    /* XP gain */
    if ((stats.total_keys % 1000) == 0) {
        stats.xp += XP_PER_1000_KEYS;
        check_level_up();
    }

    clamp_stats();
    update_state(current_kpm);
}

void tama_engine_action(tama2_action_t action)
{
    if (!enabled) return;

    switch (action) {
    case TAMA2_ACTION_FEED:
        stats.hunger = (stats.hunger + 300 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : stats.hunger + 300;
        state = TAMA2_EATING;
        ESP_LOGI(TAG, "Fed! hunger=%d", stats.hunger);
        break;
    case TAMA2_ACTION_PLAY:
        stats.happiness = (stats.happiness + 200 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : stats.happiness + 200;
        state = TAMA2_HAPPY;
        ESP_LOGI(TAG, "Played! happiness=%d", stats.happiness);
        break;
    case TAMA2_ACTION_SLEEP:
        stats.energy = (stats.energy + 400 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : stats.energy + 400;
        state = TAMA2_SLEEPING;
        ESP_LOGI(TAG, "Slept! energy=%d", stats.energy);
        break;
    case TAMA2_ACTION_MEDICINE:
        stats.hunger = (stats.hunger + 100 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : stats.hunger + 100;
        stats.happiness = (stats.happiness + 100 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : stats.happiness + 100;
        stats.energy = (stats.energy + 100 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : stats.energy + 100;
        ESP_LOGI(TAG, "Medicine! health=%d", stats.health);
        break;
    }
    clamp_stats();
}

tama2_state_t tama_engine_get_state(void) { return state; }
const tama2_stats_t *tama_engine_get_stats(void) { return &stats; }
bool tama_engine_is_enabled(void) { return enabled; }
void tama_engine_set_enabled(bool e) { enabled = e; }

void tama_engine_save(void)
{
    uint32_t ver = 1;
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "tama_stats", &stats,
                                              sizeof(stats), "tama_ver", ver);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to save tama stats: %s", esp_err_to_name(err));
}

void tama_engine_session_start(void)
{
    stats.session_keys = 0;
    /* Recover some energy on new session (like sleeping between sessions) */
    stats.energy = (stats.energy + 200 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : stats.energy + 200;
    clamp_stats();
}

void tama_engine_session_end(void)
{
    tama_engine_save();
}

uint8_t tama_engine_get_critter(void)
{
    return (stats.level <= MAX_LEVEL) ? (uint8_t)stats.level : MAX_LEVEL;
}
