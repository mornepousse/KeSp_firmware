/* Tests de la persistance réelle de keymap.c via le fake NVS RAM-backed.
 *
 * Ce TU linke keymap.c (le vrai code) et nvs_fake.c (primitives NVS en RAM).
 * Chaque test appelle nvs_fake_reset() pour garantir l'isolation.
 *
 * Logique testée :
 *   save_keymaps / load_keymaps
 *   save_layout_names / load_layout_names
 *   save_macros / load_macros
 *   recalc_macros_count
 *   garde de taille de load_macros (stored_size != expected → skip)
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
                "save_keymaps puis load_keymaps produit les donnees identiques");
    /* Verifier une cle specifique pour prouver que ce n'est pas un no-op */
    TEST_ASSERT_EQ(dst[3][2][5], src[3][2][5],
                   "cle specifique [3][2][5] correctement restauree");
}

/* ── 2. load_keymaps sur NVS vide ne corrompt pas le buffer ──── */

static void test_load_keymaps_empty_nvs_unchanged(void)
{
    nvs_fake_reset();

    uint16_t buf[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(buf, 0xAB, sizeof(buf));

    load_keymaps((uint16_t *)buf, sizeof(buf));

    uint16_t sentinel[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(sentinel, 0xAB, sizeof(sentinel));
    TEST_ASSERT(memcmp(buf, sentinel, sizeof(buf)) == 0,
                "load_keymaps sur NVS vide : buffer inchange");
}

/* ── 2b. load_keymaps : blob stocké de MAUVAISE taille → garde → défauts (E3) ── */

static void test_load_keymaps_size_guard(void)
{
    nvs_fake_reset();
    /* Blob "keymaps" plus petit que la taille attendue (config d'un autre build). */
    uint8_t fake_blob[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    nvs_fake_put_blob(STORAGE_NAMESPACE, "keymaps", fake_blob, sizeof(fake_blob));

    uint16_t buf[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(buf, 0xAB, sizeof(buf));
    load_keymaps((uint16_t *)buf, sizeof(buf));

    uint16_t sentinel[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(sentinel, 0xAB, sizeof(sentinel));
    TEST_ASSERT(memcmp(buf, sentinel, sizeof(buf)) == 0,
                "load_keymaps taille incorrecte → défauts préservés (pas de remplissage partiel)");
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
                    "save_layout_names puis load : nom de couche identique");
    }
}

/* ── 4. Structure macro_t reelle (steps[] present et bien place) */

static void test_macro_t_struct_layout(void)
{
    TEST_ASSERT_EQ(offsetof(macro_t, name), 0,
                   "name au debut de macro_t (offset 0)");
    TEST_ASSERT_EQ(offsetof(macro_t, steps), (size_t)MAX_MACRO_NAME_LENGTH,
                   "steps[] immediatement apres name[MAX_MACRO_NAME_LENGTH]");
    TEST_ASSERT_EQ(sizeof(((macro_t *)0)->steps),
                   MACRO_MAX_STEPS * sizeof(macro_step_t),
                   "steps[] = MACRO_MAX_STEPS entrees de 2 octets");
    size_t min_size = sizeof(char[MAX_MACRO_NAME_LENGTH])
                    + sizeof(macro_step_t[MACRO_MAX_STEPS])
                    + sizeof(uint8_t[6])
                    + sizeof(uint16_t);
    TEST_ASSERT(sizeof(macro_t) >= min_size,
                "sizeof(macro_t) >= somme des champs declares");
}

/* ── 5. Round-trip macros (vraie struct avec steps[]) ───────── */

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
                "save_macros puis load_macros : contenu identique (inclus steps[])");
    TEST_ASSERT_EQ(dst[0].steps[0].keycode, 0x04,
                   "macro[0].steps[0].keycode = 'A' (0x04)");
    TEST_ASSERT_EQ(dst[1].steps[0].modifier, 0x02,
                   "macro[1].steps[0].modifier = LSHIFT (0x02)");
}

/* ── 6. macros_count persiste a travers save/load ─────────── */

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
                   "macros_count = 99 (sentinelle avant load)");
    load_macros(buf, MAX_MACROS);
    TEST_ASSERT_EQ((int)macros_count, 3,
                   "macros_count restaure = 3 apres load");
}

/* ── 7. Garde de taille : blob de mauvaise taille → pas de load */

static void test_macro_size_guard_skips_load(void)
{
    nvs_fake_reset();

    /* Sentinelle dans macros_list[0] */
    macros_list[0].name[0] = '\xCC';

    /* Blob de taille incorrecte (100 octets != MAX_MACROS * sizeof(macro_t)) */
    uint8_t fake_blob[100] = {0};
    nvs_fake_put_blob(STORAGE_NAMESPACE, "macros", fake_blob, sizeof(fake_blob));

    load_macros(macros_list, MAX_MACROS);

    TEST_ASSERT(macros_list[0].name[0] == '\xCC',
                "load_macros : blob taille incorrecte → donnees inchangees (garde active)");
}

/* ── 8. recalc_macros_count : compte le dernier nom non-vide ── */

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
                   "recalc: dernier nom non-vide a index 3 → count = 4");

    memset(macros_list, 0, sizeof(macros_list));
    macros_count = 99;
    recalc_macros_count();
    TEST_ASSERT_EQ((int)macros_count, 0,
                   "recalc: tous vides → count = 0");
}

/* ── save_keymaps propage l'échec NVS (E4) ─────────────────────── */

static void test_save_keymaps_propagates_error(void)
{
    nvs_fake_reset();
    uint16_t buf[LAYERS][MATRIX_ROWS][MATRIX_COLS];
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT(save_keymaps((uint16_t *)buf, sizeof(buf)),
                "save_keymaps sans faute → true (persisté)");
    nvs_fake_fail_writes(1);   /* simule NVS pleine */
    TEST_ASSERT(!save_keymaps((uint16_t *)buf, sizeof(buf)),
                "save_keymaps avec NVS pleine → false (échec propagé, pas de faux OK)");
    nvs_fake_fail_writes(0);
}

/* ── Suite runner ────────────────────────────────────────────── */

void test_keymap_nvs(void)
{
    TEST_SUITE("Keymap NVS — persistance reelle (keymap.c + fake NVS RAM)");
    TEST_RUN(test_keymaps_real_roundtrip);
    TEST_RUN(test_load_keymaps_empty_nvs_unchanged);
    TEST_RUN(test_load_keymaps_size_guard);
    TEST_RUN(test_save_keymaps_propagates_error);
    TEST_RUN(test_layout_names_real_roundtrip);
    TEST_RUN(test_macro_t_struct_layout);
    TEST_RUN(test_macros_real_roundtrip);
    TEST_RUN(test_macros_count_persisted);
    TEST_RUN(test_macro_size_guard_skips_load);
    TEST_RUN(test_recalc_macros_count);
}
