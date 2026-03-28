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
#include "hid_bluetooth_manager.h"
#include "tap_hold.h"
#include "key_features.h"
#include "esp_log.h"

static const char *TAG = "KEY_PROC";

/* ── State ───────────────────────────────────────────────────────── */

uint16_t keypress_internal_function = 0;
uint8_t current_row_layer_changer = INVALID_KEY_POS;
uint8_t current_col_layer_changer = INVALID_KEY_POS;
uint16_t extra_keycodes[6] = {0};

static uint8_t prev_press_row[6] = {INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS,
                                     INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};
static uint8_t prev_press_col[6] = {INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS,
                                     INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};
static uint8_t tap_injected_slots = 0;

/* ── Legacy layer switching ──────────────────────────────────────── */

static void apply_momentary_layer(int16_t keycode, uint8_t key_idx)
{
    if (keycode >= MO_L0 && keycode <= MO_L9) {
        last_layer = current_layout;
        current_layout = (keycode - MO_L0) / 256;
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
        if (usb_bl_state == 0 && hid_bluetooth_is_initialized() && hid_bluetooth_is_connected())
            usb_bl_state = 1;
        else
            usb_bl_state = 0;
        km_post_display_update();
        break;
    case BT_TOGGLE:
        km_post_bt_toggle();
        break;
    }
}

/* ── Macro expansion ─────────────────────────────────────────────── */

static void expand_macro(uint16_t keycode)
{
    if (keycode < MACRO_1 || keycode > MACRO_20) return;
    int16_t idx = (keycode - MACRO_1) / 256;
    if (idx >= MAX_MACROS || macros_list[idx].name[0] == '\0') return;
    uint8_t j = 0;
    for (uint8_t i = 0; i < 6 && j < 6; i++) {
        if (keycodes[i] == 0)
            keycodes[i] = macros_list[idx].keys[j++];
    }
}

/* ── Advanced keycode processing ─────────────────────────────────── */

static bool is_advanced_keycode(uint16_t kc)
{
    if (kc <= 0xFF) return false;
    if (kc >= MO_L0 && kc <= TO_L9) return false;
    if (kc >= MACRO_1 && kc <= MACRO_20) return false;
    if (kc == BT_SWITCH_DEVICE || kc == BT_TOGGLE) return false;
    return true;
}

static uint8_t process_advanced_key(uint16_t kc, uint8_t row, uint8_t col)
{
    if (K_IS_LT(kc) || K_IS_MT(kc) || K_IS_OSM(kc)) {
        tap_hold_on_press(kc, row, col);
        return 0;
    }
    if (K_IS_OSL(kc)) { osl_arm(K_OSL_LAYER(kc)); return 0; }
    if (kc == K_CAPS_WORD) { caps_word_toggle(); return 0; }
    if (kc == K_REPEAT)    { return repeat_key_get(); }
    return 0;
}

/* ── Release detection for tap/hold ──────────────────────────────── */

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
        if (!still_pressed)
            tap_hold_on_release(prev_press_row[i], prev_press_col[i]);
    }
}

/* ── Main report builder ─────────────────────────────────────────── */

void build_keycode_report(void)
{
    tap_injected_slots = 0;

    /* Step 1: detect releases and resolve pending tap/holds */
    detect_releases();

    /* Step 2: determine active layer */
    uint8_t active_layer = current_layout;
    int8_t lt_layer = tap_hold_get_active_layer();
    if (lt_layer >= 0) active_layer = (uint8_t)lt_layer;
    int8_t osl_layer = osl_get_layer();
    if (osl_layer >= 0) active_layer = (uint8_t)osl_layer;

    uint8_t th_mods = tap_hold_get_active_mods();
    bool has_normal_press = false;

    /* Step 3: process each pressed key */
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

        /* Legacy: MO layers, TO layers, macros, BT, internal functions */
        apply_momentary_layer(kc, i);
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
                keycodes[i] = kc;
                has_normal_press = true;
            }
        }
    }

    /* Step 4: interrupt pending tap/holds if a normal key was pressed */
    if (has_normal_press)
        tap_hold_interrupt();

    /* Step 5: apply modifiers (tap/hold + one-shot) */
    uint8_t extra_mods = th_mods | osm_consume();

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
            for (uint8_t i = 0; i < 6; i++) {
                if (keycodes[i] == 0) {
                    keycodes[i] = tap_kc;
                    tap_injected_slots |= (1 << i);
                    has_normal_press = true;
                    break;
                }
            }
        }
    }

    /* Step 9: consume one-shot layer after a real keypress */
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
