/* Tap/Hold engine tests — VRAI module linké (../main/input/tap_hold.c).
 * L'horloge est contrôlable : esp_timer_get_time() (défini ici, symbole global du
 * runner) renvoie g_now_us, avancé via advance_ms() — permet de tester la borne
 * exacte du timeout, l'interruption, l'épuisement de slots, etc. sur le vrai code. */
#include "test_framework.h"
#include "tap_hold.h"
#include "key_definitions.h"   /* K_MT / K_LT / K_OSM, MOD_* */
#include "key_features.h"      /* osm_is_active / osm_consume (branche OSM du tap) */

/* Horloge host contrôlable partagée (host_clock.c définit esp_timer_get_time) */
#include "host_clock.h"
static void advance_ms(uint32_t ms) { host_clock_advance_ms(ms); }

/* Globales de layer définies par key_processor.c (linké) — sauvées/restaurées
 * autour du test LT pour ne pas polluer les autres suites. */
extern uint8_t current_layout;
extern uint8_t last_layer;

static void th_reset(void) { tap_hold_init(); host_clock_reset(); }

/* 1. MT relâché avant le timeout → TAP : consume_tap rend la touche de tap. */
static void test_th_mt_tap(void) {
    th_reset();
    uint16_t mt = K_MT(MOD_LSFT, 0x04);   /* MT(Shift, A) */
    TEST_ASSERT(tap_hold_on_press(mt, 0, 0), "MT press → tracké");
    advance_ms(50);                        /* < 200ms */
    TEST_ASSERT(tap_hold_on_release(0, 0), "MT release tracké");
    TEST_ASSERT_EQ(tap_hold_consume_tap(), 0x04, "release rapide → tap = A (0x04)");
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), 0, "aucun mod hold après un tap");
}

/* 2. MT tenu au-delà du timeout → HOLD : le mod devient actif ; release le retire. */
static void test_th_mt_hold_timeout(void) {
    th_reset();
    uint16_t mt = K_MT(MOD_LSFT, 0x04);
    tap_hold_on_press(mt, 0, 0);
    advance_ms(200);                       /* == TAP_HOLD_TIMEOUT_MS */
    tap_hold_tick();
    TEST_ASSERT(tap_hold_hold_just_activated(), "tick au timeout → hold activé");
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), MOD_LSFT, "MT hold → Shift actif");
    bool is_hold = false;
    TEST_ASSERT_EQ(tap_hold_get_resolved(0, 0, &is_hold), mt, "resolved rend le keycode MT");
    TEST_ASSERT(is_hold, "resolved → is_hold=true");
    tap_hold_on_release(0, 0);
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), 0, "release du hold → mod retiré");
}

/* 3. Borne exacte du timeout : 199ms → pas de hold ; 200ms → hold. */
static void test_th_timeout_boundary(void) {
    th_reset();
    tap_hold_on_press(K_MT(MOD_LCTL, 0x05), 0, 0);
    advance_ms(199);
    tap_hold_tick();
    TEST_ASSERT(!tap_hold_hold_just_activated(), "199ms < timeout → pas de hold");
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), 0, "199ms → aucun mod");
    advance_ms(1);                         /* total 200ms */
    tap_hold_tick();
    TEST_ASSERT(tap_hold_hold_just_activated(), "200ms == timeout → hold");
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), MOD_LCTL, "200ms → Ctrl actif");
    tap_hold_on_release(0, 0);
}

/* 4. Interruption (autre touche) → HOLD immédiat, sans attendre le timeout. */
static void test_th_interrupt_forces_hold(void) {
    th_reset();
    tap_hold_on_press(K_MT(MOD_LALT, 0x06), 0, 0);
    advance_ms(10);                        /* bien avant le timeout */
    tap_hold_interrupt();
    TEST_ASSERT_EQ(tap_hold_get_active_mods(), MOD_LALT, "interrupt → Alt hold immédiat");
    tap_hold_on_release(0, 0);
}

/* 5. LT tenu → couche active ; release → restaurée. */
static void test_th_lt_hold_layer(void) {
    th_reset();
    uint8_t save_cur = current_layout, save_last = last_layer;
    current_layout = 0;
    tap_hold_on_press(K_LT(2, 0x2C), 0, 0);   /* LT(2, Space) */
    advance_ms(200);
    tap_hold_tick();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 2, "LT hold → couche active 2");
    tap_hold_on_release(0, 0);
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), -1, "release LT → plus de couche active");
    current_layout = save_cur; last_layer = save_last;
}

