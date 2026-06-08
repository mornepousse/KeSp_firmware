/*
 * test_trackpad_core.c — Tests host-side pour les fonctions pures extraites
 * de trackpad.c : tp_parse_raw(), tp_should_skip_idle(), tp_is_show_reset().
 *
 * Ces fonctions encapsulent les décisions de logique pure qui étaient noyées
 * dans trackpad_task() (code hardware).  Elles compilent en dehors du bloc
 * #ifndef TEST_HOST et sont testables sans ESP-IDF ni FreeRTOS.
 *
 * Run: cd test/build && cmake .. && make && ./test_runner
 */
#include "test_framework.h"
#include "../main/periph/trackpad/trackpad.h"

/* ═══════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════ */

/* Encode rel_x / rel_y en big-endian dans le tableau 9 octets, tel que
 * le fait l'IQS5xx sur le bus I2C. */
static void build_raw(uint8_t out[9],
                      uint8_t ge0, uint8_t ge1, uint8_t sysinfo0,
                      uint8_t n_fingers, int16_t rx, int16_t ry)
{
    out[0] = ge0;
    out[1] = ge1;
    out[2] = sysinfo0;
    out[3] = 0x00;   /* SystemInfo1 — ignoré */
    out[4] = n_fingers;
    out[5] = (uint8_t)((uint16_t)rx >> 8);
    out[6] = (uint8_t)((uint16_t)rx & 0xFF);
    out[7] = (uint8_t)((uint16_t)ry >> 8);
    out[8] = (uint8_t)((uint16_t)ry & 0xFF);
}

/* ═══════════════════════════════════════════════════════════════════════
 * tp_parse_raw
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_tp_parse_raw_zeros(void)
{
    uint8_t data[9] = {0};
    tp_frame_t f;
    tp_parse_raw(data, &f);
    TEST_ASSERT_EQ(f.ge0,       0, "parse zeros: ge0=0");
    TEST_ASSERT_EQ(f.ge1,       0, "parse zeros: ge1=0");
    TEST_ASSERT_EQ(f.sysinfo0,  0, "parse zeros: sysinfo0=0");
    TEST_ASSERT_EQ(f.n_fingers, 0, "parse zeros: n_fingers=0");
    TEST_ASSERT_EQ(f.rel_x,     0, "parse zeros: rel_x=0");
    TEST_ASSERT_EQ(f.rel_y,     0, "parse zeros: rel_y=0");
}

static void test_tp_parse_raw_typical(void)
{
    uint8_t data[9];
    build_raw(data, 0x01, 0x02, 0x00, 3, 100, -50);
    tp_frame_t f;
    tp_parse_raw(data, &f);
    TEST_ASSERT_EQ(f.ge0,        0x01, "parse typical: ge0=0x01");
    TEST_ASSERT_EQ(f.ge1,        0x02, "parse typical: ge1=0x02");
    TEST_ASSERT_EQ(f.sysinfo0,   0x00, "parse typical: sysinfo0=0");
    TEST_ASSERT_EQ(f.n_fingers,  3,    "parse typical: n_fingers=3");
    TEST_ASSERT_EQ(f.rel_x,      100,  "parse typical: rel_x=100");
    TEST_ASSERT_EQ(f.rel_y,      -50,  "parse typical: rel_y=-50");
}

static void test_tp_parse_raw_negative_relx(void)
{
    /* rel_x=-1 : big-endian = 0xFF, 0xFF */
    uint8_t data[9];
    build_raw(data, 0x00, 0x00, 0x00, 1, -1, 0);
    tp_frame_t f;
    tp_parse_raw(data, &f);
    TEST_ASSERT_EQ(f.rel_x, -1, "parse neg rel_x=-1");
}

static void test_tp_parse_raw_negative_rely(void)
{
    /* rel_y=-32768 : big-endian = 0x80, 0x00 */
    uint8_t data[9];
    build_raw(data, 0x00, 0x00, 0x00, 0, 0, -32768);
    tp_frame_t f;
    tp_parse_raw(data, &f);
    TEST_ASSERT_EQ(f.rel_y, -32768, "parse neg rel_y=-32768 (INT16_MIN)");
}

static void test_tp_parse_raw_show_reset_bit(void)
{
    /* sysinfo0 bit 7 positionné */
    uint8_t data[9];
    build_raw(data, 0x00, 0x00, 0x80, 0, 0, 0);
    tp_frame_t f;
    tp_parse_raw(data, &f);
    TEST_ASSERT_EQ(f.sysinfo0, 0x80, "parse sysinfo0=0x80 (ShowReset)");
}

static void test_tp_parse_raw_sysinfo1_ignored(void)
{
    /* data[3] = SystemInfo1 = 0xFF — ne doit apparaître nulle part dans tp_frame_t */
    uint8_t data[9];
    build_raw(data, 0x00, 0x00, 0x00, 0, 0, 0);
    data[3] = 0xFF;   /* SysInfo1, devrait être ignoré */
    tp_frame_t f;
    tp_parse_raw(data, &f);
    /* Les champs accessibles ne doivent pas être contaminés */
    TEST_ASSERT_EQ(f.ge0,       0, "sysinfo1 ignored: ge0 intact");
    TEST_ASSERT_EQ(f.ge1,       0, "sysinfo1 ignored: ge1 intact");
    TEST_ASSERT_EQ(f.sysinfo0,  0, "sysinfo1 ignored: sysinfo0 intact");
    TEST_ASSERT_EQ(f.n_fingers, 0, "sysinfo1 ignored: n_fingers intact");
    TEST_ASSERT_EQ(f.rel_x,     0, "sysinfo1 ignored: rel_x not contaminated");
    TEST_ASSERT_EQ(f.rel_y,     0, "sysinfo1 ignored: rel_y not contaminated");
}

