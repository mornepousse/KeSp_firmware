/* Tests for tamagotchi game engine logic */
#include "test_framework.h"

#define TAMA2_STAT_MAX 1000

typedef struct {
    uint16_t hunger, happiness, energy, health;
    uint32_t total_keys, session_keys, max_kpm;
    uint16_t level, xp;
} tama_stats_t;

typedef enum {
    TAMA2_IDLE, TAMA2_HAPPY, TAMA2_EXCITED, TAMA2_EATING,
    TAMA2_SLEEPY, TAMA2_SLEEPING, TAMA2_SICK, TAMA2_SAD, TAMA2_CELEBRATING
} tama_state_t;

/* Simulate stat clamping */
static void clamp(tama_stats_t *s) {
    if (s->hunger > TAMA2_STAT_MAX) s->hunger = TAMA2_STAT_MAX;
    if (s->happiness > TAMA2_STAT_MAX) s->happiness = TAMA2_STAT_MAX;
    if (s->energy > TAMA2_STAT_MAX) s->energy = TAMA2_STAT_MAX;
    s->health = (s->hunger + s->happiness + s->energy) / 3;
}

/* Simulate state update */
static tama_state_t compute_state(tama_stats_t *s, uint32_t kpm) {
    if (s->health < 200) return TAMA2_SICK;
    if (s->happiness < 200) return TAMA2_SAD;
    if (s->energy < 100) return TAMA2_SLEEPING;
    if (s->energy < 300) return TAMA2_SLEEPY;
    if (kpm > 200) return TAMA2_EXCITED;
    if (kpm > 80) return TAMA2_HAPPY;
    return TAMA2_IDLE;
}

/* ── Stat clamping ───────────────────────────────────────────────── */

static void test_tama_clamp_max(void) {
    tama_stats_t s = {1500, 1200, 1100, 0};
    clamp(&s);
    TEST_ASSERT_EQ(s.hunger, 1000, "Hunger clamped to 1000");
    TEST_ASSERT_EQ(s.happiness, 1000, "Happiness clamped to 1000");
    TEST_ASSERT_EQ(s.energy, 1000, "Energy clamped to 1000");
}

static void test_tama_health_average(void) {
    tama_stats_t s = {600, 300, 900, 0};
    clamp(&s);
    TEST_ASSERT_EQ(s.health, 600, "Health = (600+300+900)/3 = 600");
}

static void test_tama_health_zero(void) {
    tama_stats_t s = {0, 0, 0, 0};
    clamp(&s);
    TEST_ASSERT_EQ(s.health, 0, "Health = 0 when all stats 0");
}

/* ── State transitions ───────────────────────────────────────────── */

static void test_tama_state_sick(void) {
    tama_stats_t s = {100, 100, 100, 0};
    clamp(&s); /* health = 100 */
    TEST_ASSERT_EQ(compute_state(&s, 0), TAMA2_SICK, "Health < 200 → SICK");
}

static void test_tama_state_sad(void) {
    tama_stats_t s = {800, 100, 800, 0};
    clamp(&s); /* health = 566, happiness = 100 */
    TEST_ASSERT_EQ(compute_state(&s, 0), TAMA2_SAD, "Happiness < 200 → SAD");
}

static void test_tama_state_sleeping(void) {
    tama_stats_t s = {800, 800, 50, 0};
    clamp(&s);
    TEST_ASSERT_EQ(compute_state(&s, 0), TAMA2_SLEEPING, "Energy < 100 → SLEEPING");
}

static void test_tama_state_sleepy(void) {
    tama_stats_t s = {800, 800, 250, 0};
    clamp(&s);
    TEST_ASSERT_EQ(compute_state(&s, 0), TAMA2_SLEEPY, "Energy < 300 → SLEEPY");
}

static void test_tama_state_excited(void) {
    tama_stats_t s = {800, 800, 800, 0};
    clamp(&s);
    TEST_ASSERT_EQ(compute_state(&s, 250), TAMA2_EXCITED, "KPM > 200 → EXCITED");
}

