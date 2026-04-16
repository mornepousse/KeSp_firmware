/* Key processing: converts matrix state to HID keycodes.
   Handles: layers (MO/TO/LT), modifiers (MT/OSM), macros, caps word, repeat key.

   Flow per scan cycle:
   1. Detect released keys → resolve tap/hold
   2. Determine active layer (base + LT hold + OSL override)
   3. For each pressed key: lookup keymap → process advanced or legacy keycodes
   4. Inject resolved taps, one-shot modifiers, caps word
   5. Save state for next cycle
*/
#include "key_processor.h"
#include "matrix_scan.h"
#include "keyboard_config.h"
#include "key_definitions.h"
#include "keymap.h"
#include "keyboard_actions.h"
#include "keyboard_task.h"
#include "hid_bluetooth_manager.h" /* bt_next/prev/pair/disconnect */
#include "tap_hold.h"
#include "tap_dance.h"
#include "combo.h"
#include "leader.h"
#include "key_features.h"
#include "tama_engine.h"
#include "esp_log.h"

static const char *TAG = "KEY_PROC";

/* ── State ───────────────────────────────────────────────────────── */

uint16_t keypress_internal_function = 0;
uint8_t current_row_layer_changer = INVALID_KEY_POS;
uint8_t current_col_layer_changer = INVALID_KEY_POS;
static uint8_t lm_active_mods = 0;  /* modifier mask held by LM key */
uint16_t extra_keycodes[6] = {0};

static uint8_t prev_press_row[6] = {INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS,
                                     INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};
static uint8_t prev_press_col[6] = {INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS,
                                     INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};
static uint8_t tap_injected_slots = 0;

/* ── Legacy layer switching ──────────────────────────────────────── */

static void apply_momentary_layer(uint16_t keycode, uint8_t key_idx)
{
    uint8_t layer = 0xFF;
    uint8_t mods = 0;

    if (keycode >= MO_L0 && keycode <= MO_L9) {
        layer = (keycode - MO_L0) / 256;
    } else if (K_IS_LM(keycode)) {
        layer = K_LM_LAYER(keycode);
        mods = K_LM_MODS(keycode);
    }

    if (layer <= 9) {
        last_layer = current_layout;
        current_layout = layer;
        lm_active_mods = mods;
        layer_changed();
        current_row_layer_changer = current_press_row[key_idx];
        current_col_layer_changer = current_press_col[key_idx];
    }
}

static void apply_toggle_layer(uint16_t keycode)
{
    if (keycode >= TO_L0 && keycode <= TO_L9) {
        int16_t new_layer = (keycode - TO_L0) / 256;
        current_layout = (current_layout == new_layer) ? 0 : new_layer;
        last_layer = current_layout;
        layer_changed();
    }
}

static bool is_new_press(uint8_t row, uint8_t col);

/* ── Legacy internal function dispatch ───────────────────────────── */

static bool detect_internal_function(int16_t keycode)
{
    if (keycode >= TO_L0 && keycode < K_OSM_BASE) {
        if (keypress_internal_function == 0) {
            keypress_internal_function = keycode;
            return true;
        }
    }
    return false;
}

static void dispatch_internal_function(void)
{
    switch (keypress_internal_function) {
    case BT_SWITCH_DEVICE:
        if (usb_bl_state == 0 && hid_bluetooth_is_initialized())
            usb_bl_state = 1;
        else
            usb_bl_state = 0;
        km_post_display_update();
        break;
    case BT_TOGGLE:
        km_post_bt_toggle();
        break;
    case K_BT_NEXT:
        bt_next_device();
        km_post_display_update();
        break;
    case K_BT_PREV:
        bt_prev_device();
        km_post_display_update();
        break;
    case K_BT_PAIR:
        bt_start_pairing();
        km_post_display_update();
        break;
    case K_BT_DISCONNECT:
        bt_disconnect();
        km_post_display_update();
        break;
    }
}

/* ── Macro expansion ─────────────────────────────────────────────── */

/* Pending macro sequence to play (set by expand_macro, consumed by keyboard_task) */
static int16_t pending_macro_idx = -1;

static uint8_t macro_hold_mods = 0;  /* modifier mask from held no-delay macro */

