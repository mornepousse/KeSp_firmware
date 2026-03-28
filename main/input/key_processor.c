/* Key processing: keycode building, layer switching, macros, internal functions,
   tap/hold, one-shot, caps word, repeat key. */
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

uint16_t keypress_internal_function = 0;
uint8_t current_row_layer_changer = INVALID_KEY_POS;
uint8_t current_col_layer_changer = INVALID_KEY_POS;
uint16_t extra_keycodes[6] = {0};

/* Track previous press state for detecting new presses / releases */
static uint8_t prev_press_row[6] = {INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS,
                                     INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};
static uint8_t prev_press_col[6] = {INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS,
                                     INVALID_KEY_POS, INVALID_KEY_POS, INVALID_KEY_POS};

/* Track which keycode slots have injected taps (need press+release) */
static uint8_t tap_injected_slots = 0; /* bitmask: bit i = keycodes[i] has a tap */

/* ── Layer switching ─────────────────────────────────────────────── */

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
        if (current_layout == new_layer)
            current_layout = 0;
        else
            current_layout = new_layer;
        last_layer = current_layout;
        layer_changed();
    }
}

/* ── Internal function detection & dispatch ───────────────────────── */

static bool detect_internal_function(int16_t keycode)
{
    /* Only legacy internal functions (TO_L0..BT_TOGGLE range, not advanced keycodes) */
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
        ESP_LOGI(TAG, "BT_TOGGLE");
        km_post_bt_toggle();
        break;
    }
}

/* ── Macro expansion ─────────────────────────────────────────────── */

static void expand_macro(uint16_t keycode)
{
    if (keycode >= MACRO_1 && keycode <= MACRO_20) {
        int16_t macro_i = (keycode - MACRO_1) / 256;
        if (macro_i < MAX_MACROS && macros_list[macro_i].name[0] != '\0') {
            uint8_t j = 0;
            for (uint8_t i = 0; i < 6; i++) {
                if (keycodes[i] == 0) {
                    keycodes[i] = macros_list[macro_i].keys[j];
                    j++;
                }
            }
        }
    }
}

/* ── Advanced keycode handling ───────────────────────────────────── */

/* Process a single advanced keycode. Returns the HID keycode to emit (0 = absorbed). */
static uint8_t process_advanced_key(uint16_t kc, uint8_t row, uint8_t col, uint8_t slot)
{
    /* Tap/Hold: LT, MT, and OSM all go through the tap/hold engine */
    if (K_IS_LT(kc) || K_IS_MT(kc) || K_IS_OSM(kc)) {
        tap_hold_on_press(kc, row, col);
        return 0; /* absorbed — resolved later */
    }

    /* One-Shot Layer */
    if (K_IS_OSL(kc)) {
        osl_arm(K_OSL_LAYER(kc));
        return 0;
    }

    /* Caps Word toggle */
    if (kc == K_CAPS_WORD) {
        caps_word_toggle();
        return 0;
    }

    /* Repeat Key */
    if (kc == K_REPEAT) {
        return repeat_key_get();
    }

    return 0;
}

/* Detect newly pressed/released keys for tap/hold tracking */
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
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

