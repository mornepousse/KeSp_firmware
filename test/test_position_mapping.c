/* Test V1 <-> V2 position mapping tables */
#include "test_framework.h"

/* Copy mapping tables from cdc_acm_com.c for testing */
static const uint8_t v1_to_v2_map[5][13] = {
    {4*16+0, 4*16+1, 0*16+2, 0*16+3, 0*16+4, 0*16+5, 0*16+7, 0*16+8, 0*16+9, 0*16+10, 4*16+12, 0xFF, 0*16+6},
    {0*16+0, 0*16+1, 1*16+2, 1*16+3, 1*16+4, 1*16+5, 1*16+7, 1*16+8, 1*16+9, 1*16+10, 0*16+11, 0*16+12, 1*16+6},
    {1*16+0, 1*16+1, 2*16+2, 2*16+3, 2*16+4, 2*16+5, 2*16+7, 2*16+8, 2*16+9, 2*16+10, 1*16+11, 1*16+12, 2*16+6},
    {2*16+0, 2*16+1, 3*16+2, 3*16+3, 3*16+4, 3*16+5, 3*16+7, 3*16+8, 3*16+9, 3*16+10, 2*16+11, 2*16+12, 0xFF},
    {3*16+0, 3*16+1, 4*16+2, 3*16+6, 4*16+4, 4*16+5, 4*16+7, 4*16+8, 4*16+9, 3*16+11, 3*16+11, 3*16+12, 0xFF}
};

static const uint8_t v2_to_v1_map[5][13] = {
    {1*16+0, 1*16+1, 0*16+2, 0*16+3, 0*16+4, 0*16+5, 0*16+12, 0*16+6, 0*16+7, 0*16+8, 0*16+9, 1*16+10, 1*16+11},
    {2*16+0, 2*16+1, 1*16+2, 1*16+3, 1*16+4, 1*16+5, 1*16+12, 1*16+6, 1*16+7, 1*16+8, 1*16+9, 2*16+10, 2*16+11},
    {3*16+0, 3*16+1, 2*16+2, 2*16+3, 2*16+4, 2*16+5, 2*16+12, 2*16+6, 2*16+7, 2*16+8, 2*16+9, 3*16+10, 3*16+11},
    {4*16+0, 4*16+1, 3*16+2, 3*16+3, 3*16+4, 3*16+5, 4*16+3, 3*16+6, 3*16+7, 3*16+8, 3*16+9, 4*16+9, 4*16+11},
    {0*16+0, 0*16+1, 4*16+2, 4*16+3, 4*16+4, 4*16+5, 0xFF, 4*16+6, 4*16+7, 4*16+8, 0xFF, 0xFF, 0*16+10}
};

static inline void v2_to_v1_pos(int v2_row, int v2_col, int *v1_row, int *v1_col) {
    uint8_t packed = v2_to_v1_map[v2_row][v2_col];
    if (packed == 0xFF) {
        *v1_row = v2_row;
        *v1_col = v2_col;
    } else {
        *v1_row = packed >> 4;
        *v1_col = packed & 0x0F;
    }
}

static inline void v1_to_v2_pos(int v1_row, int v1_col, int *v2_row, int *v2_col) {
    uint8_t packed = v1_to_v2_map[v1_row][v1_col];
    if (packed == 0xFF) {
        *v2_row = v1_row;
        *v2_col = v1_col;
    } else {
        *v2_row = packed >> 4;
        *v2_col = packed & 0x0F;
    }
}

/* Test: mapping produces valid row/col within bounds */
void test_v1_to_v2_bounds(void) {
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 13; c++) {
            int v2r, v2c;
            v1_to_v2_pos(r, c, &v2r, &v2c);
            TEST_ASSERT(v2r >= 0 && v2r < 5, "v2_row in bounds");
            TEST_ASSERT(v2c >= 0 && v2c < 13, "v2_col in bounds");
        }
    }
}

