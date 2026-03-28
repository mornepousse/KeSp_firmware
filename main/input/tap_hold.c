/* Tap/Hold engine implementation */
#include "tap_hold.h"
#include "key_definitions.h"
#include "key_features.h"
#include "matrix_scan.h"  /* current_layout, last_layer, layer_changed */
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TAP_HOLD";

static tap_hold_entry_t pending[TAP_HOLD_MAX_PENDING];
static uint8_t active_hold_mods = 0;
static int8_t active_hold_layer = -1;
static bool hold_activated_flag = false;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void tap_hold_init(void)
{
    memset(pending, 0, sizeof(pending));
    active_hold_mods = 0;
    active_hold_layer = -1;
}

static tap_hold_entry_t *find_by_pos(uint8_t row, uint8_t col)
{
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) {
        if (pending[i].state != TH_IDLE &&
            pending[i].row == row && pending[i].col == col)
            return &pending[i];
    }
    return NULL;
}

static tap_hold_entry_t *find_free(void)
{
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) {
        if (pending[i].state == TH_IDLE)
            return &pending[i];
    }
    return NULL;
}

static void resolve_as_hold(tap_hold_entry_t *e)
{
    e->state = TH_HOLD;
    if (K_IS_MT(e->keycode)) {
        active_hold_mods |= K_MT_MOD(e->keycode);
    } else if (K_IS_LT(e->keycode)) {
        active_hold_layer = K_LT_LAYER(e->keycode);
        last_layer = current_layout;
        current_layout = active_hold_layer;
        layer_changed();
    } else if (K_IS_OSM(e->keycode)) {
        /* Hold OSM = regular modifier (like holding shift) */
        active_hold_mods |= K_OSM_MOD(e->keycode);
    }
}

static void release_hold(tap_hold_entry_t *e)
{
    if (K_IS_MT(e->keycode)) {
        active_hold_mods &= ~K_MT_MOD(e->keycode);
    } else if (K_IS_LT(e->keycode)) {
        if (active_hold_layer == K_LT_LAYER(e->keycode))
            active_hold_layer = -1;
        current_layout = last_layer;
        layer_changed();
    } else if (K_IS_OSM(e->keycode)) {
        active_hold_mods &= ~K_OSM_MOD(e->keycode);
    }
    e->state = TH_IDLE;
}

bool tap_hold_on_press(uint16_t keycode, uint8_t row, uint8_t col)
{
    if (!K_IS_LT(keycode) && !K_IS_MT(keycode) && !K_IS_OSM(keycode))
        return false;

    /* Already tracking this position? Skip. */
    tap_hold_entry_t *existing = find_by_pos(row, col);
    if (existing) return true; /* already absorbed */

    tap_hold_entry_t *e = find_free();
    if (!e) {
        ESP_LOGW(TAG, "No free tap/hold slot");
        return false;
    }

    e->state = TH_PENDING;
    e->keycode = keycode;
    e->row = row;
    e->col = col;
    e->press_time_ms = now_ms();
    ESP_LOGI(TAG, "PRESS r%d c%d kc=0x%04X", row, col, keycode);
    return true;
}

bool tap_hold_on_release(uint8_t row, uint8_t col)
{
    tap_hold_entry_t *e = find_by_pos(row, col);
    if (!e) return false;

    if (e->state == TH_PENDING) {
        /* Released before timeout and no interrupt → TAP */
        e->state = TH_TAP;
        ESP_LOGI(TAG, "TAP r%d c%d -> 0x%02X",
                 e->row, e->col,
                 K_IS_LT(e->keycode) ? K_LT_KEY(e->keycode) : K_MT_KEY(e->keycode));
        return true;
    } else if (e->state == TH_HOLD) {
        /* Hold released → deactivate modifier/layer */
        release_hold(e);
        return true;
    }

    e->state = TH_IDLE;
    return true;
}

void tap_hold_tick(void)
{
    hold_activated_flag = false;
    uint32_t t = now_ms();
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) {
        if (pending[i].state == TH_PENDING) {
            if ((t - pending[i].press_time_ms) >= TAP_HOLD_TIMEOUT_MS) {
                resolve_as_hold(&pending[i]);
                hold_activated_flag = true;
                ESP_LOGI(TAG, "HOLD timeout r%d c%d", pending[i].row, pending[i].col);
            }
        }
    }
}

bool tap_hold_hold_just_activated(void)
{
    return hold_activated_flag;
}

void tap_hold_interrupt(void)
{
    /* Any pending tap/hold keys become holds when another key is pressed */
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) {
        if (pending[i].state == TH_PENDING) {
            resolve_as_hold(&pending[i]);
        }
    }
}

uint16_t tap_hold_get_resolved(uint8_t row, uint8_t col, bool *is_hold)
{
    tap_hold_entry_t *e = find_by_pos(row, col);
    if (!e) return 0;

    if (e->state == TH_TAP) {
        *is_hold = false;
        uint16_t tap_kc = K_IS_LT(e->keycode) ? K_LT_KEY(e->keycode)
                        : K_IS_MT(e->keycode) ? K_MT_KEY(e->keycode)
                        : 0;
        e->state = TH_IDLE; /* consumed */
        return tap_kc;
    }

    if (e->state == TH_HOLD) {
        *is_hold = true;
        return e->keycode; /* caller handles hold behavior */
    }

    /* Still pending — don't emit anything yet */
    *is_hold = false;
    return 0;
}

uint8_t tap_hold_get_active_mods(void)
{
    return active_hold_mods;
}

int8_t tap_hold_get_active_layer(void)
{
    return active_hold_layer;
}

uint8_t tap_hold_consume_tap(void)
{
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) {
        if (pending[i].state == TH_TAP) {
            uint16_t kc = pending[i].keycode;
            pending[i].state = TH_IDLE;
            if (K_IS_LT(kc)) return K_LT_KEY(kc);
            if (K_IS_MT(kc)) return K_MT_KEY(kc);
            if (K_IS_OSM(kc)) {
                /* Tap OSM = arm one-shot modifier for next keypress */
                osm_arm(K_OSM_MOD(kc));
                return 0; /* no keycode to inject, just armed OSM */
            }
            return 0;
        }
    }
    return 0;
}
