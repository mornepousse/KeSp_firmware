/* Tests pour la logique du moteur tamagotchi — rewired sur le vrai tama_engine.c.
 *
 * Tous les tests passent par l'API publique + le test accessor
 * tama_engine_test_set_stats() (disponible sous TEST_HOST uniquement).
 * reset_tama() avant chaque test garantit un état déterministe.
 *
 * Comportements couverts que la copie locale omettait :
 *  - Hystérésis STATE_HOLD_MIN : transition retardée jusqu'au 50e keypress
 *  - xp_for_level scaling +250/niveau (pas un seuil plat)
 *  - Level-up multi-niveaux via while-loop
 *  - Clamp critter à MAX_LEVEL=19 pour level>19
 */
#include "test_framework.h"
#include "tama_engine.h"
#include "host_clock.h"

/* Réinitialise le moteur à son état par défaut entre chaque test.
 * tama_engine_init() sous TEST_HOST : memset stats, valeurs par défaut,
 * reset de state/state_hold_keys/decay ticks (NVS gated). */
static void reset_tama(void)
{
    host_clock_reset();
    tama_engine_init();
}

/* ── Stat clamping (via clamp_stats() dans le vrai module) ─────────── */

static void test_tama_clamp_max(void)
{
    reset_tama();
    /* tama_engine_test_set_stats appelle clamp_stats() en interne */
    tama_engine_test_set_stats(1500, 1200, 1100, 0, 0);
    const tama2_stats_t *s = tama_engine_get_stats();
    TEST_ASSERT_EQ(s->hunger,    1000, "Hunger clamped to TAMA2_STAT_MAX");
    TEST_ASSERT_EQ(s->happiness, 1000, "Happiness clamped to TAMA2_STAT_MAX");
    TEST_ASSERT_EQ(s->energy,    1000, "Energy clamped to TAMA2_STAT_MAX");
}

static void test_tama_health_average(void)
{
    reset_tama();
    tama_engine_test_set_stats(600, 300, 900, 0, 0);
    const tama2_stats_t *s = tama_engine_get_stats();
    TEST_ASSERT_EQ(s->health, 600, "Health = (600+300+900)/3 = 600");
}

static void test_tama_health_zero(void)
{
    reset_tama();
    tama_engine_test_set_stats(0, 0, 0, 0, 0);
    const tama2_stats_t *s = tama_engine_get_stats();
    TEST_ASSERT_EQ(s->health, 0, "Health = 0 quand toutes les stats = 0");
}

/* ── State transitions (via update_state() déclenché par keypress) ─── */

/* Chaque test injecte des stats favorables à un état, puis appuie
 * STATE_HOLD_MIN=50 fois pour déclencher le changement d'état. */

static void test_tama_state_sick(void)
{
    reset_tama();
    /* health = (100+100+100)/3 = 100 < 200 → SICK */
    tama_engine_test_set_stats(100, 100, 100, 0, 0);
    for (int i = 0; i < 50; i++)
        tama_engine_keypress(0);
    TEST_ASSERT_EQ(tama_engine_get_state(), TAMA2_SICK,
                   "health < 200 → SICK après STATE_HOLD_MIN keypresses");
}

static void test_tama_state_sad(void)
{
    reset_tama();
    /* happiness=100 < 200, health=566 > 200 → SAD */
    tama_engine_test_set_stats(800, 100, 800, 0, 0);
    for (int i = 0; i < 50; i++)
        tama_engine_keypress(0);
    TEST_ASSERT_EQ(tama_engine_get_state(), TAMA2_SAD,
                   "happiness < 200 → SAD après STATE_HOLD_MIN keypresses");
}

static void test_tama_state_sleeping(void)
{
    reset_tama();
    /* energy=50 < 100 → SLEEPING (50 keypresses < decay thresholds → pas de décroissance) */
    tama_engine_test_set_stats(800, 800, 50, 0, 0);
    for (int i = 0; i < 50; i++)
        tama_engine_keypress(0);
    TEST_ASSERT_EQ(tama_engine_get_state(), TAMA2_SLEEPING,
                   "energy < 100 → SLEEPING après STATE_HOLD_MIN keypresses");
}

