#include "test_framework.h"
#include "oled_stats.h"

static void test_wpm(void) {
    TEST_ASSERT_EQ(oled_wpm_from_kpm(0),   0u,  "0 kpm → 0 wpm");
    TEST_ASSERT_EQ(oled_wpm_from_kpm(300), 60u, "300 kpm → 60 wpm");
    TEST_ASSERT_EQ(oled_wpm_from_kpm(7),   1u,  "7 kpm → 1 wpm (truncation)");
}

static void test_sparkline_scaling(void) {
    uint32_t hist[4] = { 0, 100, 200, 400 };
    uint8_t  out[4]  = {0};
    oled_sparkline_bars(hist, 4, 400, out, 4);
    TEST_ASSERT_EQ(out[0], 0, "0/400 → 0");
    TEST_ASSERT_EQ(out[3], 7, "400/400 → 7 (full)");
    TEST_ASSERT(out[1] < out[2] && out[2] < out[3], "monotonically increasing");
}

static void test_sparkline_max_zero(void) {
    uint32_t hist[2] = { 5, 9 };
    uint8_t  out[2]  = { 3, 3 };
    oled_sparkline_bars(hist, 2, 0, out, 2);
    TEST_ASSERT_EQ(out[0], 0, "max=0 → 0");
    TEST_ASSERT_EQ(out[1], 0, "max=0 → 0");
}

/* True downsampling n>out_n: each bin = average of several elements.
   Exercises bin boundaries + the average (the other tests use n==out_n,
   which bypasses the average — cf. review Task 4). */
static void test_sparkline_downsample(void) {
    /* n=8 → out_n=4, max=100. bin b = [2b, 2b+2) → average of 2 elements.
       Values that DIFFER within the bin → distinguishes the true average from
       a 1-element sampling (which would give out[1]=1, out[3]=2). */
    uint32_t hist[8] = { 0, 0, 20, 80, 100, 100, 30, 70 };
    uint8_t  out[4]  = {0};
    oled_sparkline_bars(hist, 8, 100, out, 4);
    TEST_ASSERT_EQ(out[0], 0, "bin0 (0,0) avg 0 → 0");
    TEST_ASSERT_EQ(out[1], 3, "bin1 (20,80) avg 50 → 3 (50*7/100)");
    TEST_ASSERT_EQ(out[2], 7, "bin2 (100,100) avg 100 → 7");
    TEST_ASSERT_EQ(out[3], 3, "bin3 (30,70) avg 50 → 3");
}

void test_oled_stats(void) {
    TEST_SUITE("OLED stats — pure helpers");
    TEST_RUN(test_wpm);
    TEST_RUN(test_sparkline_scaling);
    TEST_RUN(test_sparkline_downsample);
    TEST_RUN(test_sparkline_max_zero);
}