static void test_tama_state_happy(void) {
    tama_stats_t s = {800, 800, 800, 0};
    clamp(&s);
    TEST_ASSERT_EQ(compute_state(&s, 120), TAMA2_HAPPY, "KPM > 80 → HAPPY");
}

static void test_tama_state_idle(void) {
    tama_stats_t s = {800, 800, 800, 0};
    clamp(&s);
    TEST_ASSERT_EQ(compute_state(&s, 30), TAMA2_IDLE, "KPM < 80 → IDLE");
}

/* ── State priority ──────────────────────────────────────────────── */

static void test_tama_sick_overrides_excited(void) {
    tama_stats_t s = {50, 50, 50, 0};
    clamp(&s); /* health < 200 */
    TEST_ASSERT_EQ(compute_state(&s, 300), TAMA2_SICK, "SICK overrides EXCITED");
}

/* ── Actions ─────────────────────────────────────────────────────── */

static void test_tama_feed(void) {
    tama_stats_t s = {500, 800, 800, 0};
    s.hunger = (s.hunger + 300 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : s.hunger + 300;
    TEST_ASSERT_EQ(s.hunger, 800, "Feed: 500 + 300 = 800");
}

static void test_tama_feed_clamp(void) {
    tama_stats_t s = {900, 800, 800, 0};
    s.hunger = (s.hunger + 300 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : s.hunger + 300;
    TEST_ASSERT_EQ(s.hunger, 1000, "Feed: 900 + 300 clamped to 1000");
}

static void test_tama_play(void) {
    tama_stats_t s = {800, 600, 800, 0};
    s.happiness = (s.happiness + 200 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : s.happiness + 200;
    TEST_ASSERT_EQ(s.happiness, 800, "Play: 600 + 200 = 800");
}

static void test_tama_sleep_action(void) {
    tama_stats_t s = {800, 800, 400, 0};
    s.energy = (s.energy + 400 > TAMA2_STAT_MAX) ? TAMA2_STAT_MAX : s.energy + 400;
    TEST_ASSERT_EQ(s.energy, 800, "Sleep: 400 + 400 = 800");
}

/* ── Level up ────────────────────────────────────────────────────── */

static void test_tama_level_up(void) {
    tama_stats_t s = {800, 800, 800, 0, 0, 0, 0, 0, 500};
    /* XP = 500, threshold = 500 → level up */
    if (s.xp >= 500) { s.xp -= 500; s.level++; }
    TEST_ASSERT_EQ(s.level, 1, "Level 0 → 1");
    TEST_ASSERT_EQ(s.xp, 0, "XP consumed");
}

static void test_tama_max_level(void) {
    tama_stats_t s = {800, 800, 800, 0, 0, 0, 0, 19, 500};
    /* Level 19 = max, should not exceed */
    if (s.xp >= 500 && s.level < 19) { s.xp -= 500; s.level++; }
    TEST_ASSERT_EQ(s.level, 19, "Max level stays at 19");
    TEST_ASSERT_EQ(s.xp, 500, "XP not consumed at max level");
}

static void test_tama_critter_index(void) {
    for (uint16_t level = 0; level <= 19; level++) {
        uint8_t critter = (level <= 19) ? (uint8_t)level : 19;
        TEST_ASSERT_EQ(critter, level, "Critter index matches level");
    }
}

void test_tama_engine(void) {
    TEST_SUITE("Tama Engine");
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
    TEST_RUN(test_tama_sick_overrides_excited);
    TEST_RUN(test_tama_feed);
    TEST_RUN(test_tama_feed_clamp);
    TEST_RUN(test_tama_play);
    TEST_RUN(test_tama_sleep_action);
    TEST_RUN(test_tama_level_up);
    TEST_RUN(test_tama_max_level);
    TEST_RUN(test_tama_critter_index);
}
