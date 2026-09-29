/* Characterization tests for build_keycode_report() and process_matrix_changes().
 *
 * This file compiles key_processor.c (via CMakeLists.txt) and provides stubs
 * for all its hardware/HID dependencies. The goal is to pin down the CURRENT
 * behavior of the pipeline: a refactor that breaks a keystroke will break a test here.
 *
 * Stateful stubs: tap_hold_get_active_mods, tap_hold_get_active_layer,
 *                  osl_get_layer / osl_consume / osl_arm,
 *                  osm_consume, combo_consume.
 * The other stubs are pure no-ops.
 */
#include "test_framework.h"

/* ── Production includes (stubs and globals defined in this TU) ───── */
#include "key_definitions.h"     /* K_MK, K_EXLM, MOD_* — for the Modified Key section */
#include "matrix_scan.h"         /* extern globals : current_press_*, keycodes, etc. */
#include "keymap.h"              /* extern macro_t macros_list[], keymaps[][][] */
#include "key_processor.h"       /* build_keycode_report, process_matrix_changes */
#include "key_features.h"        /* osl_*, osm_*, caps_word_*, key_override_t, etc. */
#include "tap_hold.h"
#include "tap_dance.h"
#include "combo.h"
#include "leader.h"
#include "hid_bluetooth_manager.h"
#include "keyboard_task.h"
#include "keyboard_actions.h"
#include "sec_confirm.h"
#include "chest_gate.h"

/* Test-only seam for chest_gate_oath_nav's CAS loop (I1, review 2026-09-29).
 * chest_gate.c is compiled ONLY for this translation unit (see
 * test/CMakeLists.txt) with -DCHEST_GATE_TEST_SEAM=chest_gate_test_seam_hook;
 * every test leaves s_oath_nav_seam NULL so the seam is a no-op for them,
 * only test_chest_gate_oath_nav_cas_survives_a_concurrent_take arms it. */
static void (*s_oath_nav_seam)(void) = NULL;
void chest_gate_test_seam_hook(void) { if (s_oath_nav_seam) s_oath_nav_seam(); }

/* ── Local HID constants (avoids the key_definitions.h/tinyusb chain) ── */
#define T_KC_A      0x04u
#define T_KC_B      0x05u
#define T_KC_LSHIFT 0xE1u
#define T_KC_RSHIFT 0xE5u
#define T_KC_LCTRL  0xE0u

/* Numeric values of layer keycodes (static const in key_definitions.h) */
#define T_MO_L1  0x0200u   /* MO_L1 */
#define T_MO_L2  0x0300u   /* MO_L2 = MO_L0 + 2*256 */
#define T_TO_L1  0x0C00u   /* TO_L1 = TO_L0 + 256 */
#define T_MACRO_1 0x1500u  /* MACRO_1 */
#define T_OSL_1  0x3101u   /* K_OSL(1) = K_OSL_BASE | 1 */
#define T_MOD_LSFT 0x02u   /* MOD_LSFT */
#define T_K_SEC_CONFIRM 0x3E00u
#define T_K_CHEST_NEXT  0x3E01u
#define T_K_OATH_PREV   0x3E02u
#define T_K_OATH_NEXT   0x3E03u
#define T_K_OATH_CODE   0x3E04u

/* ── Definitions of all extern globals required by key_processor.c ── */

uint16_t  keymaps[LAYERS][MATRIX_ROWS][MATRIX_COLS];
uint8_t   keycodes[6];
uint8_t   current_press_row[6];
uint8_t   current_press_col[6];
uint8_t   current_press_stat[6];
uint8_t   current_layout  = 0;
uint8_t   last_layer      = 0;

uint8_t   MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t   SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
/* array of pointers to 2D arrays, as declared in matrix_scan.h */
uint8_t (*matrix_states[2])[MATRIX_ROWS][MATRIX_COLS] = {
    &MATRIX_STATE, &SLAVE_MATRIX_STATE
};

volatile uint8_t  stat_matrix_changed = 0;
volatile uint8_t  is_layer_changed    = 0;
volatile uint32_t last_activity_time_ms = 0;
uint8_t           usb_bl_state        = 0;

/* macros_list and macros_count are defined in keymap.c (now linked) */
char      default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH];

TaskHandle_t keyboard_task_handle = NULL;
volatile bool matrix_test_mode    = false;

/* ── matrix_scan.h stubs ─────────────────────────────────────────────── */

void     layer_changed(void)              {}
uint32_t get_last_activity_time_ms(void)  { return 0; }
void     matrix_setup(void)               {}
void     rtc_matrix_deinit(void)          {}

/* tap_hold.h: the REAL module is linked (../main/input/tap_hold.c) — no more
 * stubs here. No test in this suite drives tap_hold into an active state (no
 * MT/LT/OSM key pressed), so the real module stays inactive here; the
 * tap/hold logic is exercised in test_tap_hold.c (controllable clock). */

/* tap_dance.h: the REAL module is linked (../main/input/tap_dance.c) — no more
 * stubs. No test in this suite presses a TD key, the real module stays
 * inactive here; the logic is exercised in test_tap_dance.c (shared clock). */

/* combo.h: the REAL module is linked (../main/input/combo.c) — no more stubs.
 * The combo is exercised for real here (test_kp_combo_result_injected) and in
 * test_combo.c; reset via combo_init(). */

/* ── leader.h stubs ──────────────────────────────────────────────────── */

/* leader.h: the REAL module is linked (../main/input/leader.c) — no more stubs.
 * No test in this suite presses K_LEADER, the real module stays inactive
 * here; the matcher is exercised in test_leader.c. */

/* key_features.h: the REAL module is linked (../main/input/key_features.c) —
 * no more stubs here. OSM/OSL/CapsWord/Repeat/GraveEsc/LayerLock/WPM/KeyOverride/
 * TriLayer are exercised for real (key_override guarded by NVS #ifndef TEST_HOST). */


/* ── hid_bluetooth_manager.h stubs ──────────────────────────────────── */

bool hid_bluetooth_is_initialized(void) { return false; }
void bt_next_device(void)               {}
void bt_prev_device(void)               {}
void bt_start_pairing(void)             {}
void bt_disconnect(void)                {}
void save_io_mode(uint8_t m)            { (void)m; }

/* ── keyboard_actions.h + keyboard_task.h stubs ──────────────────────── */

void km_post_display_update(void)  {}
void km_post_display_next(void)    {}
void km_post_bt_toggle(void)       {}
void keyboard_worker_init(void)    {}
void vTaskKeyboard(void *pv)       { (void)pv; }
void keyboard_manager_init(void)   {}

/* keymap.h (NVS): the real implementations come from keymap.c (now linked).
 * No stubs here — the multiple-definition collision was the problem. */

/* ══════════════════════════════════════════════════════════════════════ */
/* Test helpers                                                          */
/* ══════════════════════════════════════════════════════════════════════ */

/* Resets all press slots to "no key" */
static void release_all_keys(void)
{
    for (int i = 0; i < 6; i++) {
        current_press_row[i] = INVALID_KEY_POS;
        current_press_col[i] = INVALID_KEY_POS;
        current_press_stat[i] = 0;
    }
}

/* Presses a key in a given slot */
static void press_key(int slot, uint8_t row, uint8_t col)
{
    current_press_row[slot] = row;
    current_press_col[slot] = col;
    current_press_stat[slot] = 1;
}

