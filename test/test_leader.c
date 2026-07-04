/* Leader Key engine tests — VRAI module linké (../main/input/leader.c).
 * Pilote le vrai matcher interne (try_match, réputé alambiqué) via l'API :
 * leader_set → leader_start → leader_feed → (tick) → leader_consume.
 * host_clock pour la fenêtre LEADER_TIMEOUT_MS. Si le vrai matcher divergeait
 * de l'intention, un de ces tests rougirait (= vrai bug de prod à signaler). */
#include "test_framework.h"
#include "leader.h"
#include "host_clock.h"

static void ld_reset(void) { leader_init(); host_clock_reset(); }

static void set_entry(uint8_t idx, uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3,
                      uint8_t result, uint8_t mod) {
    leader_entry_t e = { .sequence = { s0, s1, s2, s3 }, .result = result, .result_mod = mod };
    leader_set(idx, &e);
}

/* 1. Séquence 1 touche [A] → ESC. */
static void test_ld_single(void) {
    ld_reset();
    set_entry(0, 0x04, 0, 0, 0, 0x29, 0);
    leader_start();
    TEST_ASSERT(leader_feed(0x04), "feed A absorbé");
    uint8_t mod = 0xFF;
    TEST_ASSERT_EQ(leader_consume(&mod), 0x29, "[A] → ESC");
    TEST_ASSERT_EQ(mod, 0, "pas de modificateur");
}

/* 2. Séquence 2 touches [F,S] → Ctrl+S. */
static void test_ld_two_key(void) {
    ld_reset();
    set_entry(0, 0x09, 0x16, 0, 0, 0x16, 0x01);
    leader_start();
    leader_feed(0x09);
    leader_feed(0x16);
    uint8_t mod = 0;
    TEST_ASSERT_EQ(leader_consume(&mod), 0x16, "[F,S] → S");
    TEST_ASSERT_EQ(mod, 0x01, "modificateur = Ctrl");
}

/* 3. Mauvaise touche → pas de match. */
static void test_ld_wrong_key(void) {
    ld_reset();
    set_entry(0, 0x04, 0, 0, 0, 0x29, 0);
    leader_start();
    leader_feed(0x05);            /* B, pas A */
    uint8_t mod;
    TEST_ASSERT_EQ(leader_consume(&mod), 0, "mauvaise touche → rien");
}

/* 4. Séquence partielle → pas de match, leader reste actif. */
static void test_ld_partial(void) {
    ld_reset();
    set_entry(0, 0x04, 0x05, 0, 0, 0x29, 0);   /* [A,B] */
    leader_start();
    leader_feed(0x04);            /* seulement A */
    uint8_t mod;
    TEST_ASSERT_EQ(leader_consume(&mod), 0, "partielle → pas de match");
    TEST_ASSERT(leader_is_active(), "toujours actif en attente de B");
}

/* 5. Entrée non configurée (result=0) ignorée. */
static void test_ld_unconfigured(void) {
    ld_reset();
    set_entry(0, 0x04, 0, 0, 0, 0, 0);   /* result = 0 */
    leader_start();
    leader_feed(0x04);
    uint8_t mod;
    TEST_ASSERT_EQ(leader_consume(&mod), 0, "result=0 → entrée ignorée");
}

/* 6. Plusieurs entrées : la bonne matche selon longueur + contenu. */
static void test_ld_multiple(void) {
    ld_reset();
    set_entry(0, 0x04, 0, 0, 0, 0x29, 0);       /* [A]   → ESC */
    set_entry(1, 0x05, 0, 0, 0, 0x28, 0);       /* [B]   → Enter */
    set_entry(2, 0x04, 0x05, 0, 0, 0x2A, 0);    /* [A,B] → Backspace */
    leader_start();
    leader_feed(0x05);                           /* B */
    uint8_t mod;
    TEST_ASSERT_EQ(leader_consume(&mod), 0x28, "[B] → Enter");
    leader_start();
    leader_feed(0x04);
    leader_feed(0x05);
    TEST_ASSERT_EQ(leader_consume(&mod), 0x2A, "[A,B] → Backspace");
}

/* 7. Séquence pleine (4 touches) → match (exerce le chemin sequence[j+1]). */
static void test_ld_four_key(void) {
    ld_reset();
    set_entry(0, 0x04, 0x05, 0x06, 0x07, 0x29, 0x02);
    leader_start();
    leader_feed(0x04); leader_feed(0x05); leader_feed(0x06); leader_feed(0x07);
    uint8_t mod = 0;
    TEST_ASSERT_EQ(leader_consume(&mod), 0x29, "[A,B,C,D] → ESC");
    TEST_ASSERT_EQ(mod, 0x02, "modificateur = Shift");
}

/* 8. Timeout : séquence partielle non complétée → tick annule après le délai. */
static void test_ld_timeout_cancels(void) {
    ld_reset();
    set_entry(0, 0x04, 0x05, 0, 0, 0x29, 0);   /* [A,B] */
    leader_start();
    leader_feed(0x04);                           /* partiel */
    host_clock_advance_ms(1000);                 /* == LEADER_TIMEOUT_MS */
    TEST_ASSERT(!leader_tick(), "timeout sans match complet → pas de résolution");
    TEST_ASSERT(!leader_is_active(), "timeout → leader désactivé");
}

void test_leader(void) {
    TEST_SUITE("Leader Key — module réel");
    TEST_RUN(test_ld_single);
    TEST_RUN(test_ld_two_key);
    TEST_RUN(test_ld_wrong_key);
    TEST_RUN(test_ld_partial);
    TEST_RUN(test_ld_unconfigured);
    TEST_RUN(test_ld_multiple);
    TEST_RUN(test_ld_four_key);
    TEST_RUN(test_ld_timeout_cancels);
    leader_init();   /* laisse le module propre */
}
