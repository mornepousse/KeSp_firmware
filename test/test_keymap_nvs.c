/* Test keymap data integrity (save/load round-trip simulation) */
#include "test_framework.h"

#define LAYERS 10
#define MAX_LAYOUT_NAME_LENGTH 15

/* Simulate NVS blob save/load as a memcpy round-trip */
static uint8_t nvs_blob[32768];
static size_t nvs_blob_size = 0;

static void fake_nvs_set_blob(const void *data, size_t size) {
    memcpy(nvs_blob, data, size);
    nvs_blob_size = size;
}

static bool fake_nvs_get_blob(void *data, size_t size) {
    if (nvs_blob_size == 0) return false;
    if (size != nvs_blob_size) return false;
    memcpy(data, nvs_blob, size);
    return true;
}

/* Test: keymap round-trip (save then load produces identical data) */
void test_keymap_roundtrip(void) {
    uint16_t original[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    uint16_t loaded[LAYERS][MATRIX_ROWS][MATRIX_COLS];

    /* Fill with test data */
    for (int l = 0; l < LAYERS; l++)
        for (int r = 0; r < MATRIX_ROWS; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                original[l][r][c] = (uint16_t)(l * 1000 + r * 100 + c);

    size_t size = sizeof(original);
    fake_nvs_set_blob(original, size);

    memset(loaded, 0xFF, sizeof(loaded));
    bool ok = fake_nvs_get_blob(loaded, size);

    TEST_ASSERT(ok, "blob loaded");
    TEST_ASSERT(memcmp(original, loaded, size) == 0, "keymap round-trip identical");
}

/* Test: layout names round-trip */
void test_layout_names_roundtrip(void) {
    char original[LAYERS][MAX_LAYOUT_NAME_LENGTH];
    char loaded[LAYERS][MAX_LAYOUT_NAME_LENGTH];

    /* Fill with test names */
    const char *names[] = {"DVORAK", "QWERTY", "AZERTY", "COLEMAK", "GAMING",
                           "NUMPAD", "NAV", "FN", "MEDIA", "SYSTEM"};
    for (int i = 0; i < LAYERS; i++) {
        strncpy(original[i], names[i], MAX_LAYOUT_NAME_LENGTH - 1);
        original[i][MAX_LAYOUT_NAME_LENGTH - 1] = '\0';
    }

    size_t size = sizeof(original);
    fake_nvs_set_blob(original, size);

    memset(loaded, 0, sizeof(loaded));
    bool ok = fake_nvs_get_blob(loaded, size);

    TEST_ASSERT(ok, "names loaded");
    for (int i = 0; i < LAYERS; i++) {
        TEST_ASSERT(strcmp(original[i], loaded[i]) == 0, "name matches");
    }
}

/* Test: layout name truncation */
void test_layout_name_truncation(void) {
    char name[MAX_LAYOUT_NAME_LENGTH];
    const char *long_name = "THIS_IS_WAY_TOO_LONG_FOR_A_LAYOUT_NAME";

    strncpy(name, long_name, MAX_LAYOUT_NAME_LENGTH - 1);
    name[MAX_LAYOUT_NAME_LENGTH - 1] = '\0';

    TEST_ASSERT(strlen(name) == MAX_LAYOUT_NAME_LENGTH - 1, "name truncated");
    TEST_ASSERT(name[MAX_LAYOUT_NAME_LENGTH - 1] == '\0', "null terminated");
}

/* Test: keymap size calculation */
void test_keymap_size(void) {
    size_t expected = LAYERS * MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t);
    TEST_ASSERT_EQ(expected, 10 * 5 * 13 * 2, "keymap size = 1300 bytes");
}

/* Test: single layer extraction */
void test_single_layer_extract(void) {
    uint16_t keymaps[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(keymaps, 0, sizeof(keymaps));

    /* Set a specific key in layer 3 */
    keymaps[3][2][5] = 0x04;  /* HID 'a' */

    /* Extract layer 3 as a flat buffer */
    uint16_t *layer_ptr = &keymaps[3][0][0];
    size_t layer_size = MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t);

    TEST_ASSERT_EQ(layer_size, 130, "single layer = 130 bytes");
    TEST_ASSERT_EQ(layer_ptr[2 * MATRIX_COLS + 5], 0x04, "key at (2,5) = 0x04");
}

/* Test: empty NVS returns false */
void test_nvs_empty(void) {
    nvs_blob_size = 0;
    uint16_t data[MATRIX_ROWS][MATRIX_COLS];
    bool ok = fake_nvs_get_blob(data, sizeof(data));
    TEST_ASSERT(!ok, "empty NVS returns false");
}

/* Test: wrong size blob rejected */
void test_nvs_wrong_size(void) {
    uint8_t small[10] = {0};
    fake_nvs_set_blob(small, sizeof(small));

    uint16_t data[MATRIX_ROWS][MATRIX_COLS];
    bool ok = fake_nvs_get_blob(data, sizeof(data));
    TEST_ASSERT(!ok, "wrong size rejected");
}

/* Test: macro structure layout */
void test_macro_structure(void) {
    typedef struct {
        char name[16];
        uint8_t keys[6];
        uint16_t key_definition;
    } macro_t;

    macro_t m;
    memset(&m, 0, sizeof(m));
    strncpy(m.name, "CopyPaste", 15);
    m.keys[0] = 0xE0;  /* Left Ctrl */
    m.keys[1] = 0x04;  /* 'a' */
    m.key_definition = 0x1100;

    TEST_ASSERT(strcmp(m.name, "CopyPaste") == 0, "macro name");
    TEST_ASSERT_EQ(m.keys[0], 0xE0, "first key = LCtrl");
    TEST_ASSERT_EQ(m.key_definition, 0x1100, "key definition");
}

void test_keymap_nvs(void) {
    TEST_SUITE("Keymap NVS Round-trip");
    TEST_RUN(test_keymap_roundtrip);
    TEST_RUN(test_layout_names_roundtrip);
    TEST_RUN(test_layout_name_truncation);
    TEST_RUN(test_keymap_size);
    TEST_RUN(test_single_layer_extract);
    TEST_RUN(test_nvs_empty);
    TEST_RUN(test_nvs_wrong_size);
    TEST_RUN(test_macro_structure);
}