/* Checks whether a keycode is present in keycodes[] */
static bool keycode_in_report(uint8_t kc)
{
    for (int i = 0; i < 6; i++)
        if (keycodes[i] == kc) return true;
    return false;
}

/*
 * Full reset between each sub-case:
 * - Clears the keymaps, the keycodes, the layer globals
 * - Resets the stateful stubs to their default state
 * - Runs two idle cycles to clear key_processor's internal statics
 *   (prev_press_row/col, prev_shift_pressed, tap_injected_slots...)
 */
static void reset_kp_state(void)
{
    /* keymaps + press globals */
    memset(keymaps, 0, sizeof(keymaps));
    memset(keycodes, 0, sizeof(keycodes));
    memset(extra_keycodes, 0, sizeof(extra_keycodes));
    memset(macros_list, 0, sizeof(macros_list));

    release_all_keys();
    current_layout = 0;
    last_layer     = 0;

    /* Visible key_processor.h globals */
    keypress_internal_function  = 0;
    current_row_layer_changer   = INVALID_KEY_POS;
    current_col_layer_changer   = INVALID_KEY_POS;

    /* Stateful stubs */
    tap_hold_init();                               /* reinit the real tap/hold */
    tap_dance_init();                              /* reinit the real tap dance */
    (void)osm_consume();                           /* clears the real OSM */
    osl_consume();                                 /* real OSL -> -1 */
    if (caps_word_is_active()) caps_word_toggle(); /* real CapsWord -> off */
    combo_init();                                  /* reinit the real combo */
    leader_init();                                 /* reinit the real leader */

    /* Clears the internal pending macro (static opaque in key_processor.c) */
    (void)key_processor_consume_macro();

    /* Chest gate: nothing pending, no stray press left over from a prior test. */
    chest_gate_publish(0, 0);
    (void)chest_gate_take_press();
    (void)chest_gate_take_mode_next();
    (void)chest_gate_take_oath_nav();
    (void)chest_gate_take_oath_code();

    /* Two idle cycles to flush prev_press_row/col and prev_shift_pressed */
    build_keycode_report();
    build_keycode_report();
    memset(keycodes, 0, sizeof(keycodes));
    memset(extra_keycodes, 0, sizeof(extra_keycodes));
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test cases                                                            */
/* ══════════════════════════════════════════════════════════════════════ */

/* 1. Simple press: standard HID keycode -> appears in keycodes[] */
static void test_kp_simple_press(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_KC_A;     /* A at position (0,0) layer 0 */
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_A), "press A -> 0x04 in keycodes");
}

/* 2. Release: after release, keycode absent from the report */
static void test_kp_simple_release(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_KC_A;
    press_key(0, 0, 0);
    build_keycode_report();    /* cycle 1: A pressed */
    release_all_keys();
    build_keycode_report();    /* cycle 2: nothing pressed */
    TEST_ASSERT(!keycode_in_report(T_KC_A), "release A -> 0x04 absent from the report");
}

/* 3. Modifier key via matrix position -> HID mod in keycodes[] */
static void test_kp_modifier_in_report(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_KC_LSHIFT;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_LSHIFT),
                "LSHIFT in keymaps -> 0xE1 in keycodes");
}

/* 4. MO(layer): press -> current_layout switches to the layer */
static void test_kp_mo_activates_layer(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT_EQ(current_layout, 1, "MO_L1 pressed -> current_layout = 1");
}

/* 5. The MO key itself is absorbed (not in keycodes nor extra_keycodes) */
static void test_kp_mo_key_absorbed(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;
    press_key(0, 0, 0);
    build_keycode_report();
    /* slot 0: MO key absorbed -> keycodes[0] = 0 */
    TEST_ASSERT_EQ(keycodes[0], 0, "MO key absorbed -> keycodes[0] = 0");
}

/* 6. Key co-pressed with MO -> keycode of the active layer */
static void test_kp_mo_active_layer_keycode(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;       /* MO_L1 at (0,0) layer 0 */
    keymaps[1][0][1] = T_KC_B;        /* B at (0,1) layer 1 */
    press_key(0, 0, 0);
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_B),
                "key (0,1) on active layer 1 -> 0x05 (B) in keycodes");
}

/* 7. MO release: after releasing the MO key, back to layer 0 */
static void test_kp_mo_deactivates_on_release(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;
    press_key(0, 0, 0);
    build_keycode_report();    /* press MO cycle -> current_layout = 1 */
    release_all_keys();
    build_keycode_report();    /* release cycle -> restores last_layer */
    TEST_ASSERT_EQ(current_layout, 0,
                   "release MO_L1 -> current_layout = 0");
}

/* 8. TO(layer): press then release -> process_matrix_changes switches the layer */
static void test_kp_to_toggle_on(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_TO_L1;

    /* Cycle 1: press TO_L1 */
    press_key(0, 0, 0);
    build_keycode_report();
    process_matrix_changes();   /* key still held -> no toggle */

    /* Cycle 2: release */
    release_all_keys();
    build_keycode_report();
    process_matrix_changes();   /* key released -> apply_toggle_layer */

    TEST_ASSERT_EQ(current_layout, 1, "TO_L1 press+release -> current_layout = 1");
}

/* 9. K_NO: transparent key on the active layer -> fallback to last_layer */
static void test_kp_kno_fallback(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;       /* MO_L1 activates layer 1 */
    keymaps[0][0][1] = T_KC_A;        /* A at (0,1) layer 0 */
    keymaps[1][0][1] = 0x0000u;       /* K_NO at (0,1) layer 1 -> fallback to layer 0 */

    press_key(0, 0, 0);               /* slot 0: MO */
    press_key(1, 0, 1);               /* slot 1: key with K_NO on L1 */
    build_keycode_report();

    /* key (0,1) must produce T_KC_A (from last_layer = 0) */
    TEST_ASSERT(keycode_in_report(T_KC_A),
                "K_NO on active layer -> fallback layer 0 = A (0x04)");
}

/* 10. OSM (QMK logic, M5): the one-shot mod applies to the NEXT keypress and
 * is only consumed there — a cycle with no keypress (release/idle) leaves it armed. */
static void test_kp_osm_applies_to_next_press(void)
{
    reset_kp_state();
    osm_arm(T_MOD_LSFT);          /* MOD_LSFT = bit 1 -> HID_KEY_CONTROL_LEFT + 1 = 0xE1 */
    /* Cycle with no keypress: the OSM stays armed, NOT injected */
    build_keycode_report();
    TEST_ASSERT(!keycode_in_report(T_KC_LSHIFT),
                "OSM with no keypress -> mod NOT injected (stays armed, M5/QMK)");
    TEST_ASSERT(osm_is_active(), "OSM still armed after a cycle with no keypress");
    /* Target keypress: it receives the OSM mod, which is then consumed */
    keymaps[0][0][0] = T_KC_A;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_A), "target keypress present");
    TEST_ASSERT((key_processor_report_mods() & 0x02) != 0,
                "OSM applied to the keypress (modifier byte, carried outside keycodes[] — M7)");
    TEST_ASSERT(!osm_is_active(), "OSM consumed by the keypress");
}

/* 11a. Real OSL arm: press K_OSL(1) -> the real osl_arm(1) is called */
static void test_kp_osl_arm_called(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_OSL_1;       /* K_OSL(1) = 0x3101 */
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT_EQ(osl_get_layer(), 1,
                   "press K_OSL(1) -> osl armed on layer 1");
}