void build_keycode_report(void)
{
    /* Reset tap tracking for this cycle */
    tap_injected_slots = 0;

    /* Detect key releases for tap/hold */
    detect_releases();

    /* Check tap/hold timeouts */
    tap_hold_tick();

    /* Determine active layer (may be overridden by LT hold or OSL) */
    uint8_t active_layer = current_layout;
    int8_t lt_layer = tap_hold_get_active_layer();
    if (lt_layer >= 0) active_layer = (uint8_t)lt_layer;
    int8_t osl_layer = osl_get_layer();
    if (osl_layer >= 0) active_layer = (uint8_t)osl_layer;

    /* Get tap/hold modifier additions */
    uint8_t th_mods = tap_hold_get_active_mods();

    /* Check if any non-tap-hold key was pressed (interrupts pending) */
    bool has_normal_press = false;

    for (uint8_t i = 0; i < 6; i++) {
        if (current_press_col[i] != INVALID_KEY_POS) {
            uint16_t kc = keymaps[active_layer][current_press_row[i]][current_press_col[i]];

            /* Check if this is a tap/hold key with a resolved action */
            bool is_hold = false;
            uint16_t resolved = tap_hold_get_resolved(current_press_row[i], current_press_col[i], &is_hold);
            if (resolved != 0 && !is_hold) {
                /* Tap resolved — emit the tap keycode */
                kc = resolved;
            } else if (is_hold) {
                /* Hold — modifier/layer already active via tap_hold engine */
                extra_keycodes[i] = kc;
                continue;
            }

            /* Process advanced keycodes */
            if (K_IS_ADVANCED(kc) && !(kc >= MO_L0 && kc <= TO_L9) &&
                !(kc >= MACRO_1 && kc <= MACRO_20) &&
                kc != BT_SWITCH_DEVICE && kc != BT_TOGGLE) {
                uint8_t hid_kc = process_advanced_key(kc, current_press_row[i], current_press_col[i], i);
                if (hid_kc != 0) {
                    keycodes[i] = hid_kc;
                    has_normal_press = true;
                } else {
                    extra_keycodes[i] = kc;
                }
                continue;
            }

            /* Legacy keycode processing */
            apply_momentary_layer(kc, i);
            detect_internal_function(kc);
            expand_macro(kc);

            if (current_row_layer_changer == current_press_row[i] &&
                current_col_layer_changer == current_press_col[i]) {
                /* layer changer key — skip */
            } else {
                if (kc == K_NO)
                    kc = keymaps[last_layer][current_press_row[i]][current_press_col[i]];

                if (kc > 255) {
                    extra_keycodes[i] = kc;
                } else {
                    keycodes[i] = kc;
                    has_normal_press = true;
                }
            }
        } else {
            extra_keycodes[i] = 0;
            keycodes[i] = 0;
        }
    }

    /* Interrupt pending tap/holds if a normal key was pressed */
    if (has_normal_press)
        tap_hold_interrupt();

    /* Apply one-shot modifiers and tap/hold modifiers to the report */
    uint8_t extra_mods = th_mods | osm_consume();

    /* Apply caps word processing */
    for (uint8_t i = 0; i < 6; i++) {
        if (keycodes[i] != 0) {
            caps_word_process(&keycodes[i], &extra_mods);
            repeat_key_record(keycodes[i]);
        }
    }

    /* Inject extra modifiers into keycode slots (they'll be extracted by hid_report) */
    if (extra_mods) {
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (extra_mods & (1 << bit)) {
                /* Map mod bit to HID keycode: bit 0 = LCtrl (0xE0), etc. */
                uint8_t mod_hid = HID_KEY_CONTROL_LEFT + bit;
                /* Find empty slot */
                for (uint8_t i = 0; i < 6; i++) {
                    if (keycodes[i] == 0) { keycodes[i] = mod_hid; break; }
                }
            }
        }
    }

    /* Inject resolved taps (key already released — inject after main loop) */
    {
        uint8_t tap_kc;
        while ((tap_kc = tap_hold_consume_tap()) != 0) {
            for (uint8_t i = 0; i < 6; i++) {
                if (keycodes[i] == 0) {
                    keycodes[i] = tap_kc;
                    has_normal_press = true;
                    tap_injected_slots |= (1 << i);
                    break;
                }
            }
        }
    }

    /* Consume one-shot layer after processing */
    if (osl_layer >= 0 && has_normal_press)
        osl_consume();

    /* Check if momentary layer key was released */
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

    /* Save current press state for next cycle */
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
        ESP_LOGI(TAG, "Executing internal func: 0x%04X", keypress_internal_function);
        dispatch_internal_function();
        apply_toggle_layer(keypress_internal_function);
        keypress_internal_function = 0;
    }
}

bool key_processor_has_tap(void)
{
    return tap_injected_slots != 0;
}

void key_processor_clear_taps(void)
{
    for (uint8_t i = 0; i < 6; i++) {
        if (tap_injected_slots & (1 << i))
            keycodes[i] = 0;
    }
    tap_injected_slots = 0;
}
