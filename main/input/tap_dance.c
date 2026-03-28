/* Tap Dance engine implementation */
#include "tap_dance.h"
#include "keyboard_config.h"
#include "nvs_utils.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>


static const char *TAG = "TAP_DANCE";

static tap_dance_config_t configs[TAP_DANCE_MAX_SLOTS];

typedef enum { TD_IDLE, TD_COUNTING, TD_HOLDING, TD_RESOLVED } td_state_t;

typedef struct {
    td_state_t state;
    uint8_t index;          /* dance slot index */
    uint8_t tap_count;      /* number of taps so far */
    uint8_t row, col;       /* matrix position */
    bool key_held;          /* currently held? */
    uint32_t last_tap_ms;   /* time of last tap (for timeout) */
} td_active_t;

static td_active_t active = { .state = TD_IDLE };
static uint8_t resolved_keycode = 0;
static bool resolved_flag = false;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void resolve_dance(uint8_t action_index)
{
    if (action_index < 4 && configs[active.index].actions[action_index] != 0) {
        resolved_keycode = configs[active.index].actions[action_index];
        resolved_flag = true;
    }
    active.state = TD_RESOLVED;
}

/* ── Public API ──────────────────────────────────────────────────── */

void tap_dance_init(void)
{
    memset(configs, 0, sizeof(configs));
    active.state = TD_IDLE;
    resolved_keycode = 0;
    resolved_flag = false;
}

void tap_dance_set(uint8_t index, const uint8_t actions[4])
{
    if (index < TAP_DANCE_MAX_SLOTS)
        memcpy(configs[index].actions, actions, 4);
}

bool tap_dance_on_press(uint8_t index, uint8_t row, uint8_t col)
{
    if (index >= TAP_DANCE_MAX_SLOTS) return false;

    if (active.state == TD_IDLE || active.state == TD_RESOLVED) {
        /* Start new dance */
        active.state = TD_COUNTING;
        active.index = index;
        active.tap_count = 1;
        active.row = row;
        active.col = col;
        active.key_held = true;
        active.last_tap_ms = now_ms();
        resolved_keycode = 0;
        resolved_flag = false;
        return true;
    }

    if (active.state == TD_COUNTING && active.index == index) {
        /* Same dance key pressed again — increment tap count */
        active.tap_count++;
        active.key_held = true;
        active.last_tap_ms = now_ms();

        if (active.tap_count > TAP_DANCE_MAX_TAPS) {
            /* Max taps reached — resolve immediately */
            resolve_dance(TAP_DANCE_MAX_TAPS - 1);
        }
        return true;
    }

    /* Different key or state — resolve current dance and reject */
    return false;
}

void tap_dance_on_release(uint8_t row, uint8_t col)
{
    if (active.state == TD_IDLE) return;
    if (active.row != row || active.col != col) return;

    if (active.state == TD_HOLDING) {
        /* Release from hold → resolve as hold action */
        resolve_dance(3);
        return;
    }

    active.key_held = false;
    /* Don't resolve yet — wait for timeout to see if another tap comes */
}

void tap_dance_tick(void)
{
    resolved_flag = false;

    if (active.state != TD_COUNTING) return;

    uint32_t t = now_ms();
    uint32_t elapsed = t - active.last_tap_ms;

    if (active.key_held && elapsed >= TAP_DANCE_TIMEOUT_MS) {
        /* Held past timeout → HOLD action */
        active.state = TD_HOLDING;
        resolve_dance(3);
        return;
    }

    if (!active.key_held && elapsed >= TAP_DANCE_TIMEOUT_MS) {
        /* Released and timed out → resolve based on tap count */
        uint8_t action_idx = (active.tap_count > TAP_DANCE_MAX_TAPS)
                           ? TAP_DANCE_MAX_TAPS - 1
                           : active.tap_count - 1;
        resolve_dance(action_idx);
    }
}

uint8_t tap_dance_consume(void)
{
    uint8_t kc = resolved_keycode;
    resolved_keycode = 0;
    if (active.state == TD_RESOLVED)
        active.state = TD_IDLE;
    return kc;
}

bool tap_dance_just_resolved(void)
{
    return resolved_flag;
}

const tap_dance_config_t *tap_dance_get(uint8_t index)
{
    if (index >= TAP_DANCE_MAX_SLOTS) return NULL;
    return &configs[index];
}

void tap_dance_save(void)
{
    /* Save all configs as a single blob. Only count used slots. */
    uint8_t count = 0;
    for (int i = 0; i < TAP_DANCE_MAX_SLOTS; i++) {
        for (int j = 0; j < 4; j++) {
            if (configs[i].actions[j] != 0) { count = i + 1; break; }
        }
    }
    nvs_save_blob_with_total(STORAGE_NAMESPACE, "td_configs", configs,
                              count * sizeof(tap_dance_config_t), "td_count", count);
    ESP_LOGI(TAG, "Tap dance saved (%d slots)", count);
}

void tap_dance_load(void)
{
    uint32_t count = 0;
    nvs_load_blob_with_total(STORAGE_NAMESPACE, "td_configs", configs,
                              sizeof(configs), "td_count", &count);
    if (count > 0)
        ESP_LOGI(TAG, "Tap dance loaded (%lu slots)", (unsigned long)count);
}