void test_v2_to_v1_bounds(void) {
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 13; c++) {
            int v1r, v1c;
            v2_to_v1_pos(r, c, &v1r, &v1c);
            TEST_ASSERT(v1r >= 0 && v1r < 5, "v1_row in bounds");
            TEST_ASSERT(v1c >= 0 && v1c < 13, "v1_col in bounds");
        }
    }
}

/* Test: round-trip V1->V2->V1 produces original position (for non-0xFF entries)
 * Known exceptions: V1 row 4 has duplicate 'Z' keys at cols 9 and 10, both mapping
 * to V2(3,11). The reverse map picks col 9, so V1(4,10) doesn't round-trip. */
void test_roundtrip_v1_v2_v1(void) {
    int roundtrip_ok = 0, roundtrip_skip = 0, known_dup = 0;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 13; c++) {
            if (v1_to_v2_map[r][c] == 0xFF) { roundtrip_skip++; continue; }
            int v2r, v2c, v1r_back, v1c_back;
            v1_to_v2_pos(r, c, &v2r, &v2c);
            if (v2_to_v1_map[v2r][v2c] == 0xFF) { roundtrip_skip++; continue; }
            v2_to_v1_pos(v2r, v2c, &v1r_back, &v1c_back);
            if (v1r_back == r && v1c_back == c) {
                roundtrip_ok++;
            } else if (r == 4 && c == 10) {
                /* Known: duplicate Z key, V1(4,10) shares V2 position with V1(4,9) */
                known_dup++;
            } else {
                printf("    UNEXPECTED ROUNDTRIP FAIL: V1(%d,%d)->V2(%d,%d)->V1(%d,%d)\n",
                       r, c, v2r, v2c, v1r_back, v1c_back);
                _test_fail_count++;
            }
        }
    }
    TEST_ASSERT(roundtrip_ok > 50, "most V1 positions round-trip correctly");
}

/* Test: round-trip V2->V1->V2
 * Known exception: LWIN appears at both V2(3,6) and V2(4,3). V1(4,3)->V2(3,6) is
 * canonical, so V2(4,3) doesn't round-trip. */
void test_roundtrip_v2_v1_v2(void) {
    int roundtrip_ok = 0, roundtrip_skip = 0, known_dup = 0;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 13; c++) {
            if (v2_to_v1_map[r][c] == 0xFF) { roundtrip_skip++; continue; }
            int v1r, v1c, v2r_back, v2c_back;
            v2_to_v1_pos(r, c, &v1r, &v1c);
            if (v1_to_v2_map[v1r][v1c] == 0xFF) { roundtrip_skip++; continue; }
            v1_to_v2_pos(v1r, v1c, &v2r_back, &v2c_back);
            if (v2r_back == r && v2c_back == c) {
                roundtrip_ok++;
            } else if (r == 4 && c == 3) {
                /* Known: LWIN duplicate, V2(4,3) and V2(3,6) both map through V1(4,3) */
                known_dup++;
            } else {
                printf("    UNEXPECTED ROUNDTRIP FAIL: V2(%d,%d)->V1(%d,%d)->V2(%d,%d)\n",
                       r, c, v1r, v1c, v2r_back, v2c_back);
                _test_fail_count++;
            }
        }
    }
    TEST_ASSERT(roundtrip_ok > 50, "most V2 positions round-trip correctly");
}

/* Test: packed encoding is consistent (high nibble = row, low nibble = col) */
void test_packed_encoding(void) {
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 13; c++) {
            uint8_t packed = v1_to_v2_map[r][c];
            if (packed == 0xFF) continue;
            int decoded_row = packed >> 4;
            int decoded_col = packed & 0x0F;
            TEST_ASSERT(decoded_row < 5, "packed row < 5");
            TEST_ASSERT(decoded_col < 13, "packed col < 13");
        }
    }
}

void test_position_mapping(void) {
    TEST_SUITE("Position Mapping V1<->V2");
    TEST_RUN(test_v1_to_v2_bounds);
    TEST_RUN(test_v2_to_v1_bounds);
    TEST_RUN(test_roundtrip_v1_v2_v1);
    TEST_RUN(test_roundtrip_v2_v1_v2);
    TEST_RUN(test_packed_encoding);
}