static void test_tama_state_sleepy(void)
{
    reset_tama();
    /* energy=250, 100 ≤ 250 < 300 → SLEEPY */
    tama_engine_test_set_stats(800, 800, 250, 0, 0);
    for (int i = 0; i < 50; i++)
        tama_engine_keypress(0);
    TEST_ASSERT_EQ(tama_engine_get_state(), TAMA2_SLEEPY,
                   "100 ≤ energy < 300 → SLEEPY après STATE_HOLD_MIN keypresses");
}

static void test_tama_state_excited(void)
{
    reset_tama();
    /* stats par défaut (energy=1000, health=933) : kpm=300 > 200 → EXCITED */
    for (int i = 0; i < 50; i++)
        tama_engine_keypress(300);
    TEST_ASSERT_EQ(tama_engine_get_state(), TAMA2_EXCITED,
                   "kpm > 200 → EXCITED après STATE_HOLD_MIN keypresses");
}

static void test_tama_state_happy(void)
{
    reset_tama();
    /* 80 < kpm=120 ≤ 200 → HAPPY */
    for (int i = 0; i < 50; i++)
        tama_engine_keypress(120);
    TEST_ASSERT_EQ(tama_engine_get_state(), TAMA2_HAPPY,
                   "80 < kpm ≤ 200 → HAPPY après STATE_HOLD_MIN keypresses");
}

static void test_tama_state_idle(void)
{
    reset_tama();
    /* kpm=30 < 80, bonnes stats → new_state = IDLE = état initial → inchangé */
    for (int i = 0; i < 50; i++)
        tama_engine_keypress(30);
    TEST_ASSERT_EQ(tama_engine_get_state(), TAMA2_IDLE,
                   "kpm < 80, bonnes stats → IDLE (état initial préservé)");
}

/* ── Hystérésis STATE_HOLD_MIN ─────────────────────────────────────── */

/* L'état NE change PAS avant STATE_HOLD_MIN keypresses consécutifs vers
 * le même new_state — comportement absent de l'ancienne copie locale. */
static void test_tama_hysteresis_not_before_hold_min(void)
{
    reset_tama();
    /* 49 keypresses kpm=300 : state_hold_keys va de 0 à 49 < 50 → IDLE */
    for (int i = 0; i < 49; i++)
        tama_engine_keypress(300);
    TEST_ASSERT_EQ(tama_engine_get_state(), TAMA2_IDLE,
                   "< STATE_HOLD_MIN keypresses → état inchangé (hystérésis)");
}

static void test_tama_hysteresis_changes_at_hold_min(void)
{
    reset_tama();
    /* Au 50e keypress : state_hold_keys = 50 ≥ 50 → bascule vers EXCITED */
    for (int i = 0; i < 50; i++)
        tama_engine_keypress(300);
    TEST_ASSERT_EQ(tama_engine_get_state(), TAMA2_EXCITED,
                   "= STATE_HOLD_MIN keypresses → état bascule vers EXCITED");
}

/* ── Priorité des états ─────────────────────────────────────────────── */

static void test_tama_sick_overrides_excited(void)
{
    reset_tama();
    /* health < 200 même si kpm > 200 → SICK prioritaire sur EXCITED */
    tama_engine_test_set_stats(50, 50, 50, 0, 0);
    for (int i = 0; i < 50; i++)
        tama_engine_keypress(300);
    TEST_ASSERT_EQ(tama_engine_get_state(), TAMA2_SICK,
                   "SICK prioritaire sur EXCITED (health < 200)");
}

/* ── Actions ────────────────────────────────────────────────────────── */

static void test_tama_feed(void)
{
    reset_tama();
    tama_engine_test_set_stats(500, 800, 800, 0, 0);
    tama_engine_action(TAMA2_ACTION_FEED);      /* +300 → 800 */
    TEST_ASSERT_EQ(tama_engine_get_stats()->hunger, 800,
                   "FEED : 500 + 300 = 800");
}