static void expand_macro(uint16_t keycode)
{
    if (keycode < MACRO_1 || keycode > MACRO_20) return;
    int16_t idx = (keycode - MACRO_1) / 256;
    if (idx >= MAX_MACROS || macros_list[idx].name[0] == '\0') return;

    const macro_step_t *steps = macros_list[idx].steps;
    if (steps[0].keycode == 0) {
        /* Legacy fallback: simultaneous keys */
        uint8_t j = 0;
        for (uint8_t i = 0; i < 6 && j < 6; i++) {
            if (keycodes[i] == 0)
                keycodes[i] = macros_list[idx].keys[j++];
        }
        return;
    }

    /* Check if macro has any delay steps */
    bool has_delay = false;
    for (int s = 0; s < MACRO_MAX_STEPS && steps[s].keycode != 0; s++) {
        if (steps[s].keycode == MACRO_DELAY_MARKER) { has_delay = true; break; }
    }

    if (has_delay) {
        /* Sequential: queue for keyboard_task playback */
        pending_macro_idx = idx;
    } else {
        /* Hold: inject keycodes + mods into current report while key is held */
        for (int s = 0; s < MACRO_MAX_STEPS && steps[s].keycode != 0; s++) {
            for (uint8_t i = 0; i < 6; i++) {
                if (keycodes[i] == 0) { keycodes[i] = steps[s].keycode; break; }
            }
            macro_hold_mods |= steps[s].modifier;
        }
    }
}

/* Check if a macro sequence is pending */
bool key_processor_has_pending_macro(void)
{
    return pending_macro_idx >= 0;
}

/* Get and clear the pending macro index */
int16_t key_processor_consume_macro(void)
{
    int16_t idx = pending_macro_idx;
    pending_macro_idx = -1;
    return idx;
}

/* ── Advanced keycode processing ─────────────────────────────────── */

static bool is_advanced_keycode(uint16_t kc)
{
    if (kc <= 0xFF) return false;
    if (kc >= MO_L0 && kc <= TO_L9) return false;
    if (kc >= MACRO_1 && kc <= MACRO_20) return false;
    if (kc >= K_BT_NEXT && kc <= BT_TOGGLE) return false; /* all BT keycodes: 0x2900..0x2F00 */
    return true;
}

static uint8_t process_advanced_key(uint16_t kc, uint8_t row, uint8_t col)
{
    if (K_IS_LT(kc) || K_IS_MT(kc) || K_IS_OSM(kc)) {
        tap_hold_on_press(kc, row, col);
        return 0;
    }
    if (K_IS_TD(kc)) {
        tap_dance_on_press(K_TD_INDEX(kc), row, col);
        return 0;
    }
    if (K_IS_OSL(kc))      { osl_arm(K_OSL_LAYER(kc)); return 0; }
    if (kc == K_CAPS_WORD) { caps_word_toggle(); return 0; }
    if (kc == K_REPEAT)    { return repeat_key_get(); }
    if (kc == K_LEADER)    { leader_start(); return 0; }
    if (kc == K_AUTO_SHIFT_TOGGLE) { return 0; } /* deprecated, kept for compat */
    if (kc == K_GESC) {
        /* Check if any Shift/GUI is in current keycodes OR tap_hold mods */
        uint8_t mods = tap_hold_get_active_mods();
        for (uint8_t i = 0; i < 6; i++) {
            if (keycodes[i] >= 0xE0 && keycodes[i] <= 0xE7)
                mods |= (1 << (keycodes[i] - 0xE0));
        }
        return grave_esc_resolve(mods);
    }
    if (kc == K_LAYER_LOCK){ layer_lock_toggle(); return 0; }
    if (kc == K_TAMA_FEED)     { if (is_new_press(row, col)) tama_engine_action(TAMA2_ACTION_FEED); return 0; }
    if (kc == K_TAMA_PLAY)     { if (is_new_press(row, col)) tama_engine_action(TAMA2_ACTION_PLAY); return 0; }
    if (kc == K_TAMA_SLEEP)    { if (is_new_press(row, col)) tama_engine_action(TAMA2_ACTION_SLEEP); return 0; }
    if (kc == K_TAMA_MEDICINE) { if (is_new_press(row, col)) tama_engine_action(TAMA2_ACTION_MEDICINE); return 0; }
    return 0;
}

/* ── Release detection for tap/hold ──────────────────────────────── */

static bool is_new_press(uint8_t row, uint8_t col)
{
    for (int i = 0; i < 6; i++) {
        if (prev_press_row[i] == row && prev_press_col[i] == col)
            return false;
    }
    return true;
}

