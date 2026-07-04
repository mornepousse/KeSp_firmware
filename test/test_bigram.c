/* Test bigram tracking logic — rewired sur main/input/key_stats.c */
#include "test_framework.h"
#include "key_stats.h"

/*
 * Le bigramme est un effet de bord de key_stats_record_press().
 * Chaque test appelle reset_bigram_stats() en début — remet last_key_idx
 * interne à -1 et efface bigram_stats / bigram_total.
 * key_stats[][] accumule en parallèle ; on ne le teste pas ici.
 */

/* Test: première frappe ne crée pas de bigramme */
static void test_bigram_first_key_no_bigram(void) {
    reset_bigram_stats();
    key_stats_record_press(2, 3);
    TEST_ASSERT_EQ(bigram_total, 0, "premiere frappe: aucun bigramme");
}

/* Test: deux frappes consécutives créent exactement un bigramme */
static void test_bigram_two_consecutive(void) {
    reset_bigram_stats();
    int a = 2 * MATRIX_COLS + 3;
    int b = 1 * MATRIX_COLS + 5;
    key_stats_record_press(2, 3);  /* A */
    key_stats_record_press(1, 5);  /* B */
    TEST_ASSERT_EQ(bigram_stats[a][b], 1, "A->B bigramme == 1");
    TEST_ASSERT_EQ(bigram_total, 1, "total == 1");
}

/* Test: séquence répétée accumule A->B et B->A correctement */
static void test_bigram_repeated_sequence(void) {
    reset_bigram_stats();
    int a = 2 * MATRIX_COLS + 3;
    int b = 1 * MATRIX_COLS + 5;
    for (int i = 0; i < 10; i++) {
        key_stats_record_press(2, 3);
        key_stats_record_press(1, 5);
    }
    /* A->B: 10 fois ; B->A: 9 fois (B n'est pas suivi de A à la dernière itération) */
    TEST_ASSERT_EQ(bigram_stats[a][b], 10, "A->B == 10");
    TEST_ASSERT_EQ(bigram_stats[b][a], 9,  "B->A == 9");
    TEST_ASSERT_EQ(bigram_total, 19, "total == 19");
}

/* Test: saturation à UINT16_MAX — le compteur ne dépasse jamais UINT16_MAX */
static void test_bigram_saturation(void) {
    reset_bigram_stats();
    /* Préparer last_key_idx interne = 0 (key 0,0) sans créer de bigramme */
    key_stats_record_press(0, 0);
    /* Pré-charger le compteur A(0)->B(1) à UINT16_MAX - 1 */
    bigram_stats[0][1] = UINT16_MAX - 1;
    /* Frappe B : doit atteindre exactement UINT16_MAX */
    key_stats_record_press(0, 1);
    TEST_ASSERT_EQ(bigram_stats[0][1], UINT16_MAX, "atteint UINT16_MAX");
    /* Remettre last_key_idx à 0 via frappe A, puis tenter d'incrémenter au-delà */
    key_stats_record_press(0, 0);  /* crée bigramme B->A en passant, inoffensif */
    key_stats_record_press(0, 1);  /* A->B déjà à UINT16_MAX, ne doit pas incrémenter */
    TEST_ASSERT_EQ(bigram_stats[0][1], UINT16_MAX, "reste a UINT16_MAX apres saturation");
}

/* Test: auto-bigramme — même touche deux fois enregistre [idx][idx] */
static void test_bigram_self(void) {
    reset_bigram_stats();
    int idx = 2 * MATRIX_COLS + 5;
    key_stats_record_press(2, 5);
    key_stats_record_press(2, 5);
    TEST_ASSERT_EQ(bigram_stats[idx][idx], 1, "auto-bigramme (2,5)->(2,5) == 1");
}

/* Test: reset_bigram_stats efface les données et remet last_key_idx à -1 */
static void test_bigram_reset(void) {
    reset_bigram_stats();
    key_stats_record_press(0, 0);
    key_stats_record_press(1, 1);
    reset_bigram_stats();
    TEST_ASSERT_EQ(bigram_total, 0, "reset efface total");
    TEST_ASSERT_EQ(get_bigram_stats_max(), 0, "reset efface les compteurs");
    /* Apres reset, last_key_idx == -1 : une seule frappe ne crée pas de bigramme */
    key_stats_record_press(0, 0);
    TEST_ASSERT_EQ(bigram_total, 0, "first press post-reset: aucun bigramme (last_key_idx remis a -1)");
}

void test_bigram(void) {
    TEST_SUITE("Bigram Tracking");
    TEST_RUN(test_bigram_first_key_no_bigram);
    TEST_RUN(test_bigram_two_consecutive);
    TEST_RUN(test_bigram_repeated_sequence);
    TEST_RUN(test_bigram_saturation);
    TEST_RUN(test_bigram_self);
    TEST_RUN(test_bigram_reset);
}