static void test_tama_feed_clamp(void)
{
    reset_tama();
    tama_engine_test_set_stats(900, 800, 800, 0, 0);
    tama_engine_action(TAMA2_ACTION_FEED);      /* 900 + 300 = 1200 → clamped 1000 */
    TEST_ASSERT_EQ(tama_engine_get_stats()->hunger, 1000,
                   "FEED : 900 + 300 clamped à TAMA2_STAT_MAX");
}

static void test_tama_play(void)
{
    reset_tama();
    tama_engine_test_set_stats(800, 600, 800, 0, 0);
    tama_engine_action(TAMA2_ACTION_PLAY);      /* +200 → 800 */
    TEST_ASSERT_EQ(tama_engine_get_stats()->happiness, 800,
                   "PLAY : 600 + 200 = 800");
}

static void test_tama_sleep_action(void)
{
    reset_tama();
    tama_engine_test_set_stats(800, 800, 400, 0, 0);
    tama_engine_action(TAMA2_ACTION_SLEEP);     /* +400 → 800 */
    TEST_ASSERT_EQ(tama_engine_get_stats()->energy, 800,
                   "SLEEP action : 400 + 400 = 800");
}

/* ── XP scaling : +250 XP seuil par niveau ─────────────────────────── */

/* Vérifie que le seuil level N→N+1 croît de +250 par niveau.
 * xp_for_level(level) = 500 + level * 250.
 * La copie locale utilisait un seuil plat de 500 — ce test le détecte. */
static void test_tama_xp_scaling_per_level(void)
{
    reset_tama();
    /* Level 0→1 : seuil = 500. Inject xp=449, +50 via 1000 keypresses → xp=499 < 500 : pas de level-up */
    tama_engine_test_set_stats(800, 800, 800, 0, 449);
    for (int i = 0; i < 1000; i++)
        tama_engine_keypress(0);
    TEST_ASSERT_EQ(tama_engine_get_stats()->level, 0,
                   "xp=449+50=499 < seuil 500 → pas de level-up niveau 0");

    /* Level 0→1 : inject xp=499, +50 → xp=549 ≥ 500 → level-up, xp=49 */
    tama_engine_test_set_stats(800, 800, 800, 0, 499);
    for (int i = 0; i < 1000; i++)
        tama_engine_keypress(0);
    const tama2_stats_t *s = tama_engine_get_stats();
    TEST_ASSERT_EQ(s->level, 1, "xp=499+50=549 ≥ 500 → level 0→1");
    TEST_ASSERT_EQ(s->xp,   49, "XP restant = 549 - 500 = 49");

    /* Level 1→2 : seuil = 750 (pas 500). Inject level=1 xp=699, +50 → 749 < 750 → pas de level-up */
    tama_engine_test_set_stats(800, 800, 800, 1, 699);
    for (int i = 0; i < 1000; i++)
        tama_engine_keypress(0);
    TEST_ASSERT_EQ(tama_engine_get_stats()->level, 1,
                   "xp=699+50=749 < seuil 750 → pas de level-up niveau 1 (scaling +250)");

    /* Level 1→2 : inject xp=749, +50 → 799 ≥ 750 → level-up, xp=49 */
    tama_engine_test_set_stats(800, 800, 800, 1, 749);
    for (int i = 0; i < 1000; i++)
        tama_engine_keypress(0);
    s = tama_engine_get_stats();
    TEST_ASSERT_EQ(s->level, 2, "xp=749+50=799 ≥ 750 → level 1→2 (seuil +250 confirmé)");
    TEST_ASSERT_EQ(s->xp,   49, "XP restant = 799 - 750 = 49");
}

/* ── Level-up multi-niveaux (while-loop) ────────────────────────────── */

/* Un seul événement XP (+50) peut franchir plusieurs seuils d'un coup.
 * La copie locale n'avait pas de while-loop — ce test le prouve. */
