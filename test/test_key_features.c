/* Tests for key_features: OSM, OSL, Caps Word, Repeat.
 * Links the REAL module (../main/input/key_features.c) — no more reimplementation.
 * The state is global to the process (module statics) and shared with the other
 * suites that pull in key_processor.c; since key_features has no dedicated reset,
 * each sub-test brings the state back to a known baseline via the public API. */
#include "test_framework.h"
#include "key_features.h"
#include "key_definitions.h"   /* MOD_LSFT, MOD_LCTL */

/* ── Baseline via the public API (no access to the module's statics) ── */
static void reset_osm(void)  { (void)osm_consume(); }             /* clears pending mods */
static void reset_osl(void)  { osl_consume(); }                    /* → -1 */
static void reset_caps(void) { if (caps_word_is_active()) caps_word_toggle(); }

/* ── OSM ─────────────────────────────────────────────────────────── */

static void test_osm_arm_consume(void) {
    reset_osm();
    TEST_ASSERT(!osm_is_active(), "OSM initially inactive");
    osm_arm(MOD_LSFT);
    TEST_ASSERT(osm_is_active(), "OSM active after arm");
    TEST_ASSERT_EQ(osm_consume(), MOD_LSFT, "OSM consumed shift");
    TEST_ASSERT(!osm_is_active(), "OSM inactive after consume");
}

static void test_osm_multi_mod(void) {
    reset_osm();
    osm_arm(MOD_LCTL);
    osm_arm(MOD_LSFT);
    TEST_ASSERT_EQ(osm_consume(), (uint8_t)(MOD_LCTL | MOD_LSFT), "OSM multi-mod");
}

/* ── OSL ─────────────────────────────────────────────────────────── */

static void test_osl_arm_consume(void) {
    reset_osl();
    TEST_ASSERT_EQ(osl_get_layer(), -1, "OSL initially none");
    osl_arm(3);
    TEST_ASSERT_EQ(osl_get_layer(), 3, "OSL armed layer 3");
    osl_consume();
    TEST_ASSERT_EQ(osl_get_layer(), -1, "OSL consumed");
}

/* ── Caps Word ───────────────────────────────────────────────────── */

static void test_caps_word_letters(void) {
    reset_caps();
    caps_word_toggle();
    TEST_ASSERT(caps_word_is_active(), "CW active after toggle");
    uint8_t kc = 0x04, mod = 0;   /* A */
    caps_word_process(&kc, &mod);
    TEST_ASSERT_EQ(mod, MOD_LSFT, "CW shifts letters");
    TEST_ASSERT(caps_word_is_active(), "CW still active after letter");
}

static void test_caps_word_numbers(void) {
    reset_caps(); caps_word_toggle();
    uint8_t kc = 0x1E, mod = 0;   /* 1 */
    caps_word_process(&kc, &mod);
    TEST_ASSERT_EQ(mod, 0, "CW doesn't shift numbers");
    TEST_ASSERT(caps_word_is_active(), "CW still active after number");
}

static void test_caps_word_space_deactivates(void) {
    reset_caps(); caps_word_toggle();
    uint8_t kc = 0x2C, mod = 0;   /* Space */
    caps_word_process(&kc, &mod);
    TEST_ASSERT(!caps_word_is_active(), "CW deactivated on space");
}

static void test_caps_word_backspace_keeps(void) {
    reset_caps(); caps_word_toggle();
    uint8_t kc = 0x2A, mod = 0;   /* Backspace */
    caps_word_process(&kc, &mod);
    TEST_ASSERT(caps_word_is_active(), "CW stays active on backspace");
}

/* Added coverage: inactive = no-op; upper bound Z (0x1D); Tab deactivates. */
static void test_caps_word_inactive_noop(void) {
    reset_caps();                 /* inactive */
    uint8_t kc = 0x04, mod = 0;   /* A, but CW off */
    caps_word_process(&kc, &mod);
    TEST_ASSERT_EQ(mod, 0, "CW inactive → no shift");
}

static void test_caps_word_z_edge_then_tab(void) {
    reset_caps(); caps_word_toggle();
    uint8_t kc = 0x1D, mod = 0;   /* Z, upper bound of letters */
    caps_word_process(&kc, &mod);
    TEST_ASSERT_EQ(mod, MOD_LSFT, "CW shifts Z (0x1D)");
    TEST_ASSERT(caps_word_is_active(), "CW active after Z");
    kc = 0x2B; mod = 0;           /* Tab: neither letter/number/backspace → deactivates */
    caps_word_process(&kc, &mod);
    TEST_ASSERT(!caps_word_is_active(), "CW deactivated on Tab");
}

/* ── Repeat Key ──────────────────────────────────────────────────── */

static void test_repeat_key(void) {
    repeat_key_record(0x04);      /* A */
    TEST_ASSERT_EQ(repeat_key_get(), 0x04, "Repeat records A");
    repeat_key_record(0x05);      /* B */
    TEST_ASSERT_EQ(repeat_key_get(), 0x05, "Repeat records B");
}

static void test_repeat_ignores_modifiers(void) {
    repeat_key_record(0x04);
    repeat_key_record(0xE0);      /* Left Ctrl — modifier, ignoré */
    TEST_ASSERT_EQ(repeat_key_get(), 0x04, "Repeat ignores modifiers");
}

static void test_repeat_ignores_zero(void) {
    repeat_key_record(0x04);
    repeat_key_record(0);
    TEST_ASSERT_EQ(repeat_key_get(), 0x04, "Repeat ignores zero");
}

/* OSL layer OUT OF BOUNDS (>= LAYERS=10) → ignored (otherwise OOB keymaps[]). */
static void test_osl_out_of_bounds(void) {
    reset_osl();
    osl_arm(15);   /* 15 >= LAYERS */
    TEST_ASSERT_EQ(osl_get_layer(), -1, "OSL layer 15 (>= LAYERS) → ignored, no OOB");
}

/* ── Suite runner ────────────────────────────────────────────────── */

void test_key_features(void) {
    TEST_SUITE("Key Features (OSM, OSL, Caps Word, Repeat) — real module");
    TEST_RUN(test_osm_arm_consume);
    TEST_RUN(test_osm_multi_mod);
    TEST_RUN(test_osl_arm_consume);
    TEST_RUN(test_osl_out_of_bounds);
    TEST_RUN(test_caps_word_letters);
    TEST_RUN(test_caps_word_numbers);
    TEST_RUN(test_caps_word_space_deactivates);
    TEST_RUN(test_caps_word_backspace_keeps);
    TEST_RUN(test_caps_word_inactive_noop);
    TEST_RUN(test_caps_word_z_edge_then_tab);
    TEST_RUN(test_repeat_key);
    TEST_RUN(test_repeat_ignores_modifiers);
    TEST_RUN(test_repeat_ignores_zero);
    /* Leaves the state clean for the following suites (CapsWord off, OSL/OSM empty) */
    reset_caps(); reset_osl(); reset_osm();
}
