/* Tap Dance engine tests — VRAI module linké (../main/input/tap_dance.c).
 * Horloge contrôlable partagée (host_clock) : on avance le temps pour franchir
 * TAP_DANCE_TIMEOUT_MS et résoudre les danses sur le vrai code. */
#include "test_framework.h"
#include "tap_dance.h"
#include "host_clock.h"

static void td_reset(void) { tap_dance_init(); host_clock_reset(); }

/* Configure le slot 0 avec 1tap=A / 2tap=B / 3tap=C / hold=ESC. */
static void configure_slot0(void) {
    const uint8_t actions[4] = { 0x04, 0x05, 0x06, 0x29 };
    tap_dance_set(0, actions);
}

/* 1 tap, relâché, timeout → action[0] = A. */
static void test_td_single_tap(void) {
    td_reset(); configure_slot0();
    TEST_ASSERT(tap_dance_on_press(0, 0, 0), "TD press slot 0 absorbé");
    tap_dance_on_release(0, 0);
    host_clock_advance_ms(200);            /* == TAP_DANCE_TIMEOUT_MS */
    tap_dance_tick();
    TEST_ASSERT(tap_dance_just_resolved(), "1 tap + timeout → résolu");
    TEST_ASSERT_EQ(tap_dance_consume(), 0x04, "1 tap → action[0] = A");
}

/* 2 taps rapides, timeout → action[1] = B. */
static void test_td_double_tap(void) {
    td_reset(); configure_slot0();
    tap_dance_on_press(0, 0, 0);           /* count=1, last_tap=0 */
    tap_dance_on_release(0, 0);
    host_clock_advance_ms(50);             /* < timeout → pas de résolution */
    tap_dance_on_press(0, 0, 0);           /* count=2, last_tap=50 */
    tap_dance_on_release(0, 0);
    host_clock_advance_ms(200);            /* elapsed depuis last_tap = 200 */
    tap_dance_tick();
    TEST_ASSERT_EQ(tap_dance_consume(), 0x05, "2 taps → action[1] = B");
}

/* 3 taps → action[2] = C. */
static void test_td_triple_tap(void) {
    td_reset(); configure_slot0();
    tap_dance_on_press(0, 0, 0); tap_dance_on_release(0, 0);
    host_clock_advance_ms(50);
    tap_dance_on_press(0, 0, 0); tap_dance_on_release(0, 0);
    host_clock_advance_ms(50);
    tap_dance_on_press(0, 0, 0); tap_dance_on_release(0, 0);
    host_clock_advance_ms(200);
    tap_dance_tick();
    TEST_ASSERT_EQ(tap_dance_consume(), 0x06, "3 taps → action[2] = C");
}

/* > MAX_TAPS → résolution immédiate sur action[MAX_TAPS-1], sans tick. */
static void test_td_max_taps_clamped(void) {
    td_reset(); configure_slot0();
    for (int i = 0; i < TAP_DANCE_MAX_TAPS + 1; i++) {   /* 4 pressions */
        tap_dance_on_press(0, 0, 0);
        tap_dance_on_release(0, 0);
    }
    TEST_ASSERT_EQ(tap_dance_consume(), 0x06,
                   "4 taps (> MAX) → clamp action[MAX_TAPS-1] = C");
}

/* Tenu au-delà du timeout → action de hold = action[3] = ESC. */
static void test_td_hold_action(void) {
    td_reset(); configure_slot0();
    tap_dance_on_press(0, 0, 0);           /* key_held = true, pas de release */
    host_clock_advance_ms(200);
    tap_dance_tick();                      /* held + timeout → HOLDING → resolve(3) */
    TEST_ASSERT(tap_dance_just_resolved(), "hold + timeout → résolu");
    TEST_ASSERT_EQ(tap_dance_consume(), 0x29, "hold → action[3] = ESC");
}

/* Slot non configuré (actions toutes nulles) → rien de résolu, consume = 0. */
static void test_td_unconfigured_slot(void) {
    td_reset();
    const uint8_t zero[4] = { 0, 0, 0, 0 };
    tap_dance_set(1, zero);
    tap_dance_on_press(1, 0, 0);
    tap_dance_on_release(0, 0);
    host_clock_advance_ms(200);
    tap_dance_tick();
    TEST_ASSERT(!tap_dance_just_resolved(), "slot non configuré → pas de résolution");
    TEST_ASSERT_EQ(tap_dance_consume(), 0, "slot non configuré → consume = 0");
}

/* Index hors bornes → on_press refusé. */
static void test_td_bounds_reject(void) {
    td_reset();
    TEST_ASSERT(!tap_dance_on_press(TAP_DANCE_MAX_SLOTS, 0, 0),
                "index == MAX_SLOTS → refusé");
    TEST_ASSERT(!tap_dance_on_press(20, 0, 0), "index 20 → refusé");
}

/* Une autre danse (index différent) pendant COUNTING → rejetée. */
static void test_td_different_key_rejected(void) {
    td_reset(); configure_slot0();
    TEST_ASSERT(tap_dance_on_press(0, 0, 0), "danse 0 démarrée");
    TEST_ASSERT(!tap_dance_on_press(1, 0, 1),
                "autre index pendant COUNTING → rejeté");
    tap_dance_on_release(0, 0);
}

/* 4ᵉ tap résout en on_press ; un tick tombant avant la consommation ne doit PAS
 * effacer la résolution (audit M2 : tick faisait resolved_flag=false aveuglément
 * → le keycode 3-tap n'était jamais envoyé). */
static void test_td_max_taps_survives_tick(void) {
    td_reset(); configure_slot0();
    for (int i = 0; i < TAP_DANCE_MAX_TAPS + 1; i++) {   /* 4 pressions → résout en on_press */
        tap_dance_on_press(0, 0, 0);
        tap_dance_on_release(0, 0);
    }
    tap_dance_tick();   /* un tick tombe avant que le consumer ne lise la résolution */
    TEST_ASSERT(tap_dance_just_resolved(),
                "4ᵉ tap résolu en on_press survit à un tick (M2)");
    TEST_ASSERT_EQ(tap_dance_consume(), 0x06,
                   "4 taps → action[2]=C consommé même après un tick");
}

void test_tap_dance(void) {
    TEST_SUITE("Tap Dance — module réel");
    TEST_RUN(test_td_single_tap);
    TEST_RUN(test_td_max_taps_survives_tick);
    TEST_RUN(test_td_double_tap);
    TEST_RUN(test_td_triple_tap);
    TEST_RUN(test_td_max_taps_clamped);
    TEST_RUN(test_td_hold_action);
    TEST_RUN(test_td_unconfigured_slot);
    TEST_RUN(test_td_bounds_reject);
    TEST_RUN(test_td_different_key_rejected);
    tap_dance_init();   /* laisse le module propre */
}