/* 11b. Real OSL active layer: osl_arm(2) -> active_layer overwritten to 2 */
static void test_kp_osl_active_layer(void)
{
    reset_kp_state();
    osl_arm(2);                    /* OSL layer 2 armed (real module) */
    keymaps[2][0][0] = T_KC_B;    /* B on layer 2 */
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_B),
                "osl_get_layer() = 2 -> key (0,0) resolves on layer 2 = B (0x05)");
}

/* 14. Real combo: 2 keys of a combo pressed together -> result injected.
 *     The pipeline defers the 2 keys (combo_should_defer/defer_key), then
 *     combo_process sees both present+deferred -> resolves -> consume. */
static void test_kp_combo_result_injected(void)
{
    reset_kp_state();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = T_KC_A };
    combo_set(0, &cfg);
    keymaps[0][0][0] = 0x0A;   /* G — arbitrary non-zero keycode */
    keymaps[0][0][1] = 0x0B;   /* H */
    press_key(0, 0, 0);
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_A),
                "combo (0,0)+(0,1) pressed -> A (0x04) injected into keycodes");
}

/* 15. Several simultaneous keys -> all in keycodes[] */
static void test_kp_multi_key_press(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_KC_A;
    keymaps[0][0][1] = T_KC_B;
    keymaps[0][1][0] = 0x06u; /* C */
    press_key(0, 0, 0);
    press_key(1, 0, 1);
    press_key(2, 1, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_A), "multi-press: A (0x04) in report");
    TEST_ASSERT(keycode_in_report(T_KC_B), "multi-press: B (0x05) in report");
    TEST_ASSERT(keycode_in_report(0x06u),  "multi-press: C (0x06) in report");
}

/* 16. MACRO + TO co-pressed: the MACRO key must NOT occupy the
 *     keypress_internal_function slot (reserved for TO/BT), otherwise a
 *     simultaneous TO is never toggled. */
static void test_kp_macro_does_not_starve_to(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MACRO_1;   /* slot 0: macro (empty -> expand no-op) */
    keymaps[0][0][1] = T_TO_L1;     /* slot 1: TO_L1 */

    /* Press cycle: both keys held */
    press_key(0, 0, 0);
    press_key(1, 0, 1);
    build_keycode_report();
    process_matrix_changes();       /* still held -> no toggle */

    /* Release cycle: TO released -> toggle must apply */
    release_all_keys();
    build_keycode_report();
    process_matrix_changes();

    TEST_ASSERT_EQ(current_layout, 1,
                   "MACRO + TO co-pressed -> TO toggled (macro no longer starves the internal slot)");
}

/* 18. Simultaneous double MO: each MO key is resolved from the active layer
 *     at the START of the cycle, not from the layer mutated by an MO processed
 *     earlier in the loop. Without this, iteration order decides which
 *     layer is read (ordering bug). Behavior aligned with the intent
 *     documented in step 2. */
static void test_kp_double_mo_resolves_from_base_layer(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;   /* slot 0: MO_L1 on the base */
    keymaps[0][0][1] = T_MO_L2;   /* slot 1: MO_L2 on the base */
    keymaps[1][0][1] = T_KC_A;    /* on layer 1, (0,1) = normal key (trap) */

    press_key(0, 0, 0);
    press_key(1, 0, 1);
    build_keycode_report();

    /* Both MOs read from layer 0 -> MO_L2 recognized -> final layer 2.
     * (With the ordering bug: slot1 read on layer 1 = normal key -> layer 1.) */
    TEST_ASSERT_EQ(current_layout, 2,
                   "simultaneous double MO: resolution from the base layer (-> layer 2)");
}

/* 19. K_SEC_CONFIRM : press → authorize pending request, absorbed (not emitted) */
/* A key keeps the keycode of the layer it was PRESSED on until it is
 * released — even if the layer key goes away first. The bug (2026-09-20):
 * arrows on MO(2), Right arrow on the 'U' position; releasing MO(2) a hair
 * before the arrow re-resolved the held key on the base layer and typed a
 * 'u'. Same rule as QMK: the layer is latched at press time. */
#define T_KC_U      0x18u
#define T_KC_RIGHT  0x4Fu
static void test_kp_held_key_keeps_the_layer_it_was_pressed_on(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;
    keymaps[0][0][1] = T_KC_U;        /* base: U */
    keymaps[1][0][1] = T_KC_RIGHT;    /* layer 1: Right arrow on the same key */
    press_key(0, 0, 0);
    build_keycode_report();           /* MO held -> layer 1 */
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_RIGHT), "pressed under MO: Right arrow");
    /* MO released first, the arrow key still held */
    current_press_row[0] = INVALID_KEY_POS; current_press_col[0] = INVALID_KEY_POS; current_press_stat[0] = 0;
    build_keycode_report();           /* release cycle: the layer is restored at its end */
    TEST_ASSERT_EQ(current_layout, 0, "MO released -> back to layer 0");
    build_keycode_report();           /* next cycle, arrow still held, layer 0 active */
    TEST_ASSERT(keycode_in_report(T_KC_RIGHT), "held key STAYS Right arrow after the MO release");
    TEST_ASSERT(!keycode_in_report(T_KC_U), "no 'u' typed by the layer change");
    /* release the arrow, press the same key again: now it is a U */
    release_all_keys();
    build_keycode_report();
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_U), "a new press on layer 0 is a U");
}

/* The mirror case: a key held BEFORE the MO keeps its base keycode while the
 * layer is active — its assignment does not change under the finger. */
static void test_kp_key_pressed_before_mo_keeps_the_base_layer(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;
    keymaps[0][0][1] = T_KC_U;
    keymaps[1][0][1] = T_KC_RIGHT;
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_U), "pressed on base: U");
    press_key(0, 0, 0);
    build_keycode_report();           /* MO pressed while U is held */
    TEST_ASSERT_EQ(current_layout, 1, "MO held -> layer 1");
    TEST_ASSERT(keycode_in_report(T_KC_U), "the held key stays a U");
    TEST_ASSERT(!keycode_in_report(T_KC_RIGHT), "it does not turn into Right arrow");
}

/* LT resolved as a hold BY the key pressed during its tapping term: that key
 * is read on the LT layer from its first report — before, the base character
 * was typed once, then the layer one. */
static void test_kp_key_that_resolves_an_lt_hold_is_on_the_lt_layer(void)
{
    reset_kp_state();
    keymaps[0][0][0] = 0x4000u | (1u << 8) | 0x2Cu;   /* K_LT(1, Space) */
    keymaps[0][0][1] = 0x06u;   /* C on the base layer */
    keymaps[1][0][1] = T_KC_B;  /* B on layer 1 */
    press_key(0, 0, 0);
    build_keycode_report();
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 1, "the second key resolves the LT as a hold");
    TEST_ASSERT(keycode_in_report(T_KC_B), "and it is read on the LT layer: B");
    TEST_ASSERT(!keycode_in_report(0x06), "never a C");
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_B) && !keycode_in_report(0x06), "still B on the next cycle");
    /* LT released first, the key still held: it stays a B */
    current_press_row[0] = INVALID_KEY_POS; current_press_col[0] = INVALID_KEY_POS; current_press_stat[0] = 0;
    build_keycode_report();
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_B) && !keycode_in_report(0x06), "LT released, the held key stays a B");
}