static void detect_releases(void)
{
    for (int i = 0; i < 6; i++) {
        if (prev_press_row[i] == INVALID_KEY_POS) continue;
        bool still_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (current_press_row[j] == prev_press_row[i] &&
                current_press_col[j] == prev_press_col[i]) {
                still_pressed = true;
                break;
            }
        }
        if (!still_pressed) {
            tap_hold_on_release(prev_press_row[i], prev_press_col[i]);
            tap_dance_on_release(prev_press_row[i], prev_press_col[i]);
        }
    }
}

/* ── Main report builder ─────────────────────────────────────────── */

void build_keycode_report(void)
{
    tap_injected_slots = 0;
    macro_hold_mods = 0;

    /* Step 1: detect releases and resolve pending tap/holds */
    detect_releases();

    /* Step 2: first pass — resolve MO layers so active_layer is correct for all keys */
    for (uint8_t i = 0; i < 6; i++) {
        if (current_press_col[i] == INVALID_KEY_POS) continue;
        uint16_t kc = keymaps[current_layout][current_press_row[i]][current_press_col[i]];
        apply_momentary_layer(kc, i);
    }

    /* Step 3: determine active layer */
    uint8_t active_layer = current_layout;
    int8_t lt_layer = tap_hold_get_active_layer();
    if (lt_layer >= 0) active_layer = (uint8_t)lt_layer;
    int8_t osl_layer = osl_get_layer();
    if (osl_layer >= 0) active_layer = (uint8_t)osl_layer;

    uint8_t th_mods = tap_hold_get_active_mods();
    bool has_normal_press = false;

    /* Step 4: process each pressed key */
    for (uint8_t i = 0; i < 6; i++) {
        if (current_press_col[i] == INVALID_KEY_POS) {
            keycodes[i] = 0;
            extra_keycodes[i] = 0;
            continue;
        }

        uint8_t row = current_press_row[i];
        uint8_t col = current_press_col[i];
        uint16_t kc = keymaps[active_layer][row][col];

        /* Check tap/hold resolved state */
        bool is_hold = false;
        tap_hold_get_resolved(row, col, &is_hold);
        if (is_hold) {
            extra_keycodes[i] = kc;
            continue;
        }

        /* Advanced keycodes (LT, MT, OSM, OSL, CapsWord, Repeat) */
        if (is_advanced_keycode(kc)) {
            uint8_t hid_kc = process_advanced_key(kc, row, col);
            if (hid_kc != 0) {
                keycodes[i] = hid_kc;
                has_normal_press = true;
            } else {
                extra_keycodes[i] = kc;
            }
            continue;
        }

        /* Leader mode: absorb normal keycodes into sequence */
        if (leader_is_active() && kc <= 0xFF && kc != 0) {
            if (is_new_press(row, col))
                leader_feed((uint8_t)kc);
            extra_keycodes[i] = kc;
            continue;
        }

        /* Legacy: macros, BT, internal functions (MO/TO already resolved in first pass) */
        detect_internal_function(kc);
        expand_macro(kc);

        if (current_row_layer_changer == row && current_col_layer_changer == col) {
            /* Layer changer key — absorbed */
        } else {
            if (kc == K_NO)
                kc = keymaps[last_layer][row][col];
            if (kc > 0xFF) {
                extra_keycodes[i] = kc;
            } else {
                /* Key override: check if modifier+key should be replaced */
                uint8_t override_mod = 0;
                uint8_t override_kc = key_override_check(kc, th_mods, &override_mod);
                if (override_kc != 0) {
                    kc = override_kc;
                    /* TODO: apply override_mod */
                }

                /* Double-tap Shift → Caps Lock (tap sent by keyboard_task) */
                if (is_new_press(row, col) &&
                    (kc == HID_KEY_SHIFT_LEFT || kc == HID_KEY_SHIFT_RIGHT)) {
                    if (shift_double_tap_press()) {
                        extra_keycodes[i] = kc; /* suppress Shift from report */
                        continue;
                    }
                }

                /* Combo deferral: hold back combo candidate keys */
                if (is_new_press(row, col) && combo_should_defer(row, col)) {
                    combo_defer_key(row, col, (uint8_t)kc);
                    extra_keycodes[i] = kc; /* track as held, not in HID report */
                    continue;
                }
                if (combo_is_suppressed(row, col)) {
                    extra_keycodes[i] = kc; /* deferred or active combo, don't emit */
                    continue;
                }

                keycodes[i] = kc;
                has_normal_press = true;
            }
        }
    }

    /* Step 4: interrupt pending tap/holds + track WPM */
    if (has_normal_press) {
        tap_hold_interrupt();
        wpm_record_keypress();
    }

    /* Step 5: apply modifiers (tap/hold + one-shot + LM + macro hold) */
    uint8_t extra_mods = th_mods | osm_consume() | lm_active_mods | macro_hold_mods;

    /* Step 6: caps word + repeat key tracking */
    for (uint8_t i = 0; i < 6; i++) {
        if (keycodes[i] != 0) {
            caps_word_process(&keycodes[i], &extra_mods);
            repeat_key_record(keycodes[i]);
        }
    }

    /* Step 7: inject modifier keycodes */
    for (uint8_t bit = 0; bit < 8; bit++) {
        if (!(extra_mods & (1 << bit))) continue;
        uint8_t mod_hid = HID_KEY_CONTROL_LEFT + bit;
        for (uint8_t i = 0; i < 6; i++) {
            if (keycodes[i] == 0) { keycodes[i] = mod_hid; break; }
        }
    }

    /* Step 8: inject resolved taps (key already released) */
    {
        uint8_t tap_kc;
        while ((tap_kc = tap_hold_consume_tap()) != 0) {
            bool injected = false;
            for (uint8_t i = 0; i < 6; i++) {
                if (keycodes[i] == 0) {
                    keycodes[i] = tap_kc;
                    tap_injected_slots |= (1 << i);
                    has_normal_press = true;
                    injected = true;
                    break;
                }
            }
            if (!injected)
                ESP_LOGW(TAG, "Tap 0x%02X dropped (all slots full)", tap_kc);
        }
    }

    /* Step 9: check combos (with deferral) */
    combo_process(current_press_row, current_press_col);
    {
        /* Inject combo results */
        uint8_t combo_kc, r1, c1, r2, c2;
        while ((combo_kc = combo_consume(&r1, &c1, &r2, &c2)) != 0) {
            for (uint8_t i = 0; i < 6; i++) {
                if (keycodes[i] == 0) {
                    keycodes[i] = combo_kc;
                    has_normal_press = true;
                    break;
                }
            }
        }
        /* Inject expired deferred keys (no combo matched within timeout) */
        uint8_t exp_kc;
        while ((exp_kc = combo_consume_expired()) != 0) {
            for (uint8_t i = 0; i < 6; i++) {
                if (keycodes[i] == 0) {
                    keycodes[i] = exp_kc;
                    has_normal_press = true;
                    break;
                }
            }
        }
    }

    /* Step 10: consume one-shot layer after a real keypress */
    if (osl_layer >= 0 && has_normal_press)
        osl_consume();

    /* Step 10: check if momentary layer key was released */
    if (current_layout != last_layer) {
        bool changer_held = false;
        for (uint8_t i = 0; i < 6; i++) {
            if (current_press_col[i] == current_col_layer_changer &&
                current_press_row[i] == current_row_layer_changer) {
                changer_held = true;
                break;
            }
        }
        if (!changer_held) {
            current_layout = last_layer;
            lm_active_mods = 0;
            layer_changed();
            current_col_layer_changer = INVALID_KEY_POS;
            current_row_layer_changer = INVALID_KEY_POS;
        }
    }

    /* Step 11: save state for next cycle */
    for (uint8_t i = 0; i < 6; i++) {
        prev_press_row[i] = current_press_row[i];
        prev_press_col[i] = current_press_col[i];
    }
}

void process_matrix_changes(void)
{
    if (keypress_internal_function == 0) return;

    bool still_held = false;
    for (uint8_t i = 0; i < 6; i++) {
        if (extra_keycodes[i] == keypress_internal_function) {
            still_held = true;
            extra_keycodes[i] = 0;
            break;
        }
    }

    if (!still_held) {
        dispatch_internal_function();
        apply_toggle_layer(keypress_internal_function);
        keypress_internal_function = 0;
    }
}

bool key_processor_has_tap(void)       { return tap_injected_slots != 0; }

void key_processor_clear_taps(void)
{
    for (uint8_t i = 0; i < 6; i++) {
        if (tap_injected_slots & (1 << i))
            keycodes[i] = 0;
    }
    tap_injected_slots = 0;
}