static void test_tama_levelup_multilevel(void)
{
    reset_tama();
    /* Seuils : lvl 0→1 = 500, lvl 1→2 = 750. Total = 1250.
     * Inject xp=1249, +50 → xp=1299.
     * while: 1299 ≥ 500 → lvl=1, xp=799. 799 ≥ 750 → lvl=2, xp=49. 49 < 1000 → stop. */
    tama_engine_test_set_stats(800, 800, 800, 0, 1249);
    for (int i = 0; i < 1000; i++)
        tama_engine_keypress(0);
    const tama2_stats_t *s = tama_engine_get_stats();
    TEST_ASSERT_EQ(s->level, 2,
                   "Level-up while-loop : 1299 XP franchit 2 seuils → level=2");
    TEST_ASSERT_EQ(s->xp,   49,
                   "XP restant après double level-up : 1299 - 500 - 750 = 49");
}

/* ── Cap MAX_LEVEL ──────────────────────────────────────────────────── */

static void test_tama_max_level_cap(void)
{
    reset_tama();
    /* Au niveau MAX_LEVEL=19, plus de level-up même avec un XP excédentaire */
    tama_engine_test_set_stats(800, 800, 800, 19, 9999);
    for (int i = 0; i < 1000; i++)
        tama_engine_keypress(0);
    TEST_ASSERT_EQ(tama_engine_get_stats()->level, 19,
                   "MAX_LEVEL : pas de level-up au-delà de 19");
}

/* ── Critter index ──────────────────────────────────────────────────── */

/* Clamp critter à 19 pour level > MAX_LEVEL — absent de l'ancienne copie. */
static void test_tama_critter_clamp_at_max(void)
{
    reset_tama();
    /* Injection directe level=21 > MAX_LEVEL=19 → critter doit être 19 */
    tama_engine_test_set_stats(800, 800, 800, 21, 0);
    TEST_ASSERT_EQ(tama_engine_get_critter(), 19,
                   "level > MAX_LEVEL → critter clamped à 19");
}

static void test_tama_critter_at_max_level(void)
{
    reset_tama();
    tama_engine_test_set_stats(800, 800, 800, 19, 0);
    TEST_ASSERT_EQ(tama_engine_get_critter(), 19,
                   "level = MAX_LEVEL=19 → critter = 19");
}

static void test_tama_critter_matches_level(void)
{
    for (uint16_t level = 0; level <= 19; level++) {
        reset_tama();
        tama_engine_test_set_stats(800, 800, 800, level, 0);
        TEST_ASSERT_EQ((int)tama_engine_get_critter(), (int)level,
                       "Critter index = level pour levels 0-19");
    }
}

/* ── Suite runner ───────────────────────────────────────────────────── */

void test_tama_engine(void)
{
    TEST_SUITE("Tama Engine (vrai module)");
    TEST_RUN(test_tama_clamp_max);
    TEST_RUN(test_tama_health_average);
    TEST_RUN(test_tama_health_zero);
    TEST_RUN(test_tama_state_sick);
    TEST_RUN(test_tama_state_sad);
    TEST_RUN(test_tama_state_sleeping);
    TEST_RUN(test_tama_state_sleepy);
    TEST_RUN(test_tama_state_excited);
    TEST_RUN(test_tama_state_happy);
    TEST_RUN(test_tama_state_idle);
    TEST_RUN(test_tama_hysteresis_not_before_hold_min);
    TEST_RUN(test_tama_hysteresis_changes_at_hold_min);
    TEST_RUN(test_tama_sick_overrides_excited);
    TEST_RUN(test_tama_feed);
    TEST_RUN(test_tama_feed_clamp);
    TEST_RUN(test_tama_play);
    TEST_RUN(test_tama_sleep_action);
    TEST_RUN(test_tama_xp_scaling_per_level);
    TEST_RUN(test_tama_levelup_multilevel);
    TEST_RUN(test_tama_max_level_cap);
    TEST_RUN(test_tama_critter_clamp_at_max);
    TEST_RUN(test_tama_critter_at_max_level);
    TEST_RUN(test_tama_critter_matches_level);
}