/* I2 (review 2026-09-29, fix round 1): when the SAME cycle that resolves an
 * LT hold reads its resolving key on the LT layer, and that key is a
 * K_IS_SEC action (K_SEC_CONFIRM/K_CHEST_NEXT/K_OATH_*) rather than a plain
 * HID code, the re-latch loop (~line 492) only knew how to fold a plain
 * keycode back in — for an advanced keycode it just zeroed keycodes[i] and
 * moved on, never calling process_advanced_key. On every later cycle
 * is_new_press(row,col) is already false for that slot (latched as
 * "previously pressed" at Step 11 of THIS same cycle), so the action's own
 * is_new_press gate never opens again: the press is silently swallowed
 * forever. This is the RED test: before the fix, the OATH request never
 * queues. */
static void test_kp_lt_resolves_oath_next_on_its_press_cycle(void)
{
    reset_kp_state();
    keymaps[0][0][0] = 0x4000u | (1u << 8) | 0x2Cu;   /* K_LT(1, Space) */
    keymaps[0][0][1] = 0x06u;          /* base: C */
    keymaps[1][0][1] = T_K_OATH_NEXT;  /* layer 1: OATH Next */
    press_key(0, 0, 0);
    build_keycode_report();
    press_key(1, 0, 1);
    build_keycode_report();            /* THIS cycle resolves the LT as a hold */
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 1, "the second key resolves the LT as a hold");
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), 1,
                   "K_OATH_NEXT fires on the very cycle its LT resolves, not never");
    TEST_ASSERT(!keycode_in_report(0x06), "never a C");
}

/* Same bug, K_SEC_CONFIRM from a LOCAL (left) column: one confirm, on the
 * resolving cycle. sec_confirm_from_local's gate and chest_gate_press()'s
 * ordering are untouched — this goes through the same process_advanced_key
 * branch as every other K_SEC_CONFIRM press. */
static void test_kp_lt_resolves_sec_confirm_from_left_confirms_once(void)
{
    reset_kp_state();
    chest_gate_publish(1, 0);                          /* chest: op 1 pending */
    keymaps[0][0][0] = 0x4000u | (1u << 8) | 0x2Cu;    /* K_LT(1, Space) */
    keymaps[0][0][1] = 0x06u;                           /* base: C */
    keymaps[1][0][1] = T_K_SEC_CONFIRM;                 /* layer 1, LOCAL column 1 */
    press_key(0, 0, 0);
    build_keycode_report();
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 1, "the second key resolves the LT as a hold");
    TEST_ASSERT_EQ(chest_gate_take_press(), CHEST_TAG(1, 0),
                   "K_SEC_CONFIRM from the left confirms on the resolving cycle");
    chest_gate_publish(0, 0);
}

/* Mirror: K_SEC_CONFIRM from a REMOTE (right-half) column (>=
 * SEC_CONFIRM_LOCAL_COLS, 4 in this host build) confirms nothing, even
 * reached through an LT resolution — the left-only filter survives the fix. */
static void test_kp_lt_resolves_sec_confirm_from_right_confirms_nothing(void)
{
    reset_kp_state();
    chest_gate_publish(1, 0);
    keymaps[0][0][0] = 0x4000u | (1u << 8) | 0x2Cu;    /* K_LT(1, Space) */
    keymaps[0][0][5] = 0x06u;                           /* base col 5 (remote): C */
    keymaps[1][0][5] = T_K_SEC_CONFIRM;                 /* layer 1, REMOTE column 5 */
    press_key(0, 0, 0);
    build_keycode_report();
    press_key(1, 0, 5);
    build_keycode_report();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 1, "the second key resolves the LT as a hold");
    TEST_ASSERT_EQ(chest_gate_take_press(), 0,
                   "no confirm from a remote column, even via an LT resolution");
    chest_gate_publish(0, 0);
}

static void test_kp_sec_confirm_authorizes(void)
{
    reset_kp_state();
    sec_confirm_reset();
    sec_confirm_arm(2, 0);                 /* pending request for slot 2 */
    keymaps[0][0][0] = T_K_SEC_CONFIRM;
    press_key(0, 0, 0);
    build_keycode_report();

    uint8_t slot = 0xFF;
    sec_confirm_state_t st = sec_confirm_poll(1, &slot);
    TEST_ASSERT_EQ(st, SEC_CONFIRM_AUTHORIZED, "K_SEC_CONFIRM press authorizes pending");
    TEST_ASSERT_EQ(slot, 2, "authorized slot = 2");
    TEST_ASSERT_EQ(keycodes[0], 0, "K_SEC_CONFIRM absorbed (not in HID report)");
}

/* A chest operation pending: the press goes to the chest, not the local gate. */
static void test_kp_sec_confirm_routes_to_chest(void)
{
    reset_kp_state();
    sec_confirm_reset();
    sec_confirm_arm(2, 0);                 /* a local request exists too */
    chest_gate_publish(1, 0);                 /* chest: SIGN pending */
    keymaps[0][0][0] = T_K_SEC_CONFIRM;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_press(), 1, "press queued for the chest (op 1)");
    TEST_ASSERT_EQ(chest_gate_take_press(), 0, "exactly one press");
    uint8_t slot = 0xFF;
    TEST_ASSERT(sec_confirm_poll(1, &slot) != SEC_CONFIRM_AUTHORIZED, "local gate NOT authorized");
    TEST_ASSERT_EQ(keycodes[0], 0, "absorbed");
    chest_gate_publish(0, 0);
}

/* Held key: one press, not one per scan (review focus). */
static void test_kp_sec_confirm_held_confirms_chest_once(void)
{
    reset_kp_state();
    chest_gate_publish(1, 0);
    keymaps[0][0][0] = T_K_SEC_CONFIRM;
    press_key(0, 0, 0);
    build_keycode_report();
    build_keycode_report();
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_press(), 1, "first cycle queues (op 1)");
    TEST_ASSERT_EQ(chest_gate_take_press(), 0, "held key does not queue again");
    chest_gate_publish(0, 0);
}

/* Security invariant: only a NEW physical press may confirm — a chest op
 * that arrives while K_SEC_CONFIRM is already held must NOT be confirmed by
 * that hold. Catches `&&` operand reordering: `!chest_gate_press() &&
 * is_new_press(...)` would call chest_gate_press() on every scan and this
 * test would go red. */
static void test_kp_sec_confirm_held_before_chest_op_does_not_confirm(void)
{
    reset_kp_state();
    chest_gate_publish(0, 0);
    keymaps[0][0][0] = T_K_SEC_CONFIRM;
    press_key(0, 0, 0);
    build_keycode_report();                /* new press, nothing pending: goes to the local gate */

    chest_gate_publish(1, 0);                 /* op arrives while the key is STILL held */
    build_keycode_report();
    build_keycode_report();                /* no release, no new press */
    TEST_ASSERT_EQ(chest_gate_take_press(), 0, "a chest op arriving mid-hold is not confirmed by the held key");
    chest_gate_publish(0, 0);
}

/* chest_gate_press() stores the op it SAW at press time, not a live pointer
 * to the gate: take_press must hand back that recorded op even though the
 * gate itself may have changed by the time the link task reads it. And once
 * the chest clears its op, a fresh physical press (release + new press)
 * finds nothing pending and goes to the local gate, not the chest — no
 * leftover op to be confirmed against. */
