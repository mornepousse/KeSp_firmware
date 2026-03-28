/* Tests for key_features: OSM, OSL, Caps Word, Repeat Key */
#include "test_framework.h"

/* ── Minimal reimplementation for host testing ───────────────────── */

#define MOD_LSFT 0x02
#define MOD_LCTL 0x01
#define HID_KEY_CONTROL_LEFT 0xE0

/* OSM */
static uint8_t osm_mods = 0;
static void osm_arm(uint8_t m) { osm_mods |= m; }
static uint8_t osm_consume(void) { uint8_t m = osm_mods; osm_mods = 0; return m; }
static bool osm_is_active(void) { return osm_mods != 0; }

/* OSL */
static int8_t osl_layer = -1;
static void osl_arm(uint8_t l) { osl_layer = (int8_t)l; }
static int8_t osl_get_layer(void) { return osl_layer; }
static void osl_consume(void) { osl_layer = -1; }

/* Caps Word */
static bool cw_active = false;
static void caps_word_toggle(void) { cw_active = !cw_active; }
static bool caps_word_is_active(void) { return cw_active; }
static void caps_word_process(uint8_t *kc, uint8_t *mod) {
    if (!cw_active || *kc == 0) return;
    if (*kc >= 0x04 && *kc <= 0x1D) { *mod |= MOD_LSFT; return; }
    if ((*kc >= 0x1E && *kc <= 0x27) || *kc == 0x2D || *kc == 0x2E) return;
    if (*kc == 0x2A) return; /* backspace */
    cw_active = false;
}

/* Repeat Key */
static uint8_t last_kc = 0;
static void repeat_key_record(uint8_t kc) {
    if (kc != 0 && kc < HID_KEY_CONTROL_LEFT) last_kc = kc;
}
static uint8_t repeat_key_get(void) { return last_kc; }

/* ── OSM tests ───────────────────────────────────────────────────── */

static void test_osm_arm_consume(void) {
    osm_mods = 0;
    TEST_ASSERT(!osm_is_active(), "OSM initially inactive");
    osm_arm(MOD_LSFT);
    TEST_ASSERT(osm_is_active(), "OSM active after arm");
    uint8_t m = osm_consume();
    TEST_ASSERT_EQ(m, MOD_LSFT, "OSM consumed shift");
    TEST_ASSERT(!osm_is_active(), "OSM inactive after consume");
}

static void test_osm_multi_mod(void) {
    osm_mods = 0;
    osm_arm(MOD_LCTL);
    osm_arm(MOD_LSFT);
    TEST_ASSERT_EQ(osm_consume(), MOD_LCTL | MOD_LSFT, "OSM multi-mod");
}

/* ── OSL tests ───────────────────────────────────────────────────── */

static void test_osl_arm_consume(void) {
    osl_layer = -1;
    TEST_ASSERT_EQ(osl_get_layer(), -1, "OSL initially none");
    osl_arm(3);
    TEST_ASSERT_EQ(osl_get_layer(), 3, "OSL armed layer 3");
    osl_consume();
    TEST_ASSERT_EQ(osl_get_layer(), -1, "OSL consumed");
}

/* ── Caps Word tests ─────────────────────────────────────────────── */

static void test_caps_word_letters(void) {
    cw_active = false;
    caps_word_toggle();
    TEST_ASSERT(caps_word_is_active(), "CW active");

    uint8_t kc = 0x04; /* A */
    uint8_t mod = 0;
    caps_word_process(&kc, &mod);
    TEST_ASSERT_EQ(mod, MOD_LSFT, "CW shifts letters");
    TEST_ASSERT(caps_word_is_active(), "CW still active after letter");
}

static void test_caps_word_numbers(void) {
    cw_active = true;
    uint8_t kc = 0x1E; /* 1 */
    uint8_t mod = 0;
    caps_word_process(&kc, &mod);
    TEST_ASSERT_EQ(mod, 0, "CW doesn't shift numbers");
    TEST_ASSERT(caps_word_is_active(), "CW still active after number");
}

static void test_caps_word_space_deactivates(void) {
    cw_active = true;
    uint8_t kc = 0x2C; /* Space */
    uint8_t mod = 0;
    caps_word_process(&kc, &mod);
    TEST_ASSERT(!caps_word_is_active(), "CW deactivated on space");
}

static void test_caps_word_backspace_keeps(void) {
    cw_active = true;
    uint8_t kc = 0x2A; /* Backspace */
    uint8_t mod = 0;
    caps_word_process(&kc, &mod);
    TEST_ASSERT(caps_word_is_active(), "CW stays active on backspace");
}

/* ── Repeat Key tests ────────────────────────────────────────────── */

static void test_repeat_key(void) {
    last_kc = 0;
    TEST_ASSERT_EQ(repeat_key_get(), 0, "Repeat initially empty");
    repeat_key_record(0x04); /* A */
    TEST_ASSERT_EQ(repeat_key_get(), 0x04, "Repeat records A");
    repeat_key_record(0x05); /* B */
    TEST_ASSERT_EQ(repeat_key_get(), 0x05, "Repeat records B");
}

static void test_repeat_ignores_modifiers(void) {
    last_kc = 0x04;
    repeat_key_record(0xE0); /* Left Ctrl — modifier, should be ignored */
    TEST_ASSERT_EQ(repeat_key_get(), 0x04, "Repeat ignores modifiers");
}

static void test_repeat_ignores_zero(void) {
    last_kc = 0x04;
    repeat_key_record(0);
    TEST_ASSERT_EQ(repeat_key_get(), 0x04, "Repeat ignores zero");
}

/* ── Suite runner ────────────────────────────────────────────────── */

void test_key_features(void) {
    TEST_SUITE("Key Features (OSM, OSL, Caps Word, Repeat)");
    TEST_RUN(test_osm_arm_consume);
    TEST_RUN(test_osm_multi_mod);
    TEST_RUN(test_osl_arm_consume);
    TEST_RUN(test_caps_word_letters);
    TEST_RUN(test_caps_word_numbers);
    TEST_RUN(test_caps_word_space_deactivates);
    TEST_RUN(test_caps_word_backspace_keeps);
    TEST_RUN(test_repeat_key);
    TEST_RUN(test_repeat_ignores_modifiers);
    TEST_RUN(test_repeat_ignores_zero);
}