/* ═══════════════════════════════════════════════════════════════════════
 * tp_should_skip_idle
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_skip_idle_all_zero_no_prev(void)
{
    /* Cas nominal : frame tout à zéro, aucun doigt précédent → drop */
    bool skip = tp_should_skip_idle(0, 0, 0, 0, 0, 0);
    TEST_ASSERT(skip, "skip_idle: all-zero + prev_nf=0 → drop");
}

static void test_skip_idle_finger_lift(void)
{
    /* Frame tout à zéro MAIS un doigt était posé → NE PAS dropper (finger-lift) */
    bool skip = tp_should_skip_idle(0, 0, 0, 0, 0, 1);
    TEST_ASSERT(!skip, "skip_idle: all-zero + prev_nf=1 (finger-lift) → must forward");
}

static void test_skip_idle_finger_lift_multi(void)
{
    /* Idem avec plusieurs doigts précédents */
    bool skip = tp_should_skip_idle(0, 0, 0, 0, 0, 5);
    TEST_ASSERT(!skip, "skip_idle: all-zero + prev_nf=5 → must forward");
}

static void test_skip_idle_ge0_nonzero(void)
{
    bool skip = tp_should_skip_idle(0x01, 0, 0, 0, 0, 0);
    TEST_ASSERT(!skip, "skip_idle: ge0!=0 → active frame, do not drop");
}

static void test_skip_idle_ge1_nonzero(void)
{
    bool skip = tp_should_skip_idle(0, 0x02, 0, 0, 0, 0);
    TEST_ASSERT(!skip, "skip_idle: ge1!=0 → active frame, do not drop");
}

static void test_skip_idle_n_fingers_nonzero(void)
{
    bool skip = tp_should_skip_idle(0, 0, 1, 0, 0, 0);
    TEST_ASSERT(!skip, "skip_idle: n_fingers=1 → active frame, do not drop");
}

static void test_skip_idle_rel_x_nonzero(void)
{
    bool skip = tp_should_skip_idle(0, 0, 0, 10, 0, 0);
    TEST_ASSERT(!skip, "skip_idle: rel_x=10 → active frame, do not drop");
}

static void test_skip_idle_rel_y_negative(void)
{
    bool skip = tp_should_skip_idle(0, 0, 0, 0, -5, 0);
    TEST_ASSERT(!skip, "skip_idle: rel_y=-5 → active frame, do not drop");
}

/* ═══════════════════════════════════════════════════════════════════════
 * tp_is_show_reset
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_show_reset_bit7_set(void)
{
    TEST_ASSERT(tp_is_show_reset(0x80), "show_reset: bit7 set (0x80) → true");
}

static void test_show_reset_all_bits_set(void)
{
    TEST_ASSERT(tp_is_show_reset(0xFF), "show_reset: 0xFF (bit7 set) → true");
}

static void test_show_reset_bit7_clear(void)
{
    TEST_ASSERT(!tp_is_show_reset(0x7F), "show_reset: 0x7F (bit7 clear) → false");
}

static void test_show_reset_zero(void)
{
    TEST_ASSERT(!tp_is_show_reset(0x00), "show_reset: 0x00 → false");
}

static void test_show_reset_adjacent_bit(void)
{
    /* bit 6 positionné mais pas bit 7 */
    TEST_ASSERT(!tp_is_show_reset(0x40), "show_reset: 0x40 (bit6 only) → false");
}

static void test_show_reset_bit7_with_lower(void)
{
    /* bit7 + quelques bits inférieurs */
    TEST_ASSERT(tp_is_show_reset(0x83), "show_reset: 0x83 (bit7 set) → true");
}

/* ═══════════════════════════════════════════════════════════════════════
 * Entrée de la suite
 * ═══════════════════════════════════════════════════════════════════════ */

void test_trackpad_core(void)
{
    TEST_SUITE("trackpad_core: tp_parse_raw");
    test_tp_parse_raw_zeros();
    test_tp_parse_raw_typical();
    test_tp_parse_raw_negative_relx();
    test_tp_parse_raw_negative_rely();
    test_tp_parse_raw_show_reset_bit();
    test_tp_parse_raw_sysinfo1_ignored();

    TEST_SUITE("trackpad_core: tp_should_skip_idle");
    test_skip_idle_all_zero_no_prev();
    test_skip_idle_finger_lift();
    test_skip_idle_finger_lift_multi();
    test_skip_idle_ge0_nonzero();
    test_skip_idle_ge1_nonzero();
    test_skip_idle_n_fingers_nonzero();
    test_skip_idle_rel_x_nonzero();
    test_skip_idle_rel_y_negative();

    TEST_SUITE("trackpad_core: tp_is_show_reset");
    test_show_reset_bit7_set();
    test_show_reset_all_bits_set();
    test_show_reset_bit7_clear();
    test_show_reset_zero();
    test_show_reset_adjacent_bit();
    test_show_reset_bit7_with_lower();
}
