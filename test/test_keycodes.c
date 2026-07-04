/* Tests for advanced keycode encoding/decoding.
 * Linke le VRAI header de prod (key_definitions.h, partagé avec KaSe_soft) au
 * lieu de recopier les macros : une dérive d'encodage casse maintenant le test.
 * key_definitions.h est autonome (tinyusb.h stubé) et compile déjà host-side
 * via key_processor.c. */
#include "test_framework.h"
#include "key_definitions.h"

/* ── OSM tests ───────────────────────────────────────────────────── */

static void test_osm_encoding(void) {
    uint16_t kc = K_OSM(MOD_LSFT);
    TEST_ASSERT_EQ(kc, 0x3002, "OSM(Shift) encoding");
    TEST_ASSERT(K_IS_OSM(kc), "OSM detection");
    TEST_ASSERT_EQ(K_OSM_MOD(kc), MOD_LSFT, "OSM mod extraction");

    kc = K_OSM(MOD_LCTL | MOD_LALT);
    TEST_ASSERT_EQ(kc, 0x3005, "OSM(Ctrl+Alt) encoding");
    TEST_ASSERT_EQ(K_OSM_MOD(kc), 0x05, "OSM multi-mod extraction");
}

static void test_osm_not_confused(void) {
    TEST_ASSERT(!K_IS_OSM(0x0029), "ESC is not OSM");
    TEST_ASSERT(!K_IS_OSM(0x4229), "LT is not OSM");
    TEST_ASSERT(!K_IS_OSM(0x5204), "MT is not OSM");
}

/* ── OSL tests ───────────────────────────────────────────────────── */

static void test_osl_encoding(void) {
    uint16_t kc = K_OSL(2);
    TEST_ASSERT_EQ(kc, 0x3102, "OSL(2) encoding");
    TEST_ASSERT(K_IS_OSL(kc), "OSL detection");
    TEST_ASSERT_EQ(K_OSL_LAYER(kc), 2, "OSL layer extraction");
}

/* ── LT tests ────────────────────────────────────────────────────── */

static void test_lt_encoding(void) {
    uint16_t kc = K_LT(2, 0x2C); /* LT(2, Space) */
    TEST_ASSERT_EQ(kc, 0x422C, "LT(2,Space) encoding");
    TEST_ASSERT(K_IS_LT(kc), "LT detection");
    TEST_ASSERT_EQ(K_LT_LAYER(kc), 2, "LT layer extraction");
    TEST_ASSERT_EQ(K_LT_KEY(kc), 0x2C, "LT key extraction");
}

static void test_lt_all_layers(void) {
    /* Le nibble de couche tient 0..15 — couvrir toute la plage, pas seulement 0..9 */
    for (int layer = 0; layer < 16; layer++) {
        uint16_t kc = K_LT(layer, 0x04); /* LT(layer, A) */
        TEST_ASSERT(K_IS_LT(kc), "LT detection for all layers");
        TEST_ASSERT_EQ(K_LT_LAYER(kc), layer, "LT layer extraction");
        TEST_ASSERT_EQ(K_LT_KEY(kc), 0x04, "LT key extraction");
    }
}

/* ── MT tests ────────────────────────────────────────────────────── */

static void test_mt_encoding(void) {
    uint16_t kc = K_MT(MOD_LSFT, 0x04); /* MT(Shift, A) */
    TEST_ASSERT_EQ(kc, 0x5204, "MT(Shift,A) encoding");
    TEST_ASSERT(K_IS_MT(kc), "MT detection");
    TEST_ASSERT_EQ(K_MT_MOD(kc), MOD_LSFT, "MT mod extraction");
    TEST_ASSERT_EQ(K_MT_KEY(kc), 0x04, "MT key extraction");
}

static void test_mt_esc_shift(void) {
    uint16_t kc = K_MT(MOD_LSFT, 0x29); /* MT(Shift, ESC) */
    TEST_ASSERT_EQ(kc, 0x5229, "MT(Shift,ESC) encoding");
    TEST_ASSERT_EQ(K_MT_KEY(kc), 0x29, "MT ESC key");
}

