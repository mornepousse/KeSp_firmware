/* Tests de caractérisation pour build_keycode_report() et process_matrix_changes().
 *
 * Ce fichier compile key_processor.c (via CMakeLists.txt) et fournit des stubs
 * pour toutes ses dépendances hardware/HID. Le but est d'épingler le comportement
 * ACTUEL du pipeline : un refactor qui casse un keystroke cassera un test ici.
 *
 * Stubs stateful : tap_hold_get_active_mods, tap_hold_get_active_layer,
 *                  osl_get_layer / osl_consume / osl_arm,
 *                  osm_consume, combo_consume.
 * Les autres stubs sont de purs no-ops.
 */
#include "test_framework.h"

/* ── Inclusions production (stubs et globals définis dans ce TU) ───── */
#include "matrix_scan.h"         /* extern globals : current_press_*, keycodes, etc. */
#include "keymap.h"              /* extern macro_t macros_list[], keymaps[][][] */
#include "key_processor.h"       /* build_keycode_report, process_matrix_changes */
#include "key_features.h"        /* osl_*, osm_*, caps_word_*, key_override_t, etc. */
#include "tap_hold.h"
#include "tap_dance.h"
#include "combo.h"
#include "leader.h"
#include "tama_engine.h"
#include "hid_bluetooth_manager.h"
#include "keyboard_task.h"
#include "keyboard_actions.h"
#include "sec_confirm.h"

/* ── Constantes HID locales (évite la chaîne key_definitions.h/tinyusb) ── */
#define T_KC_A      0x04u
#define T_KC_B      0x05u
#define T_KC_LSHIFT 0xE1u
#define T_KC_RSHIFT 0xE5u
#define T_KC_LCTRL  0xE0u

/* Valeurs numériques des keycodes de couche (static const dans key_definitions.h) */
#define T_MO_L1  0x0200u   /* MO_L1 */
#define T_MO_L2  0x0300u   /* MO_L2 = MO_L0 + 2*256 */
#define T_TO_L1  0x0C00u   /* TO_L1 = TO_L0 + 256 */
#define T_MACRO_1 0x1500u  /* MACRO_1 */
#define T_OSL_1  0x3101u   /* K_OSL(1) = K_OSL_BASE | 1 */
#define T_MOD_LSFT 0x02u   /* MOD_LSFT */
#define T_K_SEC_CONFIRM 0x3E00u

/* ── Définitions de tous les globals externes requis par key_processor.c ── */

uint16_t  keymaps[LAYERS][MATRIX_ROWS][MATRIX_COLS];
uint8_t   keycodes[6];
uint8_t   current_press_row[6];
uint8_t   current_press_col[6];
uint8_t   current_press_stat[6];
uint8_t   current_layout  = 0;
uint8_t   last_layer      = 0;

uint8_t   MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t   SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
/* tableau de pointeurs vers des tableaux 2D, comme déclaré dans matrix_scan.h */
uint8_t (*matrix_states[2])[MATRIX_ROWS][MATRIX_COLS] = {
    &MATRIX_STATE, &SLAVE_MATRIX_STATE
};

volatile uint8_t  stat_matrix_changed = 0;
volatile uint8_t  is_layer_changed    = 0;
volatile uint32_t last_activity_time_ms = 0;
uint8_t           usb_bl_state        = 0;

macro_t   macros_list[MAX_MACROS];
size_t    macros_count = 0;
char      default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH];

TaskHandle_t keyboard_task_handle = NULL;
volatile bool matrix_test_mode    = false;

/* ── État interne des stubs stateful ────────────────────────────────── */

static uint8_t  g_combo_kc        = 0;
static bool     g_leader_active   = false;

/* ── Stubs matrix_scan.h ─────────────────────────────────────────────── */

void     layer_changed(void)              {}
uint32_t get_last_activity_time_ms(void)  { return 0; }
void     matrix_setup(void)               {}
void     rtc_matrix_deinit(void)          {}

/* tap_hold.h : le VRAI module est linké (../main/input/tap_hold.c) — plus de
 * stubs ici. Aucun test de cette suite ne pilote tap_hold en état actif (aucune
 * touche MT/LT/OSM pressée), donc le vrai module reste inactif ici ; la logique
 * tap/hold est exercée dans test_tap_hold.c (horloge contrôlable). */

