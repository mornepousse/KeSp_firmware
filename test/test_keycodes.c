/* Tests for advanced keycode encoding/decoding */
#include "test_framework.h"

/* Reproduce the defines from key_definitions.h */
#define K_OSM_BASE  0x3000
#define K_OSM(mod)  (K_OSM_BASE | (mod))
#define K_IS_OSM(kc) (((kc) & 0xFF00) == K_OSM_BASE)
#define K_OSM_MOD(kc) ((kc) & 0xFF)

#define K_OSL_BASE  0x3100
#define K_OSL(layer) (K_OSL_BASE | (layer))
#define K_IS_OSL(kc) (((kc) & 0xFFF0) == K_OSL_BASE)
#define K_OSL_LAYER(kc) ((kc) & 0x0F)

#define K_CAPS_WORD 0x3200
#define K_REPEAT    0x3300
#define K_LEADER    0x3400

#define K_LT_BASE   0x4000
#define K_LT(layer, kc) (K_LT_BASE | ((layer) << 8) | (kc))
#define K_IS_LT(kc) (((kc) & 0xF000) == K_LT_BASE)
#define K_LT_LAYER(kc) (((kc) >> 8) & 0x0F)
#define K_LT_KEY(kc) ((kc) & 0xFF)

#define K_MT_BASE   0x5000
#define K_MT(mod, kc) (K_MT_BASE | ((mod) << 8) | (kc))
#define K_IS_MT(kc) (((kc) & 0xF000) == K_MT_BASE)
#define K_MT_MOD(kc) (((kc) >> 8) & 0x0F)
#define K_MT_KEY(kc) ((kc) & 0xFF)

#define K_TD_BASE   0x6000
#define K_TD(index) (K_TD_BASE | ((index) << 8))
#define K_IS_TD(kc) (((kc) & 0xF000) == K_TD_BASE)
#define K_TD_INDEX(kc) (((kc) >> 8) & 0x0F)

#define K_IS_ADVANCED(kc) ((kc) > 0x00FF)

#define MOD_LCTL 0x01
#define MOD_LSFT 0x02
#define MOD_LALT 0x04
#define MOD_LGUI 0x08

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
    for (int layer = 0; layer < 10; layer++) {
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
}
