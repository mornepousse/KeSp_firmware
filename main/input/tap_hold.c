/* Tap/Hold engine implementation */
#include "tap_hold.h"
#include "key_definitions.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TAP_HOLD";

static tap_hold_entry_t pending[TAP_HOLD_MAX_PENDING];
static uint8_t active_hold_mods = 0;
static int8_t active_hold_layer = -1;

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
        ESP_LOGD(TAG, "MT hold: mod=0x%02X", K_MT_MOD(e->keycode));
    } else if (K_IS_LT(e->keycode)) {
        active_hold_layer = K_LT_LAYER(e->keycode);
        ESP_LOGD(TAG, "LT hold: layer=%d", active_hold_layer);
    }
}

static void release_hold(tap_hold_entry_t *e)
{
    if (K_IS_MT(e->keycode)) {
        active_hold_mods &= ~K_MT_MOD(e->keycode);
    } else if (K_IS_LT(e->keycode)) {
        /* Only clear if this was the active hold layer */
        if (active_hold_layer == K_LT_LAYER(e->keycode))
            active_hold_layer = -1;
    }
    e->state = TH_IDLE;
}

bool tap_hold_on_press(uint16_t keycode, uint8_t row, uint8_t col)
{
    if (!K_IS_LT(keycode) && !K_IS_MT(keycode))
        return false;

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
    return true;
}

bool tap_hold_on_release(uint8_t row, uint8_t col)
{
    tap_hold_entry_t *e = find_by_pos(row, col);
    if (!e) return false;

    if (e->state == TH_PENDING) {
        /* Released before timeout and no interrupt → TAP */
        e->state = TH_TAP;
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
    uint32_t t = now_ms();
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) {
        if (pending[i].state == TH_PENDING) {
            if ((t - pending[i].press_time_ms) >= TAP_HOLD_TIMEOUT_MS) {
                resolve_as_hold(&pending[i]);
            }
        }
    }
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