static void test_kp_sec_confirm_press_records_the_seen_op(void)
{
    reset_kp_state();
    chest_gate_publish(7, 0);                 /* chest: OATH_CODE pending */
    keymaps[0][0][0] = T_K_SEC_CONFIRM;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_press(), 7, "press records the op seen at press time");

    chest_gate_publish(0, 0);                 /* the chest cleared its op */
    release_all_keys();
    build_keycode_report();                /* release cycle */
    press_key(0, 0, 0);
    build_keycode_report();                /* new press, nothing pending: local gate */
    TEST_ASSERT_EQ(chest_gate_take_press(), 0, "nothing pending: goes to the local gate, not the chest");
    chest_gate_publish(0, 0);
}

/* The press carries the ARMING seen on screen, not just the op code. */
static void test_kp_sec_confirm_records_the_instance(void)
{
    reset_kp_state();
    chest_gate_publish(1, 7);
    keymaps[0][0][0] = T_K_SEC_CONFIRM;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_press(), CHEST_TAG(1, 7), "op 1 instance 7 recorded");
    chest_gate_publish(0, 0);
}

/* Left half only: a K_SEC_CONFIRM at a remote (right-half) column reaches
 * neither the chest nor the local gate. The host build sets
 * SEC_CONFIRM_LOCAL_COLS=4 so columns >= 4 are "remote". */
static void test_kp_sec_confirm_ignored_from_the_right_half(void)
{
    reset_kp_state();
    sec_confirm_reset();
    sec_confirm_arm(2, 0);
    chest_gate_publish(1, 3);
    keymaps[0][0][5] = T_K_SEC_CONFIRM;
    press_key(0, 0, 5);
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_press(), 0, "no chest press from the right half");
    uint8_t slot = 0xFF;
    TEST_ASSERT(sec_confirm_poll(1, &slot) != SEC_CONFIRM_AUTHORIZED, "local gate not authorized either");
    chest_gate_publish(0, 0);
    keymaps[0][0][5] = 0;
}

static void test_kp_chest_next_requests_a_mode_change_once(void)
{
    reset_kp_state();
    (void)chest_gate_take_mode_next();
    keymaps[0][0][0] = T_K_CHEST_NEXT;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(chest_gate_take_mode_next(), "a new press requests the next mode");
    build_keycode_report();                /* still held, no release */
    TEST_ASSERT(!chest_gate_take_mode_next(), "a held key does not request again");
    TEST_ASSERT_EQ(keycodes[0], 0, "absorbed, not typed");
}

/* K_OATH_NEXT: one new press -> +1 queued, and ONLY the nav channel (M2,
 * review 2026-09-29: catches NEXT also requesting a code or a mode change);
 * a held key does not queue again; absorbed (not typed). Mirrors
 * test_kp_chest_next_requests_a_mode_change_once. */
static void test_kp_oath_next_requests_nav_once(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_K_OATH_NEXT;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), 1, "a new press queues +1");
    TEST_ASSERT(!chest_gate_take_oath_code(), "NEXT must not also request a code");
    TEST_ASSERT(!chest_gate_take_mode_next(), "NEXT must not also request a mode change");
    TEST_ASSERT_EQ(chest_gate_take_press(), 0, "NEXT must not also confirm the chest");
    press_key(0, 0, 0);
    build_keycode_report();                /* still held, no release */
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), 0, "a held key does not queue again");
    TEST_ASSERT_EQ(keycodes[0], 0, "absorbed, not typed");
}

/* K_OATH_PREV: symmetric, -1 queued, same cross-channel isolation. */
static void test_kp_oath_prev_requests_nav_once(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_K_OATH_PREV;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), -1, "a new press queues -1");
    TEST_ASSERT(!chest_gate_take_oath_code(), "PREV must not also request a code");
    TEST_ASSERT(!chest_gate_take_mode_next(), "PREV must not also request a mode change");
    TEST_ASSERT_EQ(chest_gate_take_press(), 0, "PREV must not also confirm the chest");
    press_key(0, 0, 0);
    build_keycode_report();                /* still held, no release */
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), 0, "a held key does not queue again");
    TEST_ASSERT_EQ(keycodes[0], 0, "absorbed, not typed");
}

/* K_OATH_CODE: one new press -> requested once, and ONLY the code channel
 * (M2: catches CODE also pushing a nav step); a held key does not
 * re-request; absorbed. */
static void test_kp_oath_code_requests_once(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_K_OATH_CODE;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(chest_gate_take_oath_code(), "a new press requests a code");
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), 0, "CODE must not also queue a nav step");
    TEST_ASSERT(!chest_gate_take_mode_next(), "CODE must not also request a mode change");
    TEST_ASSERT_EQ(chest_gate_take_press(), 0, "CODE must not also confirm the chest");
    press_key(0, 0, 0);
    build_keycode_report();                /* still held, no release */
    TEST_ASSERT(!chest_gate_take_oath_code(), "a held key does not request again");
    TEST_ASSERT_EQ(keycodes[0], 0, "absorbed, not typed");
}

/* M1 (review 2026-09-29): none of the three OATH keys is left-only —
 * browsing and asking for a code carry no authority, unlike K_SEC_CONFIRM.
 * A remote (right-half) column (>= SEC_CONFIRM_LOCAL_COLS, 4 in this host
 * build) is accepted the same as a local one. Mutant to kill: NEXT (or
 * PREV/CODE) gated by sec_confirm_from_local like K_SEC_CONFIRM. */
static void test_kp_oath_keys_accepted_from_the_right_half(void)
{
    reset_kp_state();
    keymaps[0][0][5] = T_K_OATH_NEXT;
    press_key(0, 0, 5);
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), 1,
                   "K_OATH_NEXT from a remote column still queues +1");
    release_all_keys();
    build_keycode_report();

    keymaps[0][0][5] = T_K_OATH_PREV;
    press_key(0, 0, 5);
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), -1,
                   "K_OATH_PREV from a remote column still queues -1");
    release_all_keys();
    build_keycode_report();

    keymaps[0][0][5] = T_K_OATH_CODE;
    press_key(0, 0, 5);
    build_keycode_report();
    TEST_ASSERT(chest_gate_take_oath_code(),
                "K_OATH_CODE from a remote column still requests a code");
    keymaps[0][0][5] = 0;
}

/* NEXT, NEXT, PREV, each a separate physical press (release between), then
 * one take == +1 and a second take == 0 (cleared). */
static void test_kp_oath_nav_sequence_take_once(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_K_OATH_NEXT;
    press_key(0, 0, 0);
    build_keycode_report();                /* NEXT: +1 */
    release_all_keys();
    build_keycode_report();
    press_key(0, 0, 0);
    build_keycode_report();                /* NEXT: +1 (total +2) */
    release_all_keys();
    build_keycode_report();
    keymaps[0][0][0] = T_K_OATH_PREV;
    press_key(0, 0, 0);
    build_keycode_report();                /* PREV: -1 (total +1) */
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), 1, "NEXT,NEXT,PREV -> +1 queued");
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), 0, "a second take is cleared");
}

/* The queued delta saturates at +16, even with more presses. */
static void test_kp_oath_nav_saturates_positive(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_K_OATH_NEXT;
    for (int i = 0; i < 20; i++) {
        press_key(0, 0, 0);
        build_keycode_report();
        release_all_keys();
        build_keycode_report();
    }
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), 16, "20 NEXT presses saturate at +16");
}