/* ── Stubs tap_dance.h ───────────────────────────────────────────────── */

void     tap_dance_init(void)                                              {}
bool     tap_dance_on_press(uint8_t idx, uint8_t row, uint8_t col)
         { (void)idx; (void)row; (void)col; return false; }
void     tap_dance_on_release(uint8_t row, uint8_t col)
         { (void)row; (void)col; }
void     tap_dance_tick(void)                                              {}
uint8_t  tap_dance_consume(void)         { return 0; }
bool     tap_dance_just_resolved(void)   { return false; }
void     tap_dance_set(uint8_t idx, const uint8_t acts[4])
         { (void)idx; (void)acts; }
const tap_dance_config_t *tap_dance_get(uint8_t idx)
         { (void)idx; return NULL; }
void     tap_dance_save(void)                                              {}
void     tap_dance_load(void)                                              {}

/* ── Stubs combo.h ───────────────────────────────────────────────────── */

void    combo_init(void)                                                   {}
void    combo_set(uint8_t idx, const combo_config_t *cfg)
        { (void)idx; (void)cfg; }
const combo_config_t *combo_get(uint8_t idx) { (void)idx; return NULL; }
bool    combo_should_defer(uint8_t row, uint8_t col)
        { (void)row; (void)col; return false; }
void    combo_defer_key(uint8_t row, uint8_t col, uint8_t kc)
        { (void)row; (void)col; (void)kc; }
bool    combo_is_suppressed(uint8_t row, uint8_t col)
        { (void)row; (void)col; return false; }
int     combo_process(const uint8_t pr[6], const uint8_t pc[6])
        { (void)pr; (void)pc; return 0; }
uint8_t combo_consume(uint8_t *r1, uint8_t *c1, uint8_t *r2, uint8_t *c2)
{
    uint8_t k = g_combo_kc;
    g_combo_kc = 0;
    if (r1) *r1 = INVALID_KEY_POS;
    if (c1) *c1 = INVALID_KEY_POS;
    if (r2) *r2 = INVALID_KEY_POS;
    if (c2) *c2 = INVALID_KEY_POS;
    return k;
}
uint8_t combo_consume_expired(void) { return 0; }
void    combo_save(void)            {}
void    combo_load(void)            {}

/* ── Stubs leader.h ──────────────────────────────────────────────────── */

void    leader_init(void)                                                  {}
void    leader_set(uint8_t idx, const leader_entry_t *e)
        { (void)idx; (void)e; }
const leader_entry_t *leader_get(uint8_t idx) { (void)idx; return NULL; }
void    leader_start(void)                                                 {}
bool    leader_feed(uint8_t kc)    { (void)kc; return false; }
bool    leader_tick(void)          { return false; }
uint8_t leader_consume(uint8_t *m) { if (m) *m = 0; return 0; }
bool    leader_is_active(void)     { return g_leader_active; }
void    leader_save(void)          {}
void    leader_load(void)          {}

/* key_features.h : le VRAI module est linké (../main/input/key_features.c) —
 * plus de stubs ici. OSM/OSL/CapsWord/Repeat/GraveEsc/LayerLock/WPM/KeyOverride/
 * TriLayer sont exercés en réel (key_override NVS guardé #ifndef TEST_HOST). */

/* ── Stubs tama_engine.h ─────────────────────────────────────────────── */

void tama_engine_action(tama2_action_t a) { (void)a; }

/* ── Stubs hid_bluetooth_manager.h ──────────────────────────────────── */

bool hid_bluetooth_is_initialized(void) { return false; }
void bt_next_device(void)               {}
void bt_prev_device(void)               {}
void bt_start_pairing(void)             {}
void bt_disconnect(void)                {}
void save_io_mode(uint8_t m)            { (void)m; }

/* ── Stubs keyboard_actions.h + keyboard_task.h ──────────────────────── */

void km_post_display_update(void)  {}
void km_post_bt_toggle(void)       {}
void keyboard_worker_init(void)    {}
void vTaskKeyboard(void *pv)       { (void)pv; }
void keyboard_manager_init(void)   {}

/* ── Stubs keymap.h (NVS) ────────────────────────────────────────────── */

