/* Tap/Hold engine: distinguishes tap (quick press+release) from hold (long press).
   Used by LT (Layer-Tap), MT (Mod-Tap), and OSM (One-Shot Modifier).

   Flow:
   1. on_press: key enters PENDING state
   2. Either:
      a) tick() timeout → HOLD (activates modifier/layer)
      b) on_release before timeout → TAP (emits tap keycode)
      c) interrupt (another key pressed) → HOLD
   3. on_release from HOLD → deactivates modifier/layer
*/
#include "tap_hold.h"
#include "key_definitions.h"
#include "key_features.h"
#include "matrix_scan.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TAP_HOLD";

static tap_hold_entry_t pending[TAP_HOLD_MAX_PENDING];
static uint8_t active_hold_mods = 0;
static int8_t active_hold_layer = -1;
static bool hold_activated_flag = false;
static uint32_t th_seq = 0;         /* compteur d'ordre d'activation des LT holds */
static int8_t th_layer_base = -1;   /* couche à restaurer quand plus aucune LT tenue (-1 = pas de LT) */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ── Internal helpers ────────────────────────────────────────────── */

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

/* Recalcule la couche pilotée par les LT holds actifs : la LT activée le plus
 * récemment (activate_seq max) gagne ; quand plus aucune LT n'est tenue, restaure
 * la couche de base capturée à la 1re LT. Robuste au relâchement dans le désordre
 * de plusieurs LT concurrentes (le bug audit E1 : le mono-slot perdait la couche
 * de la LT encore tenue au 1er release). */
static void recompute_lt_layer(void)
{
    tap_hold_entry_t *top = NULL;
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) {
        if (pending[i].state == TH_HOLD && K_IS_LT(pending[i].keycode) &&
            (!top || pending[i].activate_seq > top->activate_seq))
            top = &pending[i];
    }
    if (top) {
        active_hold_layer = (int8_t)K_LT_LAYER(top->keycode);
        current_layout = (uint8_t)active_hold_layer;
    } else {
        active_hold_layer = -1;
        if (th_layer_base >= 0) {
            current_layout = (uint8_t)th_layer_base;
            th_layer_base = -1;
        }
    }
    layer_changed();
}

static void activate_hold(tap_hold_entry_t *e)
{
    e->state = TH_HOLD;
    uint16_t kc = e->keycode;

    if (K_IS_MT(kc)) {
        active_hold_mods |= K_MT_MOD(kc);
    } else if (K_IS_LT(kc)) {
        uint8_t layer = K_LT_LAYER(kc);
        if (layer < LAYERS) {   /* couche hors bornes → pas de changement (évite l'OOB keymaps[]) */
            if (active_hold_layer < 0)         /* 1re LT active → mémorise la couche de base */
                th_layer_base = (int8_t)current_layout;
            e->activate_seq = ++th_seq;
            recompute_lt_layer();              /* e (TH_HOLD, seq max) devient la couche active */
        }
    } else if (K_IS_OSM(kc)) {
        active_hold_mods |= K_OSM_MOD(kc);
    }
}

static void deactivate_hold(tap_hold_entry_t *e)
{
    uint16_t kc = e->keycode;

    if (K_IS_MT(kc)) {
        active_hold_mods &= ~K_MT_MOD(kc);
    } else if (K_IS_LT(kc)) {
        uint8_t layer = K_LT_LAYER(kc);
        if (layer < LAYERS) {   /* symétrique à activate_hold : la couche hors bornes n'avait rien changé */
            e->state = TH_IDLE;                /* retirer ce hold avant de recalculer la couche */
            recompute_lt_layer();              /* → LT encore tenue la + récente, ou couche de base */
        }
    } else if (K_IS_OSM(kc)) {
        active_hold_mods &= ~K_OSM_MOD(kc);
    }

    e->state = TH_IDLE;
}

/* ── Public API ──────────────────────────────────────────────────── */

void tap_hold_init(void)
{
    memset(pending, 0, sizeof(pending));
    active_hold_mods = 0;
    active_hold_layer = -1;
    th_seq = 0;
    th_layer_base = -1;
}

bool tap_hold_on_press(uint16_t keycode, uint8_t row, uint8_t col)
{
    if (!K_IS_LT(keycode) && !K_IS_MT(keycode) && !K_IS_OSM(keycode))
        return false;

    /* Already tracking this position — don't duplicate */
    if (find_by_pos(row, col))
        return true;

    tap_hold_entry_t *e = find_free();
    if (!e) {
        ESP_LOGW(TAG, "No free slot");
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
        e->state = TH_TAP;
    } else if (e->state == TH_HOLD) {
        deactivate_hold(e);
    } else {
        e->state = TH_IDLE;
    }
    return true;
}

void tap_hold_tick(void)
{
    hold_activated_flag = false;
    uint32_t t = now_ms();
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) {
        if (pending[i].state == TH_PENDING &&
            (t - pending[i].press_time_ms) >= TAP_HOLD_TIMEOUT_MS) {
            activate_hold(&pending[i]);
            hold_activated_flag = true;
        }
    }
}

bool tap_hold_hold_just_activated(void)
{
    return hold_activated_flag;
}

void tap_hold_interrupt(void)
{
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) {
        if (pending[i].state == TH_PENDING)
            activate_hold(&pending[i]);
    }
}

uint16_t tap_hold_get_resolved(uint8_t row, uint8_t col, bool *is_hold)
{
    tap_hold_entry_t *e = find_by_pos(row, col);
    if (!e) return 0;

    if (e->state == TH_HOLD) {
        *is_hold = true;
        return e->keycode;
    }

    *is_hold = false;
    return 0;
}

uint8_t tap_hold_consume_tap(void)
{
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) {
        if (pending[i].state != TH_TAP) continue;

        uint16_t kc = pending[i].keycode;
        pending[i].state = TH_IDLE;

        if (K_IS_LT(kc)) return K_LT_KEY(kc);
        if (K_IS_MT(kc)) return K_MT_KEY(kc);
        if (K_IS_OSM(kc))
            osm_arm(K_OSM_MOD(kc));
        /* OSM armé (ou tap sans keycode direct) → continuer à scanner les autres
         * taps en attente au lieu de rendre 0, ce qui arrêterait la boucle du
         * caller et perdrait un tap MT/LT résolu le même cycle (audit M4). */
    }
    return 0;
}

uint8_t tap_hold_get_active_mods(void)  { return active_hold_mods; }
int8_t tap_hold_get_active_layer(void)  { return active_hold_layer; }