/* Symmetric: saturates at -16. */
static void test_kp_oath_nav_saturates_negative(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_K_OATH_PREV;
    for (int i = 0; i < 20; i++) {
        press_key(0, 0, 0);
        build_keycode_report();
        release_all_keys();
        build_keycode_report();
    }
    TEST_ASSERT_EQ(chest_gate_take_oath_nav(), -16, "20 PREV presses saturate at -16");
}

/* K_OATH_CODE pressed twice (two distinct physical presses) before any
 * take(): the request is a flag, not a counter -> one take true, then
 * false. */
static void test_kp_oath_code_twice_before_take_yields_one(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_K_OATH_CODE;
    press_key(0, 0, 0);
    build_keycode_report();                /* press 1 */
    release_all_keys();
    build_keycode_report();
    press_key(0, 0, 0);
    build_keycode_report();                /* press 2, still before any take */
    TEST_ASSERT(chest_gate_take_oath_code(), "first take: true");
    TEST_ASSERT(!chest_gate_take_oath_code(), "second take: false (cleared)");
}

/* Race oracle for the CAS loop in chest_gate_oath_nav (I1, review
 * 2026-09-29): a concurrent chest_gate_take_oath_nav() landing between the
 * loop's load and its store must never lose or duplicate a step. Seeds the
 * accumulator to v, then arms the seam so that mid-attempt a "link task"
 * takes the seeded v and resets to 0; the writer's compare_exchange must
 * then retry against the new value and land on 1 (0 + the racing +1).
 * taken + remaining must equal v + 1 — the plain load/saturate/store this
 * replaced gives 2v+1 instead (the take's v survives the race AND the
 * writer's stale store overwrites the take's reset to 0 with v+1). */
static int8_t s_seam_taken;
static bool   s_seam_fired;
static void oath_nav_race_seam(void)
{
    if (s_seam_fired) return;      /* interleave once: the first CAS attempt */
    s_seam_fired = true;
    s_seam_taken = chest_gate_take_oath_nav();
}

static void test_chest_gate_oath_nav_cas_survives_a_concurrent_take(void)
{
    reset_kp_state();
    const int8_t v = 5;
    for (int8_t i = 0; i < v; i++) chest_gate_oath_nav(1);   /* seed to v */

    s_seam_fired = false;
    s_seam_taken = 0;
    s_oath_nav_seam = oath_nav_race_seam;
    chest_gate_oath_nav(1);                    /* the racing writer: +1 */
    s_oath_nav_seam = NULL;

    int8_t remaining = chest_gate_take_oath_nav();
    TEST_ASSERT_EQ((int)s_seam_taken + (int)remaining, (int)v + 1,
                   "CAS: no step lost or duplicated across a concurrent take");
}

static void test_sec_confirm_from_local(void)
{
    TEST_ASSERT(sec_confirm_from_local(0, 7, 14), "left col 0");
    TEST_ASSERT(sec_confirm_from_local(6, 7, 14), "left col 6");
    TEST_ASSERT(!sec_confirm_from_local(7, 7, 14), "right col 7");
    TEST_ASSERT(!sec_confirm_from_local(13, 7, 14), "right col 13");
    TEST_ASSERT(sec_confirm_from_local(12, 13, 13), "non-split board: every column");
    TEST_ASSERT(sec_confirm_from_local(13, 13, 13), "non-split: a column at/after local_cols is still local");
}

/* ══════════════════════════════════════════════════════════════════════ */
/* expand_macro tests via the pipeline                                   */
/* ══════════════════════════════════════════════════════════════════════ */

/* 20. Inline macro (no MACRO_DELAY_MARKER step): the steps' keycodes
 *     are injected into keycodes[] by expand_macro from build_keycode_report. */
static void test_kp_macro_inline_injects_steps(void)
{
    reset_kp_state();
    macros_list[0].name[0] = 'm';
    macros_list[0].steps[0].keycode   = T_KC_A;
    macros_list[0].steps[0].modifier  = 0;
    /* steps[1].keycode = 0 -> end of sequence (already 0 via memset in reset_kp_state) */
    keymaps[0][0][0] = T_MACRO_1;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_A),
                "expand_macro inline: step.keycode injected into keycodes[]");
}

/* 21. Macro with name[0]=='\0' -> guard active, no keycode injected (no-op) */
static void test_kp_macro_empty_name_noop(void)
{
    reset_kp_state();
    /* macros_list[0].name[0] = '\0' -> already via memset in reset_kp_state */
    macros_list[0].steps[0].keycode = T_KC_A;  /* must NOT be injected */
    keymaps[0][0][0] = T_MACRO_1;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(!keycode_in_report(T_KC_A),
                "expand_macro: empty name -> no-op, no keycode injected");
}

/* 22. Sequential macro (MACRO_DELAY_MARKER step present) -> pending_macro_idx
 *     set; consumable via key_processor_consume_macro(). */
static void test_kp_macro_delay_sets_pending(void)
{
    reset_kp_state();
    macros_list[0].name[0]            = 's';
    macros_list[0].steps[0].keycode   = T_KC_A;
    macros_list[0].steps[0].modifier  = 0;
    macros_list[0].steps[1].keycode   = MACRO_DELAY_MARKER;
    macros_list[0].steps[1].modifier  = 5; /* 50ms */
    macros_list[0].steps[2].keycode   = T_KC_B;
    macros_list[0].steps[2].modifier  = 0;
    /* steps[3].keycode = 0 -> end (via memset) */
    keymaps[0][0][0] = T_MACRO_1;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(key_processor_has_pending_macro(),
                "expand_macro delay: pending_macro_idx set");
    TEST_ASSERT_EQ(key_processor_consume_macro(), 0,
                   "consume_macro returns index 0");
    TEST_ASSERT(!key_processor_has_pending_macro(),
                "after consume: pending_macro_idx empty");
}

/* 22b. HELD delay macro: does not re-arm on the next scan while the key
 * is held (audit M8 — otherwise replays in a loop on every matrix change). */
static void test_kp_macro_delay_no_retrigger_held(void)
{
    reset_kp_state();
    macros_list[0].name[0]            = 's';
    macros_list[0].steps[0].keycode   = T_KC_A;
    macros_list[0].steps[1].keycode   = MACRO_DELAY_MARKER;
    macros_list[0].steps[1].modifier  = 5;
    macros_list[0].steps[2].keycode   = T_KC_B;
    keymaps[0][0][0] = T_MACRO_1;
    press_key(0, 0, 0);
    build_keycode_report();                          /* 1st press -> pending */
    TEST_ASSERT(key_processor_has_pending_macro(), "1st delay-macro press -> pending");
    (void)key_processor_consume_macro();             /* keyboard_task plays it -> consumes */
    build_keycode_report();                          /* key still held */
    TEST_ASSERT(!key_processor_has_pending_macro(),
                "held delay-macro -> no re-arm on the next scan (M8)");
}

/* 23. Legacy macro (steps[0].keycode == 0): uses keys[] instead of steps[].
 *     keys[0] must appear in keycodes[] after build_keycode_report(). */
static void test_kp_macro_legacy_injects_keys(void)
{
    reset_kp_state();
    macros_list[0].name[0] = 'l';
    /* steps[0].keycode = 0 -> legacy path (already 0 via memset in reset_kp_state) */
    macros_list[0].keys[0] = T_KC_A;
    keymaps[0][0][0] = T_MACRO_1;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_A),
                "expand_macro legacy (steps[0]==0): keys[0] injected into keycodes[]");
}