void save_keymaps(uint16_t *d, size_t s)               { (void)d; (void)s; }
void load_keymaps(uint16_t *d, size_t s)               { (void)d; (void)s; }
void keymap_init_nvs(void)                             {}
void save_layout_names(char n[][MAX_LAYOUT_NAME_LENGTH], size_t c)
     { (void)n; (void)c; }
void load_layout_names(char n[][MAX_LAYOUT_NAME_LENGTH], size_t c)
     { (void)n; (void)c; }
void save_macros(macro_t *m, size_t c)                { (void)m; (void)c; }
void load_macros(macro_t *m, size_t c)                { (void)m; (void)c; }
void recalc_macros_count(void)                        {}

/* ══════════════════════════════════════════════════════════════════════ */
/* Helpers de test                                                       */
/* ══════════════════════════════════════════════════════════════════════ */

/* Remet tous les slots de pression à "aucune touche" */
static void release_all_keys(void)
{
    for (int i = 0; i < 6; i++) {
        current_press_row[i] = INVALID_KEY_POS;
        current_press_col[i] = INVALID_KEY_POS;
        current_press_stat[i] = 0;
    }
}

/* Appuie sur une touche dans un slot donné */
static void press_key(int slot, uint8_t row, uint8_t col)
{
    current_press_row[slot] = row;
    current_press_col[slot] = col;
    current_press_stat[slot] = 1;
}

/* Vérifie si un keycode est présent dans keycodes[] */
static bool keycode_in_report(uint8_t kc)
{
    for (int i = 0; i < 6; i++)
        if (keycodes[i] == kc) return true;
    return false;
}

/*
 * Réinitialisation complète entre chaque sous-cas :
 * - Vide les keymaps, les keycodes, les globals de couche
 * - Remet les stubs stateful à leur état par défaut
 * - Exécute deux cycles idle pour vider les statics internes de key_processor
 *   (prev_press_row/col, prev_shift_pressed, tap_injected_slots…)
 */
static void reset_kp_state(void)
{
    /* Globals keymaps + press */
    memset(keymaps, 0, sizeof(keymaps));
    memset(keycodes, 0, sizeof(keycodes));
    memset(extra_keycodes, 0, sizeof(extra_keycodes));
    memset(macros_list, 0, sizeof(macros_list));

    release_all_keys();
    current_layout = 0;
    last_layer     = 0;

    /* Globals key_processor.h visibles */
    keypress_internal_function  = 0;
    current_row_layer_changer   = INVALID_KEY_POS;
    current_col_layer_changer   = INVALID_KEY_POS;

    /* Stubs stateful */
    tap_hold_init();                               /* réinit le vrai tap/hold */
    (void)osm_consume();                           /* vide l'OSM réel */
    osl_consume();                                 /* OSL réel → -1 */
    if (caps_word_is_active()) caps_word_toggle(); /* CapsWord réel → off */
    g_combo_kc        = 0;
    g_leader_active   = false;

    /* Vide le macro pending interne (opaque static dans key_processor.c) */
    (void)key_processor_consume_macro();

    /* Deux cycles idle pour flusher prev_press_row/col et prev_shift_pressed */
    build_keycode_report();
    build_keycode_report();
    memset(keycodes, 0, sizeof(keycodes));
    memset(extra_keycodes, 0, sizeof(extra_keycodes));
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Cas de test                                                           */
/* ══════════════════════════════════════════════════════════════════════ */

/* 1. Press simple : keycode HID standard → apparaît dans keycodes[] */
static void test_kp_simple_press(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_KC_A;     /* A à la position (0,0) couche 0 */
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_A), "press A → 0x04 dans keycodes");
}

/* 2. Release : après relâchement, keycode absent du report */
static void test_kp_simple_release(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_KC_A;
    press_key(0, 0, 0);
    build_keycode_report();    /* cycle 1 : A pressé */
    release_all_keys();
    build_keycode_report();    /* cycle 2 : rien pressé */
    TEST_ASSERT(!keycode_in_report(T_KC_A), "relâchement A → 0x04 absent du report");
}

/* 3. Touche modificatrice via position matricielle → HID mod dans keycodes[] */
static void test_kp_modifier_in_report(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_KC_LSHIFT;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_LSHIFT),
                "LSHIFT dans keymaps → 0xE1 dans keycodes");
}

/* 4. MO(layer) : press → current_layout bascule sur la couche */
static void test_kp_mo_activates_layer(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT_EQ(current_layout, 1, "MO_L1 pressé → current_layout = 1");
}

