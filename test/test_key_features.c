/* Tests for key_features: OSM, OSL, Caps Word, Repeat.
 * Linke le VRAI module (../main/input/key_features.c) — plus de réimplémentation.
 * L'état est global au process (statics du module) et partagé avec les autres
 * suites qui tirent key_processor.c ; key_features n'ayant pas de reset dédié,
 * chaque sous-test ramène l'état à une baseline connue via l'API publique. */
#include "test_framework.h"
#include "key_features.h"
#include "key_definitions.h"   /* MOD_LSFT, MOD_LCTL */

/* ── Baseline via API publique (pas d'accès aux statics du module) ── */
static void reset_osm(void)  { (void)osm_consume(); }             /* vide les mods pending */
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

/* Couverture ajoutée : inactif = no-op ; borne haute Z (0x1D) ; Tab désactive. */
static void test_caps_word_inactive_noop(void) {
    reset_caps();                 /* inactif */
    uint8_t kc = 0x04, mod = 0;   /* A, mais CW off */
    caps_word_process(&kc, &mod);
    TEST_ASSERT_EQ(mod, 0, "CW inactif → aucun shift");
}

static void test_caps_word_z_edge_then_tab(void) {
    reset_caps(); caps_word_toggle();
    uint8_t kc = 0x1D, mod = 0;   /* Z, borne haute des lettres */
    caps_word_process(&kc, &mod);
    TEST_ASSERT_EQ(mod, MOD_LSFT, "CW shifte Z (0x1D)");
    TEST_ASSERT(caps_word_is_active(), "CW actif après Z");
    kc = 0x2B; mod = 0;           /* Tab : ni lettre/chiffre/backspace → désactive */
    caps_word_process(&kc, &mod);
    TEST_ASSERT(!caps_word_is_active(), "CW désactivé sur Tab");
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

/* OSL couche HORS BORNES (>= LAYERS=10) → ignorée (sinon OOB keymaps[]). */
static void test_osl_out_of_bounds(void) {
    reset_osl();
    osl_arm(15);   /* 15 >= LAYERS */
    TEST_ASSERT_EQ(osl_get_layer(), -1, "OSL couche 15 (>= LAYERS) → ignorée, pas d'OOB");
}

/* ── Suite runner ────────────────────────────────────────────────── */

void test_key_features(void) {
    TEST_SUITE("Key Features (OSM, OSL, Caps Word, Repeat) — module réel");
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
    /* Laisse l'état propre pour les suites suivantes (CapsWord off, OSL/OSM vides) */
    reset_caps(); reset_osl(); reset_osm();
}