/* Key override triggered by a PHYSICAL mod + result_mod applied + trigger
 * mod removed (audit E6: the override only saw th_mods and dropped
 * result_mod -> never triggered in normal usage). */
static void test_kp_key_override_physical_mod(void)
{
    reset_kp_state();
    key_override_init();
    key_override_t ov = { .trigger_key = T_KC_A, .trigger_mod = 0x02 /* LShift */,
                          .result_key = T_KC_B,  .result_mod  = 0x01 /* LCtrl  */ };
    key_override_set(0, &ov);
    keymaps[0][0][0] = T_KC_LSHIFT;   /* physical Shift at (0,0) */
    keymaps[0][0][1] = T_KC_A;        /* A at (0,1) */
    press_key(0, 0, 0);
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_B),
                "override: physical Shift + A -> B emitted (triggered by physical mod)");
    TEST_ASSERT(!keycode_in_report(T_KC_A),
                "override: A replaced, absent from the report");
    TEST_ASSERT(!keycode_in_report(T_KC_LSHIFT),
                "override: trigger Shift removed from the report");
    TEST_ASSERT((key_processor_report_mods() & 0x01) != 0,
                "override: result_mod (Ctrl) applied (modifier byte — M7)");
    key_override_init();   /* leaves it clean for the following suites */
}

/* Mods carried outside keycodes[]: a mod (OSM Shift) survives even when the
 * 6 slots are full of real keys — before, the mod would steal/lose a slot (audit M7). */
static void test_kp_mod_survives_full_report(void)
{
    reset_kp_state();
    /* 6 normal keys -> fill keycodes[] (row 0, cols 0..5) */
    for (uint8_t c = 0; c < 6; c++) {
        keymaps[0][0][c] = (uint16_t)(T_KC_A + c);
        press_key(c, 0, c);
    }
    osm_arm(T_MOD_LSFT);          /* a mod via OSM -> extra_mods */
    build_keycode_report();
    int keys = 0;
    for (int i = 0; i < 6; i++) if (keycodes[i] != 0) keys++;
    TEST_ASSERT_EQ(keys, 6, "the 6 keys fill keycodes[]");
    TEST_ASSERT(!keycode_in_report(T_KC_LSHIFT),
                "the mod does NOT occupy a keycode slot");
    TEST_ASSERT((key_processor_report_mods() & 0x02) != 0,
                "Shift (OSM) carried in the modifier byte even with a full report (M7)");
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Suite runner                                                          */
/* ══════════════════════════════════════════════════════════════════════ */

/* ── Recycled slot: an absorbed key inherits the previous keycode ────────────
 *
 * Bug observed at the bench on 2026-09-12. While holding a layer (MO) on the
 * REMOTE half and typing a digit on the LOCAL one, the digit never
 * released — the host repeated it ("231111114" for a single press).
 *
 * Cause: build_keycode_report was not resetting keycodes[i] to zero in the
 * "layer-changing key — absorbed" branch (nor in is_hold, advanced,
 * leader, combo). This held up as long as each slot kept the same TYPE of
 * key from one cycle to the next. But remote fusion REPACKS the slots: on
 * release of the local key, the remote MO slides from slot 1 to slot 0 —
 * the slot that held the digit — and inherits its keycode left there.
 *
 * We reproduce the repack: digit in slot 0 + MO in slot 1, then MO alone in
 * slot 0. The final report must contain NO digit. */
static void test_kp_slot_recycle_ne_gele_pas_le_keycode(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_KC_A;     /* "A" at (0,0) */
    keymaps[0][0][1] = T_MO_L1;    /* MO(L1) at (0,1) */

    /* Cycle 1: A (slot 0) + MO (slot 1). */
    press_key(0, 0, 0);
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_A), "cycle 1: A present");

    /* Cycle 2: the local key (A) is released, the MO slides to slot 0 (repack). */
    release_all_keys();
    press_key(0, 0, 1);            /* MO alone, at SLOT 0 (where A used to be) */
    build_keycode_report();
    TEST_ASSERT(!keycode_in_report(T_KC_A),
                "cycle 2: A must NO LONGER be in the report (slot recycle)");
}

void test_keycode_report(void)
{
    test_kp_slot_recycle_ne_gele_pas_le_keycode();
    TEST_SUITE("Keycode Report Pipeline (build_keycode_report)");
    TEST_RUN(test_kp_simple_press);
    TEST_RUN(test_kp_simple_release);
    TEST_RUN(test_kp_modifier_in_report);
    TEST_RUN(test_kp_key_override_physical_mod);
    TEST_RUN(test_kp_mod_survives_full_report);
    TEST_RUN(test_kp_mo_activates_layer);
    TEST_RUN(test_kp_mo_key_absorbed);
    TEST_RUN(test_kp_mo_active_layer_keycode);
    TEST_RUN(test_kp_mo_deactivates_on_release);
    TEST_RUN(test_kp_to_toggle_on);
    TEST_RUN(test_kp_kno_fallback);
    TEST_RUN(test_kp_osm_applies_to_next_press);
    TEST_RUN(test_kp_osl_arm_called);
    TEST_RUN(test_kp_osl_active_layer);
    TEST_RUN(test_kp_combo_result_injected);
    TEST_RUN(test_kp_multi_key_press);
    TEST_RUN(test_kp_macro_does_not_starve_to);
    TEST_RUN(test_kp_double_mo_resolves_from_base_layer);
    TEST_RUN(test_kp_held_key_keeps_the_layer_it_was_pressed_on);
    TEST_RUN(test_kp_key_pressed_before_mo_keeps_the_base_layer);
    TEST_RUN(test_kp_key_that_resolves_an_lt_hold_is_on_the_lt_layer);
    TEST_RUN(test_kp_lt_resolves_oath_next_on_its_press_cycle);
    TEST_RUN(test_kp_lt_resolves_sec_confirm_from_left_confirms_once);
    TEST_RUN(test_kp_lt_resolves_sec_confirm_from_right_confirms_nothing);
    TEST_RUN(test_kp_sec_confirm_authorizes);
    TEST_RUN(test_kp_sec_confirm_routes_to_chest);
    TEST_RUN(test_kp_sec_confirm_held_confirms_chest_once);
    TEST_RUN(test_kp_sec_confirm_held_before_chest_op_does_not_confirm);
    TEST_RUN(test_kp_sec_confirm_press_records_the_seen_op);
    TEST_RUN(test_kp_sec_confirm_records_the_instance);
    TEST_RUN(test_kp_sec_confirm_ignored_from_the_right_half);
    TEST_RUN(test_kp_chest_next_requests_a_mode_change_once);
    TEST_RUN(test_kp_oath_next_requests_nav_once);
    TEST_RUN(test_kp_oath_prev_requests_nav_once);
    TEST_RUN(test_kp_oath_code_requests_once);
    TEST_RUN(test_kp_oath_keys_accepted_from_the_right_half);
    TEST_RUN(test_kp_oath_nav_sequence_take_once);
    TEST_RUN(test_kp_oath_nav_saturates_positive);
    TEST_RUN(test_kp_oath_nav_saturates_negative);
    TEST_RUN(test_kp_oath_code_twice_before_take_yields_one);
    TEST_RUN(test_chest_gate_oath_nav_cas_survives_a_concurrent_take);
    TEST_RUN(test_sec_confirm_from_local);
    TEST_RUN(test_kp_macro_inline_injects_steps);
    TEST_RUN(test_kp_macro_empty_name_noop);
    TEST_RUN(test_kp_macro_delay_sets_pending);
    TEST_RUN(test_kp_macro_delay_no_retrigger_held);
    TEST_RUN(test_kp_macro_legacy_injects_keys);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Modified Key (MK) — 0x8000-0x8FFF: "this key sends Shift+1".
 *
 * Brief: docs/superpowers/specs/2026-09-11-modified-keycodes-design.md.
 *
 * HID does not know "!": 1 and ! are the same key (0x1E), it's the OS that
 * decides based on Shift. The firmware must therefore put Shift in the
 * MODIFIER BYTE and 0x1E in keycodes[] IN THE SAME REPORT. Two mistakes are
 * natural here, and each test below names the one it catches:
 *
 *   (M7)  pushing 0xE1 into keycodes[] instead of the modifier byte — this is
 *         the bug fixed in commit bffdf4ec, where the mod stole a slot and got
 *         lost when all six were full;
 *   (COL) leaving the mod in extra_mods after release — sticky mod;
 *   (TH)  routing MK to tap_hold like MT — a tap would send "1";
 *   (BIT) testing `& 0x8000` instead of `& 0xF000 == 0x8000` — catches everything
 *         that is >= 0x8000, including future ranges.
 * ══════════════════════════════════════════════════════════════════════════ */

#define T_KC_1    0x1Eu
#define T_KC_C    0x06u
#define T_MOD_LCTL 0x01u

static void test_mk_press_shift_dans_le_modifier_et_1_dans_keycodes(void)
{
    /* The base semantics. Catches (M7) and (TH). */
    reset_kp_state();
    keymaps[0][0][0] = K_EXLM;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_1), "K_EXLM -> 0x1E in keycodes[]");
    TEST_ASSERT(key_processor_report_mods() & T_MOD_LSFT,
                "K_EXLM -> Shift in the modifier byte");
    TEST_ASSERT(!keycode_in_report(T_KC_LSHIFT),
                "and NEVER 0xE1 in keycodes[] — the mod does not steal a slot (M7)");
}