/* 5. La touche MO elle-même est absorbée (pas dans keycodes ni extra_keycodes) */
static void test_kp_mo_key_absorbed(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;
    press_key(0, 0, 0);
    build_keycode_report();
    /* slot 0 : MO key absorbée → keycodes[0] = 0 */
    TEST_ASSERT_EQ(keycodes[0], 0, "touche MO absorbée → keycodes[0] = 0");
}

/* 6. Touche co-pressée avec MO → keycode de la couche active */
static void test_kp_mo_active_layer_keycode(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;       /* MO_L1 en (0,0) couche 0 */
    keymaps[1][0][1] = T_KC_B;        /* B en (0,1) couche 1 */
    press_key(0, 0, 0);
    press_key(1, 0, 1);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_B),
                "touche (0,1) sur couche 1 active → 0x05 (B) dans keycodes");
}

/* 7. MO release : après relâchement de la touche MO, retour couche 0 */
static void test_kp_mo_deactivates_on_release(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;
    press_key(0, 0, 0);
    build_keycode_report();    /* cycle press MO → current_layout = 1 */
    release_all_keys();
    build_keycode_report();    /* cycle release → restaure last_layer */
    TEST_ASSERT_EQ(current_layout, 0,
                   "relâchement MO_L1 → current_layout = 0");
}

/* 8. TO(layer) : press puis release → process_matrix_changes bascule la couche */
static void test_kp_to_toggle_on(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_TO_L1;

    /* Cycle 1 : press TO_L1 */
    press_key(0, 0, 0);
    build_keycode_report();
    process_matrix_changes();   /* clé encore tenue → pas de toggle */

    /* Cycle 2 : relâchement */
    release_all_keys();
    build_keycode_report();
    process_matrix_changes();   /* clé relâchée → apply_toggle_layer */

    TEST_ASSERT_EQ(current_layout, 1, "TO_L1 press+release → current_layout = 1");
}

/* 9. K_NO : touche transparente sur couche active → fallback sur last_layer */
static void test_kp_kno_fallback(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;       /* MO_L1 active la couche 1 */
    keymaps[0][0][1] = T_KC_A;        /* A en (0,1) couche 0 */
    keymaps[1][0][1] = 0x0000u;       /* K_NO en (0,1) couche 1 → fallback à couche 0 */

    press_key(0, 0, 0);               /* slot 0 : MO */
    press_key(1, 0, 1);               /* slot 1 : touche avec K_NO sur L1 */
    build_keycode_report();

    /* la touche (0,1) doit produire T_KC_A (depuis last_layer = 0) */
    TEST_ASSERT(keycode_in_report(T_KC_A),
                "K_NO sur couche active → fallback couche 0 = A (0x04)");
}

/* 10. OSM réel : osm_arm() puis build → osm_consume() injecte le modificateur */
static void test_kp_osm_mod_injection(void)
{
    reset_kp_state();
    osm_arm(T_MOD_LSFT);          /* MOD_LSFT = bit 1 → HID_KEY_CONTROL_LEFT + 1 = 0xE1 */
    /* Aucune touche pressée : le modificateur OSM sort seul */
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_LSHIFT),
                "osm_arm(MOD_LSFT) → 0xE1 injecté dans keycodes");
}

/* 11a. OSL arm réel : press K_OSL(1) → le vrai osl_arm(1) est appelé */
static void test_kp_osl_arm_called(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_OSL_1;       /* K_OSL(1) = 0x3101 */
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT_EQ(osl_get_layer(), 1,
                   "press K_OSL(1) → osl armé sur couche 1");
}

/* 11b. OSL active layer réel : osl_arm(2) → active_layer écrasé sur 2 */
static void test_kp_osl_active_layer(void)
{
    reset_kp_state();
    osl_arm(2);                    /* OSL couche 2 armé (vrai module) */
    keymaps[2][0][0] = T_KC_B;    /* B sur couche 2 */
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(keycode_in_report(T_KC_B),
                "osl_get_layer() = 2 → key (0,0) résout sur couche 2 = B (0x05)");
}

