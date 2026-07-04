/* Combo engine tests — VRAI module linké (../main/input/combo.c).
 * Combo = 2 touches déférées puis présentes ensemble dans la fenêtre
 * COMBO_TIMEOUT_MS → result émis, les deux touches sources supprimées.
 * Horloge partagée host_clock pour tester la fenêtre / le timeout. */
#include "test_framework.h"
#include "combo.h"
#include "host_clock.h"

#define INVALID 0xFF

static void press2(uint8_t r1, uint8_t c1, uint8_t r2, uint8_t c2,
                   uint8_t pr[6], uint8_t pc[6]) {
    for (int i = 0; i < 6; i++) { pr[i] = INVALID; pc[i] = INVALID; }
    pr[0] = r1; pc[0] = c1; pr[1] = r2; pc[1] = c2;
}
static void press1(uint8_t r, uint8_t c, uint8_t pr[6], uint8_t pc[6]) {
    for (int i = 0; i < 6; i++) { pr[i] = INVALID; pc[i] = INVALID; }
    pr[0] = r; pc[0] = c;
}
static void press_none(uint8_t pr[6], uint8_t pc[6]) {
    for (int i = 0; i < 6; i++) { pr[i] = INVALID; pc[i] = INVALID; }
}

static void cb_reset(void) { combo_init(); host_clock_reset(); }

/* 1. Combo déclenché : 2 touches déférées + présentes → result + suppression. */
static void test_cb_fires(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    TEST_ASSERT(combo_should_defer(0, 0), "touche membre → should_defer");
    combo_defer_key(0, 0, 0x0A);
    combo_defer_key(0, 1, 0x0B);
    uint8_t pr[6], pc[6]; press2(0, 0, 0, 1, pr, pc);
    TEST_ASSERT_EQ(combo_process(pr, pc), 1, "les 2 présentes+déférées → 1 combo résolu");
    uint8_t r1, c1, r2, c2;
    TEST_ASSERT_EQ(combo_consume(&r1, &c1, &r2, &c2), 0x29, "consume → result ESC");
    TEST_ASSERT(combo_is_suppressed(0, 0), "touche du combo actif → supprimée");
}

/* 2. should_defer : membre oui / hors-combo non / slot non configuré non. */
static void test_cb_should_defer(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 1, .col1 = 2, .row2 = 1, .col2 = 3, .result = 0x28 };
    combo_set(0, &cfg);
    TEST_ASSERT(combo_should_defer(1, 2), "membre du combo → defer");
    TEST_ASSERT(!combo_should_defer(5, 5), "touche hors combo → pas de defer");
    combo_config_t zero = { 0 };
    combo_set(1, &zero);
    TEST_ASSERT(!combo_should_defer(0, 0), "slot non configuré (result=0) → pas de defer");
}

/* 3. Une seule touche présente → pas de résolution. */
static void test_cb_one_key_no_fire(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    combo_defer_key(0, 0, 0x0A);
    uint8_t pr[6], pc[6]; press1(0, 0, pr, pc);
    TEST_ASSERT_EQ(combo_process(pr, pc), 0, "une seule touche → 0 combo");
    uint8_t d;
    TEST_ASSERT_EQ(combo_consume(&d, &d, &d, &d), 0, "rien à consommer");
}

/* 4. Timeout dépassé, touche toujours pressée seule → ressort en 'expired'. */
static void test_cb_timeout_expired(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    combo_defer_key(0, 0, 0x0A);       /* press_time = 0 */
    host_clock_advance_ms(51);         /* > COMBO_TIMEOUT_MS (50) */
    uint8_t pr[6], pc[6]; press1(0, 0, pr, pc);
    combo_process(pr, pc);
    TEST_ASSERT_EQ(combo_consume_expired(), 0x0A, "timeout → touche déférée ressort telle quelle");
}

/* 5. Le partenaire arrive dans la fenêtre → combo résolu. */
static void test_cb_partner_in_time(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    combo_defer_key(0, 0, 0x0A);       /* t = 0 */
    host_clock_advance_ms(30);         /* < 50ms */
    combo_defer_key(0, 1, 0x0B);
    uint8_t pr[6], pc[6]; press2(0, 0, 0, 1, pr, pc);
    TEST_ASSERT_EQ(combo_process(pr, pc), 1, "partenaire à temps → combo résolu");
    uint8_t d;
    TEST_ASSERT_EQ(combo_consume(&d, &d, &d, &d), 0x29, "result ESC");
}

/* 6. Touche déférée relâchée (absente du report) → ressort, pas de combo. */
static void test_cb_released_expires(void) {
    cb_reset();
    combo_config_t cfg = { .row1 = 0, .col1 = 0, .row2 = 0, .col2 = 1, .result = 0x29 };
    combo_set(0, &cfg);
    combo_defer_key(0, 0, 0x0A);
    uint8_t pr[6], pc[6]; press_none(pr, pc);   /* touche relâchée avant partenaire */
    combo_process(pr, pc);
    TEST_ASSERT_EQ(combo_consume_expired(), 0x0A, "relâchée avant partenaire → ressort");
}

void test_combo(void) {
    TEST_SUITE("Combos — module réel");
    TEST_RUN(test_cb_fires);
    TEST_RUN(test_cb_should_defer);
    TEST_RUN(test_cb_one_key_no_fire);
    TEST_RUN(test_cb_timeout_expired);
    TEST_RUN(test_cb_partner_in_time);
    TEST_RUN(test_cb_released_expires);
    combo_init();   /* laisse le module propre */
}