static void test_mk_release_ne_laisse_rien(void)
{
    /* Catches (COL). */
    reset_kp_state();
    keymaps[0][0][0] = K_EXLM;
    press_key(0, 0, 0);
    build_keycode_report();
    release_all_keys();
    build_keycode_report();
    TEST_ASSERT(!keycode_in_report(T_KC_1), "released -> 0x1E absent");
    TEST_ASSERT(!(key_processor_report_mods() & T_MOD_LSFT),
                "released -> Shift absent from the modifier (no sticky mod)");
}

static void test_mk_ne_vole_pas_de_slot_quand_le_rapport_est_plein(void)
{
    /* THE M7 test: five normal keys + K_EXLM = exactly six slots. If the
     * Shift went into keycodes[], one of the six would be evicted. */
    reset_kp_state();
    keymaps[0][0][0] = K_EXLM;
    keymaps[0][0][1] = T_KC_A;
    keymaps[0][0][2] = T_KC_B;
    keymaps[0][0][3] = T_KC_C;
    keymaps[0][0][4] = 0x07u;   /* D */
    keymaps[0][0][5] = 0x08u;   /* E */
    for (int c = 0; c < 6; c++) press_key(c, 0, (uint8_t)c);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_1) && keycode_in_report(T_KC_A) &&
                keycode_in_report(T_KC_B) && keycode_in_report(T_KC_C) &&
                keycode_in_report(0x07u)  && keycode_in_report(0x08u),
                "all six keys are in the report");
    TEST_ASSERT(key_processor_report_mods() & T_MOD_LSFT, "and Shift is in the modifier");
    TEST_ASSERT(!keycode_in_report(T_KC_LSHIFT), "no 0xE1 in keycodes[] (M7)");
}

static void test_mk_n_est_pas_que_shift(void)
{
    reset_kp_state();
    keymaps[0][0][0] = K_MK(MOD_LCTL, T_KC_C);
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_C), "K_MK(LCTL, C) -> C in keycodes[]");
    TEST_ASSERT(key_processor_report_mods() & T_MOD_LCTL, "and Ctrl in the modifier");
    TEST_ASSERT(!(key_processor_report_mods() & T_MOD_LSFT), "and NOT Shift");
}

static void test_mk_tenu_plus_une_lettre_la_limite_hid(void)
{
    /* The modifier byte is GLOBAL to the report: holding K_EXLM and pressing A gives
     * Shift+1+A = "!A". QMK does the same. We pin this down as documented
     * behavior, not as a hidden defect. */
    reset_kp_state();
    keymaps[0][0][0] = K_EXLM;
    keymaps[0][0][1] = T_KC_A;
    press_key(0, 0, 0);
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_1) && keycode_in_report(T_KC_A),
                "0x1E and 0x04 in the same report");
    TEST_ASSERT(key_processor_report_mods() & T_MOD_LSFT, "with global Shift");
}

static void test_mk_bornes_de_la_plage(void)
{
    /* Catches (BIT). */
    TEST_ASSERT(!K_IS_MK(0x7FFFu), "0x7FFF is not MK");
    TEST_ASSERT( K_IS_MK(0x8000u), "0x8000 is MK");
    TEST_ASSERT( K_IS_MK(0x8FFFu), "0x8FFF is MK");
    TEST_ASSERT(!K_IS_MK(0x9000u), "0x9000 is NOT MK — `& 0x8000` would catch it");
    TEST_ASSERT(K_MK_MOD(K_EXLM) == MOD_LSFT && K_MK_KEY(K_EXLM) == T_KC_1,
                "K_EXLM decomposes into (Shift, 0x1E)");
}

static void test_mk_repeat_reproduit_le_symbole(void)
{
    /* Brief decision 4: after "!", Repeat must give back "!", not "1". */
    reset_kp_state();
    keymaps[0][0][0] = K_EXLM;
    keymaps[0][0][1] = K_REPEAT;
    press_key(0, 0, 0);
    build_keycode_report();
    release_all_keys();
    build_keycode_report();
    press_key(0, 0, 1);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_1), "Repeat -> 0x1E");
    TEST_ASSERT(key_processor_report_mods() & T_MOD_LSFT,
                "Repeat -> and the MK's Shift, otherwise we get \"1\"");
}

static void test_mk_sous_caps_word_un_seul_shift(void)
{
    /* Decision 2: caps word only shifts letters; 0x1E is not one.
     * Expected result: exactly Shift (the MK's), nothing more. */
    reset_kp_state();
    caps_word_toggle();
    keymaps[0][0][0] = K_EXLM;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_1), "0x1E present");
    TEST_ASSERT((key_processor_report_mods() & 0x0F) == T_MOD_LSFT,
                "a single Shift, no other mod added");
}

void test_modified_key(void)
{
    printf("\n-- Modified Key: Shift+key in one press --\n");
    test_mk_press_shift_dans_le_modifier_et_1_dans_keycodes();
    test_mk_release_ne_laisse_rien();
    test_mk_ne_vole_pas_de_slot_quand_le_rapport_est_plein();
    test_mk_n_est_pas_que_shift();
    test_mk_tenu_plus_une_lettre_la_limite_hid();
    test_mk_bornes_de_la_plage();
    test_mk_repeat_reproduit_le_symbole();
    test_mk_sous_caps_word_un_seul_shift();
}
