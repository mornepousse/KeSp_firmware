/* Test CDC protocol binary format construction */
#include "test_framework.h"

/* Test KEYSTATS header format (19 bytes) */
void test_keystats_header_format(void) {
    uint8_t header[19];
    memset(header, 0, sizeof(header));

    /* Magic: "KEYSTATS" (8 bytes, no null) */
    memcpy(header, "KEYSTATS", 8);
    TEST_ASSERT(memcmp(header, "KEYSTATS", 8) == 0, "magic correct");

    /* rows (offset 8) */
    header[8] = MATRIX_ROWS;
    TEST_ASSERT_EQ(header[8], 5, "rows = 5");

    /* cols (offset 9) */
    header[9] = MATRIX_COLS;
    TEST_ASSERT_EQ(header[9], 13, "cols = 13");

    /* hw_version (offset 10) */
    header[10] = 0x02;  /* VERSION_2_DEBUG */
    TEST_ASSERT_EQ(header[10], 0x02, "hw_version = V2");

    /* total_presses (offset 11, uint32_t LE) */
    uint32_t total = 12345;
    memcpy(&header[11], &total, 4);
    uint32_t read_total;
    memcpy(&read_total, &header[11], 4);
    TEST_ASSERT_EQ(read_total, 12345, "total encoded correctly");

    /* max_presses (offset 15, uint32_t LE) */
    uint32_t max = 500;
    memcpy(&header[15], &max, 4);
    uint32_t read_max;
    memcpy(&read_max, &header[15], 4);
    TEST_ASSERT_EQ(read_max, 500, "max encoded correctly");
}

/* Test KEYSTATS total packet size */
void test_keystats_packet_size(void) {
    size_t header_size = 19;
    size_t data_size = MATRIX_ROWS * MATRIX_COLS * sizeof(uint32_t);
    size_t end_marker = 2;  /* \r\n */
    size_t total = header_size + data_size + end_marker;
    TEST_ASSERT_EQ(data_size, 260, "data = 5*13*4 = 260 bytes");
    TEST_ASSERT_EQ(total, 281, "total packet = 281 bytes");
}

/* Test BIGRAMS header format (18 bytes) */
void test_bigrams_header_format(void) {
    uint8_t header[18];
    memset(header, 0, sizeof(header));

    /* Magic: "BIGRAMS\0" (7 chars + null = 8 bytes) */
    memcpy(header, "BIGRAMS\0", 8);
    TEST_ASSERT(memcmp(header, "BIGRAMS", 7) == 0, "bigram magic correct");
    TEST_ASSERT_EQ(header[7], 0, "null byte at offset 7");

    /* hw_version (offset 8) */
    header[8] = 0x02;
    TEST_ASSERT_EQ(header[8], 0x02, "hw_version");

    /* num_keys (offset 9) */
    header[9] = NUM_KEYS;
    TEST_ASSERT_EQ(header[9], 65, "num_keys = 65");

    /* total (offset 10, uint32_t LE) */
    uint32_t total = 5000;
    memcpy(&header[10], &total, 4);

    /* max_count (offset 14, uint16_t LE) */
    uint16_t max_count = 150;
    memcpy(&header[14], &max_count, 2);

    /* n_entries (offset 16, uint16_t LE) */
    uint16_t n_entries = 256;
    memcpy(&header[16], &n_entries, 2);

    uint16_t read_entries;
    memcpy(&read_entries, &header[16], 2);
    TEST_ASSERT_EQ(read_entries, 256, "n_entries encoded correctly");
}

/* Test bigram entry format (4 bytes each) */
void test_bigrams_entry_format(void) {
    uint8_t entry[4];
    entry[0] = 2 * 13 + 3;  /* prev_key: row 2, col 3 */
    entry[1] = 2 * 13 + 4;  /* curr_key: row 2, col 4 */
    uint16_t count = 150;
    memcpy(&entry[2], &count, 2);

    /* Decode */
    uint8_t prev_key = entry[0];
    uint8_t curr_key = entry[1];
    uint16_t decoded_count;
    memcpy(&decoded_count, &entry[2], 2);

    TEST_ASSERT_EQ(prev_key / 13, 2, "prev row = 2");
    TEST_ASSERT_EQ(prev_key % 13, 3, "prev col = 3");
    TEST_ASSERT_EQ(curr_key / 13, 2, "curr row = 2");
    TEST_ASSERT_EQ(curr_key % 13, 4, "curr col = 4");
    TEST_ASSERT_EQ(decoded_count, 150, "count = 150");
}

/* Test key index to row/col conversion */
void test_key_index_conversion(void) {
    for (int r = 0; r < MATRIX_ROWS; r++) {
        for (int c = 0; c < MATRIX_COLS; c++) {
            int idx = r * MATRIX_COLS + c;
            TEST_ASSERT_EQ(idx / MATRIX_COLS, r, "row from index");
            TEST_ASSERT_EQ(idx % MATRIX_COLS, c, "col from index");
        }
    }
}

/* Test max entries cap (256) */
void test_bigrams_max_entries(void) {
    uint16_t max_entries = 256;
    TEST_ASSERT(max_entries <= NUM_KEYS * NUM_KEYS, "256 <= 65*65");
    TEST_ASSERT(max_entries * 4 <= 1024, "max entry data = 1024 bytes");
}

/* Test: macro index calculation from keycode */
void test_macro_index_calculation(void) {
    /* Macro keycodes: MACRO_1 to MACRO_20, spaced by 256 */
    uint16_t MACRO_1 = 0x1100;  /* example base */
    for (int i = 0; i < 20; i++) {
        uint16_t keycode = MACRO_1 + i * 256;
        int16_t macro_i = (keycode - MACRO_1) / 256;
        TEST_ASSERT_EQ(macro_i, i, "macro index correct");
    }
}

void test_cdc_protocol(void) {
    TEST_SUITE("CDC Protocol Format");
    TEST_RUN(test_keystats_header_format);
    TEST_RUN(test_keystats_packet_size);
    TEST_RUN(test_bigrams_header_format);
    TEST_RUN(test_bigrams_entry_format);
    TEST_RUN(test_key_index_conversion);
    TEST_RUN(test_bigrams_max_entries);
    TEST_RUN(test_macro_index_calculation);
}