/* 14. Combo : combo_consume() retourne un keycode → injecté dans keycodes[] */
static void test_kp_combo_result_injected(void)
{
    reset_kp_state();
    g_combo_kc = T_KC_A;   /* simulation : combo J+K = A */
    build_keycode_report(); /* aucune touche pressée mais combo_consume retourne A */
    TEST_ASSERT(keycode_in_report(T_KC_A),
                "combo_consume() retourne 0x04 → A injecté dans keycodes");
}

/* 15. Plusieurs touches simultanées → toutes dans keycodes[] */
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
    TEST_ASSERT(keycode_in_report(T_KC_A), "multi-press : A (0x04) dans report");
    TEST_ASSERT(keycode_in_report(T_KC_B), "multi-press : B (0x05) dans report");
    TEST_ASSERT(keycode_in_report(0x06u),  "multi-press : C (0x06) dans report");
}

/* 16. MACRO + TO co-pressés : la touche MACRO ne doit PAS occuper le slot
 *     keypress_internal_function (réservé à TO/BT), sinon un TO simultané
 *     n'est jamais togglé. */
static void test_kp_macro_does_not_starve_to(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MACRO_1;   /* slot 0 : macro (vide → expand no-op) */
    keymaps[0][0][1] = T_TO_L1;     /* slot 1 : TO_L1 */

    /* Cycle press : les deux touches tenues */
    press_key(0, 0, 0);
    press_key(1, 0, 1);
    build_keycode_report();
    process_matrix_changes();       /* encore tenues → pas de toggle */

    /* Cycle release : TO relâché → toggle doit s'appliquer */
    release_all_keys();
    build_keycode_report();
    process_matrix_changes();

    TEST_ASSERT_EQ(current_layout, 1,
                   "MACRO + TO co-pressés → TO togglé (macro n'affame plus le slot interne)");
}

/* 18. Double MO simultané : chaque touche MO est résolue depuis la couche
 *     active en DÉBUT de cycle, pas depuis la couche mutée par un MO traité
 *     plus tôt dans la boucle. Sans ça, l'ordre d'itération décide quelle
 *     couche est lue (bug d'ordre). Comportement aligné sur l'intention
 *     documentée de l'étape 2. */
static void test_kp_double_mo_resolves_from_base_layer(void)
{
    reset_kp_state();
    keymaps[0][0][0] = T_MO_L1;   /* slot 0 : MO_L1 sur la base */
    keymaps[0][0][1] = T_MO_L2;   /* slot 1 : MO_L2 sur la base */
    keymaps[1][0][1] = T_KC_A;    /* sur couche 1, (0,1) = touche normale (piège) */

    press_key(0, 0, 0);
    press_key(1, 0, 1);
    build_keycode_report();

    /* Les deux MO lus depuis la couche 0 → MO_L2 reconnu → couche finale 2.
     * (Avec le bug d'ordre : slot1 lu sur couche 1 = touche normale → couche 1.) */
    TEST_ASSERT_EQ(current_layout, 2,
                   "double MO simultané : résolution depuis la couche de base (→ couche 2)");
}

/* 19. K_SEC_CONFIRM : press → authorize pending request, absorbed (not emitted) */
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

/* ══════════════════════════════════════════════════════════════════════ */
/* Suite runner                                                          */
/* ══════════════════════════════════════════════════════════════════════ */

void test_keycode_report(void)
{
    TEST_SUITE("Keycode Report Pipeline (build_keycode_report)");
    TEST_RUN(test_kp_simple_press);
    TEST_RUN(test_kp_simple_release);
    TEST_RUN(test_kp_modifier_in_report);
    TEST_RUN(test_kp_mo_activates_layer);
    TEST_RUN(test_kp_mo_key_absorbed);
    TEST_RUN(test_kp_mo_active_layer_keycode);
    TEST_RUN(test_kp_mo_deactivates_on_release);
    TEST_RUN(test_kp_to_toggle_on);
    TEST_RUN(test_kp_kno_fallback);
    TEST_RUN(test_kp_osm_mod_injection);
    TEST_RUN(test_kp_osl_arm_called);
    TEST_RUN(test_kp_osl_active_layer);
    TEST_RUN(test_kp_combo_result_injected);
    TEST_RUN(test_kp_multi_key_press);
    TEST_RUN(test_kp_macro_does_not_starve_to);
    TEST_RUN(test_kp_double_mo_resolves_from_base_layer);
    TEST_RUN(test_kp_sec_confirm_authorizes);
}
