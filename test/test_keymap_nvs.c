/* Tests of keymap.c's real persistence via the fake RAM-backed NVS.
 *
 * This TU links keymap.c (the real code) and nvs_fake.c (NVS primitives in RAM).
 * Each test calls nvs_fake_reset() to guarantee isolation.
 *
 * Logic under test:
 *   save_keymaps / load_keymaps
 *   save_layout_names / load_layout_names
 *   save_macros / load_macros
 *   recalc_macros_count
 *   load_macros size guard (stored_size != expected → skip)
 */
#include "test_framework.h"
#include "keymap.h"
#include "nvs_fake.h"
#include "keyboard_config.h"
#include <stddef.h>

/* ── 1. Round-trip keymaps ─────────────────────────────────────── */

static void test_keymaps_real_roundtrip(void)
{
    nvs_fake_reset();

    uint16_t src[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    uint16_t dst[LAYERS][MATRIX_ROWS][MATRIX_COLS];

    for (int l = 0; l < LAYERS; l++)
        for (int r = 0; r < MATRIX_ROWS; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                src[l][r][c] = (uint16_t)(l * 1000 + r * 100 + c + 1);

    save_keymaps((uint16_t *)src, sizeof(src));

    memset(dst, 0, sizeof(dst));
    load_keymaps((uint16_t *)dst, sizeof(dst));

    TEST_ASSERT(memcmp(src, dst, sizeof(src)) == 0,
                "save_keymaps then load_keymaps produces identical data");
    /* Check one specific key to prove this is not a no-op */
    TEST_ASSERT_EQ(dst[3][2][5], src[3][2][5],
                   "specific key [3][2][5] correctly restored");
}

/* ── 2. load_keymaps on empty NVS does not corrupt the buffer ──── */

static void test_load_keymaps_empty_nvs_unchanged(void)
{
    nvs_fake_reset();

    uint16_t buf[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(buf, 0xAB, sizeof(buf));

    load_keymaps((uint16_t *)buf, sizeof(buf));

    uint16_t sentinel[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(sentinel, 0xAB, sizeof(sentinel));
    TEST_ASSERT(memcmp(buf, sentinel, sizeof(buf)) == 0,
                "load_keymaps on empty NVS: buffer unchanged");
}

/* ── 2b. load_keymaps: stored blob of the WRONG size → guard → defaults (E3) ── */

static void test_load_keymaps_size_guard(void)
{
    nvs_fake_reset();
    /* "keymaps" blob smaller than the expected size (config from another build). */
    uint8_t fake_blob[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    nvs_fake_put_blob(STORAGE_NAMESPACE, "keymaps", fake_blob, sizeof(fake_blob));

    uint16_t buf[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(buf, 0xAB, sizeof(buf));
    load_keymaps((uint16_t *)buf, sizeof(buf));

    uint16_t sentinel[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(sentinel, 0xAB, sizeof(sentinel));
    TEST_ASSERT(memcmp(buf, sentinel, sizeof(buf)) == 0,
                "load_keymaps wrong size → defaults preserved (no partial fill)");
}

/* ── 3. Round-trip layout_names ─────────────────────────────── */

static void test_layout_names_real_roundtrip(void)
{
    nvs_fake_reset();

    char src[LAYERS][MAX_LAYOUT_NAME_LENGTH];
    char dst[LAYERS][MAX_LAYOUT_NAME_LENGTH];

    const char *names[] = { "DVORAK", "QWERTY", "AZERTY", "COLEMAK",
                            "GAMING", "NUMPAD", "NAV",    "FN",
                            "MEDIA",  "SYS" };
    for (int i = 0; i < LAYERS; i++) {
        strncpy(src[i], names[i], MAX_LAYOUT_NAME_LENGTH - 1);
        src[i][MAX_LAYOUT_NAME_LENGTH - 1] = '\0';
    }

    save_layout_names(src, LAYERS);

    memset(dst, 0, sizeof(dst));
    load_layout_names(dst, LAYERS);

    for (int i = 0; i < LAYERS; i++) {
        TEST_ASSERT(strcmp(src[i], dst[i]) == 0,
                    "save_layout_names then load: layer name identical");
    }
}

/* load_layout_names: blob of the WRONG size → guard → defaults (M10, mirrors E3). */
static void test_load_layout_names_size_guard(void)
{
    nvs_fake_reset();
    uint8_t fake_blob[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    nvs_fake_put_blob(STORAGE_NAMESPACE, "layout_names", fake_blob, sizeof(fake_blob));

    char buf[LAYERS][MAX_LAYOUT_NAME_LENGTH];
    memset(buf, 0xAB, sizeof(buf));
    load_layout_names(buf, LAYERS);

    char sentinel[LAYERS][MAX_LAYOUT_NAME_LENGTH];
    memset(sentinel, 0xAB, sizeof(sentinel));
    TEST_ASSERT(memcmp(buf, sentinel, sizeof(buf)) == 0,
                "load_layout_names wrong size → defaults preserved (guard)");
}

/* ── 4. Real macro_t layout (steps[] present and correctly placed) */

static void test_macro_t_struct_layout(void)
{
    TEST_ASSERT_EQ(offsetof(macro_t, name), 0,
                   "name at the start of macro_t (offset 0)");
    TEST_ASSERT_EQ(offsetof(macro_t, steps), (size_t)MAX_MACRO_NAME_LENGTH,
                   "steps[] immediately after name[MAX_MACRO_NAME_LENGTH]");
    TEST_ASSERT_EQ(sizeof(((macro_t *)0)->steps),
                   MACRO_MAX_STEPS * sizeof(macro_step_t),
                   "steps[] = MACRO_MAX_STEPS entries of 2 bytes");
    size_t min_size = sizeof(char[MAX_MACRO_NAME_LENGTH])
                    + sizeof(macro_step_t[MACRO_MAX_STEPS])
                    + sizeof(uint8_t[6])
                    + sizeof(uint16_t);
    TEST_ASSERT(sizeof(macro_t) >= min_size,
                "sizeof(macro_t) >= sum of declared fields");
}

/* ── 5. Round-trip macros (real struct with steps[]) ───────── */

static void test_macros_real_roundtrip(void)
{
    nvs_fake_reset();

    macro_t src[MAX_MACROS];
    macro_t dst[MAX_MACROS];
    memset(src, 0, sizeof(src));
    memset(dst, 0xFF, sizeof(dst));

    strncpy(src[0].name, "TestA", MAX_MACRO_NAME_LENGTH - 1);
    src[0].steps[0].keycode  = 0x04;
    src[0].steps[0].modifier = 0x00;
    src[0].key_definition    = 0x1500;

    strncpy(src[1].name, "TestB", MAX_MACRO_NAME_LENGTH - 1);
    src[1].steps[0].keycode  = 0x05;
    src[1].steps[0].modifier = 0x02;
    src[1].key_definition    = 0x1501;

    save_macros(src, 2);
    load_macros(dst, MAX_MACROS);

    TEST_ASSERT(memcmp(src, dst, sizeof(src)) == 0,
                "save_macros then load_macros: identical content (including steps[])");
    TEST_ASSERT_EQ(dst[0].steps[0].keycode, 0x04,
                   "macro[0].steps[0].keycode = 'A' (0x04)");
    TEST_ASSERT_EQ(dst[1].steps[0].modifier, 0x02,
                   "macro[1].steps[0].modifier = LSHIFT (0x02)");
}

/* ── 6. macros_count survives save/load ─────────── */

static void test_macros_count_persisted(void)
{
    nvs_fake_reset();

    macro_t buf[MAX_MACROS];
    memset(buf, 0, sizeof(buf));
    strncpy(buf[0].name, "X", MAX_MACRO_NAME_LENGTH - 1);
    strncpy(buf[1].name, "Y", MAX_MACRO_NAME_LENGTH - 1);
    strncpy(buf[2].name, "Z", MAX_MACRO_NAME_LENGTH - 1);

    save_macros(buf, 3);

    macros_count = 99;
    TEST_ASSERT_EQ((int)macros_count, 99,
                   "macros_count = 99 (sentinel before load)");
    load_macros(buf, MAX_MACROS);
    TEST_ASSERT_EQ((int)macros_count, 3,
                   "macros_count restored = 3 after load");
}

/* ── 7. Size guard: wrong-size blob → no load */

static void test_macro_size_guard_skips_load(void)
{
    nvs_fake_reset();

    /* Sentinel in macros_list[0] */
    macros_list[0].name[0] = '\xCC';

    /* Wrong-size blob (100 bytes != MAX_MACROS * sizeof(macro_t)) */
    uint8_t fake_blob[100] = {0};
    nvs_fake_put_blob(STORAGE_NAMESPACE, "macros", fake_blob, sizeof(fake_blob));

    load_macros(macros_list, MAX_MACROS);

    TEST_ASSERT(macros_list[0].name[0] == '\xCC',
                "load_macros: wrong-size blob → data unchanged (guard active)");
}

/* 7b. Version guard: a blob of the RIGHT size but a stale version (struct
 * reordered without changing size) → defaults preserved (M11). */
static void test_load_macros_version_guard(void)
{
    nvs_fake_reset();
    macro_t src[MAX_MACROS];
    memset(src, 0, sizeof(src));
    src[0].name[0] = 'x';
    save_macros(src, MAX_MACROS);                                  /* writes blob + current version */
    nvs_fake_put_u32(STORAGE_NAMESPACE, "macros_ver", 0xDEAD);     /* force a stale version */

    macro_t dst[MAX_MACROS];
    memset(dst, 0xAB, sizeof(dst));
    load_macros(dst, MAX_MACROS);

    macro_t sentinel[MAX_MACROS];
    memset(sentinel, 0xAB, sizeof(sentinel));
    TEST_ASSERT(memcmp(dst, sentinel, sizeof(dst)) == 0,
                "load_macros: stale version (right size) → defaults preserved (M11)");
}

/* ── 8. recalc_macros_count: counts the last non-empty name ── */

static void test_recalc_macros_count(void)
{
    macro_t buf[MAX_MACROS];
    memset(buf, 0, sizeof(buf));
    strncpy(buf[0].name, "A", MAX_MACRO_NAME_LENGTH - 1);
    strncpy(buf[3].name, "D", MAX_MACRO_NAME_LENGTH - 1);
    memcpy(macros_list, buf, sizeof(buf));

    macros_count = 0;
    recalc_macros_count();
    TEST_ASSERT_EQ((int)macros_count, 4,
                   "recalc: last non-empty name at index 3 → count = 4");

    memset(macros_list, 0, sizeof(macros_list));
    macros_count = 99;
    recalc_macros_count();
    TEST_ASSERT_EQ((int)macros_count, 0,
                   "recalc: all empty → count = 0");
}

/* ── save_keymaps propagates the NVS failure (E4) ─────────────────────── */

static void test_save_keymaps_propagates_error(void)
{
    nvs_fake_reset();
    uint16_t buf[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT(save_keymaps((uint16_t *)buf, sizeof(buf)),
                "save_keymaps with no fault → true (persisted)");
    nvs_fake_fail_writes(1);   /* simulate a full NVS */
    TEST_ASSERT(!save_keymaps((uint16_t *)buf, sizeof(buf)),
                "save_keymaps with full NVS → false (failure propagated, no false OK)");
    nvs_fake_fail_writes(0);
}

/* ── Suite runner ────────────────────────────────────────────── */

void test_keymap_nvs(void)
{
    TEST_SUITE("Keymap NVS — real persistence (keymap.c + fake RAM NVS)");
    TEST_RUN(test_keymaps_real_roundtrip);
    TEST_RUN(test_load_keymaps_empty_nvs_unchanged);
    TEST_RUN(test_load_keymaps_size_guard);
    TEST_RUN(test_save_keymaps_propagates_error);
    TEST_RUN(test_layout_names_real_roundtrip);
    TEST_RUN(test_load_layout_names_size_guard);
    TEST_RUN(test_macro_t_struct_layout);
    TEST_RUN(test_macros_real_roundtrip);
    TEST_RUN(test_macros_count_persisted);
    TEST_RUN(test_macro_size_guard_skips_load);
    TEST_RUN(test_load_macros_version_guard);
    TEST_RUN(test_recalc_macros_count);
}