/* ── TD tests ────────────────────────────────────────────────────── */

static void test_td_encoding(void) {
    uint16_t kc = K_TD(0);
    TEST_ASSERT_EQ(kc, 0x6000, "TD(0) encoding");
    TEST_ASSERT(K_IS_TD(kc), "TD detection");
    TEST_ASSERT_EQ(K_TD_INDEX(kc), 0, "TD index extraction");

    kc = K_TD(5);
    TEST_ASSERT_EQ(kc, 0x6500, "TD(5) encoding");
    TEST_ASSERT_EQ(K_TD_INDEX(kc), 5, "TD(5) index");
}

/* ── Cross-detection tests ───────────────────────────────────────── */

static void test_no_cross_detection(void) {
    /* Each type should only match its own range */
    uint16_t lt = K_LT(1, 0x04);
    uint16_t mt = K_MT(1, 0x04);
    uint16_t td = K_TD(1);
    uint16_t osm = K_OSM(MOD_LSFT);

    TEST_ASSERT( K_IS_LT(lt) && !K_IS_MT(lt) && !K_IS_TD(lt) && !K_IS_OSM(lt), "LT exclusive");
    TEST_ASSERT(!K_IS_LT(mt) &&  K_IS_MT(mt) && !K_IS_TD(mt) && !K_IS_OSM(mt), "MT exclusive");
    TEST_ASSERT(!K_IS_LT(td) && !K_IS_MT(td) &&  K_IS_TD(td) && !K_IS_OSM(td), "TD exclusive");
    TEST_ASSERT(!K_IS_LT(osm) && !K_IS_MT(osm) && !K_IS_TD(osm) && K_IS_OSM(osm), "OSM exclusive");
}

static void test_special_keycodes(void) {
    TEST_ASSERT(K_IS_ADVANCED(K_CAPS_WORD), "CAPS_WORD is advanced");
    TEST_ASSERT(K_IS_ADVANCED(K_REPEAT), "REPEAT is advanced");
    TEST_ASSERT(K_IS_ADVANCED(K_LEADER), "LEADER is advanced");
    TEST_ASSERT(!K_IS_ADVANCED(0x04), "A is not advanced");
    TEST_ASSERT(!K_IS_ADVANCED(0x00), "K_NO is not advanced");
}

/* Security keycodes (0x3E00 block) */
static void test_keycode_sec_confirm(void)
{
    TEST_ASSERT_EQ(K_SEC_CONFIRM, 0x3E00, "K_SEC_CONFIRM = 0x3E00");
    TEST_ASSERT(K_IS_SEC(K_SEC_CONFIRM), "K_IS_SEC true for K_SEC_CONFIRM");
    TEST_ASSERT(!K_IS_SEC(0x3D00), "K_IS_SEC false for K_OVERRIDE base");
    TEST_ASSERT(!K_IS_SEC(0x4000), "K_IS_SEC false for LT base");
    TEST_ASSERT_EQ(K_SEC_TYPE(K_SEC_CONFIRM), 0x00, "K_SEC_TYPE extracts low byte");
}

/* ── Suite runner ────────────────────────────────────────────────── */

void test_keycodes(void) {
    TEST_SUITE("Keycode Encoding");
    TEST_RUN(test_osm_encoding);
    TEST_RUN(test_osm_not_confused);
    TEST_RUN(test_osl_encoding);
    TEST_RUN(test_lt_encoding);
    TEST_RUN(test_lt_all_layers);
    TEST_RUN(test_mt_encoding);
    TEST_RUN(test_mt_esc_shift);
    TEST_RUN(test_td_encoding);
    TEST_RUN(test_no_cross_detection);
    TEST_RUN(test_special_keycodes);
    TEST_RUN(test_keycode_sec_confirm);
}