/* 6. OSM tapé → consume_tap arme le one-shot mod (pas de keycode direct). */
static void test_th_osm_tap_arms(void) {
    th_reset();
    (void)osm_consume();                   /* baseline OSM vide */
    tap_hold_on_press(K_OSM(MOD_LGUI), 0, 0);
    advance_ms(30);
    tap_hold_on_release(0, 0);
    TEST_ASSERT_EQ(tap_hold_consume_tap(), 0, "OSM tap ne rend pas de keycode direct");
    TEST_ASSERT(osm_is_active(), "OSM tap → one-shot GUI armé");
    (void)osm_consume();                   /* nettoie */
}

/* 7. Touche normale (non LT/MT/OSM) → non trackée. */
static void test_th_ignores_normal_key(void) {
    th_reset();
    TEST_ASSERT(!tap_hold_on_press(0x04, 0, 0), "touche normale A → non trackée");
}

/* 8. Épuisement des slots : TAP_HOLD_MAX_PENDING pris → le suivant échoue. */
static void test_th_slot_exhaustion(void) {
    th_reset();
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++)
        TEST_ASSERT(tap_hold_on_press(K_MT(MOD_LSFT, 0x04), 0, i), "slot libre pris");
    TEST_ASSERT(!tap_hold_on_press(K_MT(MOD_LSFT, 0x04), 1, 0),
                "au-delà de TAP_HOLD_MAX_PENDING → refusé");
    for (int i = 0; i < TAP_HOLD_MAX_PENDING; i++) tap_hold_on_release(0, i);
}

/* 9. LT avec couche HORS BORNES (>= LAYERS=10) → ignorée (sinon lecture OOB de
 *    keymaps[] + current_layout bloqué sur une couche illégale). */
static void test_th_lt_layer_out_of_bounds(void) {
    th_reset();
    uint8_t save_cur = current_layout, save_last = last_layer;
    current_layout = 0;
    tap_hold_on_press(K_LT(15, 0x2C), 0, 0);   /* couche 15 >= LAYERS */
    advance_ms(200);
    tap_hold_tick();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), -1,
                   "LT couche 15 (>= LAYERS) → ignorée, aucune couche active");
    TEST_ASSERT_EQ(current_layout, 0, "current_layout non corrompu par LT hors bornes");
    tap_hold_on_release(0, 0);
    current_layout = save_cur; last_layer = save_last;
}

/* 10. Deux LT tenues simultanément (bug audit E1) : relâcher la plus récente
 *     doit revenir à la couche de la LT encore tenue, PAS tout perdre ; relâcher
 *     les deux revient à la base. L'ancien code mettait active_hold_layer=-1 dès
 *     le 1er release → couche perdue, puis clavier bloqué. */
static void test_th_two_lt_concurrent(void) {
    th_reset();
    uint8_t save_cur = current_layout, save_last = last_layer;
    current_layout = 0;
    tap_hold_on_press(K_LT(1, 0x2C), 0, 0);   /* P1 = LT(1) */
    advance_ms(200); tap_hold_tick();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 1, "P1 tenu → couche 1");
    tap_hold_on_press(K_LT(2, 0x2D), 0, 1);   /* P2 = LT(2), plus récent */
    advance_ms(200); tap_hold_tick();
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 2, "P2 tenu → couche 2 (plus récent)");
    tap_hold_on_release(0, 1);                 /* relâche P2 (P1 toujours tenu) */
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), 1, "release P2 → retour couche 1 (P1 tenu)");
    tap_hold_on_release(0, 0);                 /* relâche P1 */
    TEST_ASSERT_EQ(tap_hold_get_active_layer(), -1, "release P1 → plus de couche LT");
    TEST_ASSERT_EQ(current_layout, 0, "retour à la base 0 (pas bloqué)");
    current_layout = save_cur; last_layer = save_last;
}

void test_tap_hold(void) {
    TEST_SUITE("Tap/Hold State Machine — module réel");
    TEST_RUN(test_th_mt_tap);
    TEST_RUN(test_th_mt_hold_timeout);
    TEST_RUN(test_th_timeout_boundary);
    TEST_RUN(test_th_interrupt_forces_hold);
    TEST_RUN(test_th_lt_hold_layer);
    TEST_RUN(test_th_lt_layer_out_of_bounds);
    TEST_RUN(test_th_two_lt_concurrent);
    TEST_RUN(test_th_osm_tap_arms);
    TEST_RUN(test_th_ignores_normal_key);
    TEST_RUN(test_th_slot_exhaustion);
    tap_hold_init();   /* laisse le module propre pour les suites suivantes */
}
