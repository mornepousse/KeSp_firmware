/* Choice of sleep level (pure logic) — brick B7.
 *
 * Two tiers, and each pays its way:
 *
 *   LIGHT  — CPU in light sleep, state kept, wakeup in ~1 ms on the local
 *             matrix. 240 uA (ESP32-S3 datasheet v2.2, table 5-10, p. 68) plus
 *             4 uA for the HT7833. On a ~650 mAh 16340, four hours at
 *             this rate cost 1 mAh: the tier is almost free, and that's
 *             what makes it possible to hold it for hours rather than minutes.
 *
 *   DEEP — deep sleep, 8 uA, wakeup via EXT1 on the lines. RAM is
 *             lost: waking up is a full restart, measured at 683 ms
 *             on the bench. So it's only reached after hours of absence, where
 *             this delay goes unnoticed.
 *
 * The ULP is excluded: 170 uA (same table), more than three times the target of
 * 50 uA by itself alone. The "RTC scan" announced in the design cannot hold.
 *
 * ⚠ The radio is off from the LIGHT tier onward — listening costs 13.1 mA
 * (nRF24L01+ PS v1.0, table 4, p. 14), two orders of magnitude above
 * any sleep budget. A sleeping half therefore does NOT hear the other: the
 * wakeup happens on its own matrix. This is a constraint of the chip, not
 * an implementation choice.
 */
#include "test_framework.h"
#include "../main/power/veille.h"

#define LEGERE   60000u        /* 1 min */
#define PROFONDE 14400000u     /* 4 h  */

static void test_activite_recente_ne_dort_pas(void)
{
    TEST_ASSERT(veille_niveau(0, false, LEGERE, PROFONDE) == VEILLE_AUCUNE,
                "just typed");
    TEST_ASSERT(veille_niveau(LEGERE - 1, false, LEGERE, PROFONDE) == VEILLE_AUCUNE,
                "just before the threshold: still awake");
}

static void test_etage_leger(void)
{
    TEST_ASSERT(veille_niveau(LEGERE, false, LEGERE, PROFONDE) == VEILLE_LEGERE,
                "at threshold: light sleep");
    TEST_ASSERT(veille_niveau(PROFONDE - 1, false, LEGERE, PROFONDE) == VEILLE_LEGERE,
                "up to the last millisecond before deep sleep");
}

static void test_etage_profond(void)
{
    TEST_ASSERT(veille_niveau(PROFONDE, false, LEGERE, PROFONDE) == VEILLE_PROFONDE,
                "at threshold: deep sleep");
    TEST_ASSERT(veille_niveau(0xFFFFFFFFu, false, LEGERE, PROFONDE) == VEILLE_PROFONDE,
                "and well beyond");
}

static void test_le_blocage_prime_sur_tout(void)
{
    /* THE test of this file. A lock — USB plugged in, key held, update
     * in progress — must forbid ALL sleep, even after hours. Getting
     * this wrong puts a keyboard to sleep while it's in active use. */
    TEST_ASSERT(veille_niveau(LEGERE, true, LEGERE, PROFONDE) == VEILLE_AUCUNE,
                "blocked: no light sleep");
    TEST_ASSERT(veille_niveau(PROFONDE * 2, true, LEGERE, PROFONDE) == VEILLE_AUCUNE,
                "blocked: no deep sleep either, even after eight hours");
}

static void test_les_seuils_sont_ordonnes(void)
{
    /* A deep threshold shorter than the light one would make the light tier
     * unreachable, and the keyboard would jump straight to the 683 ms restart. */
    TEST_ASSERT(VEILLE_LEGERE_MS < VEILLE_PROFONDE_MS,
                "the light threshold comes before the deep one");
    TEST_ASSERT(VEILLE_PROFONDE_MS >= 3600000u,
                "deep sleep doesn't arrive before an hour of absence");
    /* Awake and idle, the board draws ~28 mA (160 MHz) versus 0.24 mA
     * asleep: every second of waiting is worth a hundred seconds of sleep. At
     * 60 s, a day of typing used to lose ~0.2 V (2026-09-15). The light tier must
     * stay SHORT — but not zero either: a typing pause of a few
     * seconds should not put the board to sleep on every breath. */
    TEST_ASSERT(VEILLE_LEGERE_MS <= 20000u,
                "light sleep comes within 20 s at most: awake, the board costs 100 times the sleep");
    TEST_ASSERT(VEILLE_LEGERE_MS >= 5000u,
                "but not before 5 s: a breath between two words is not a pause");
}

/* Grace period after a wakeup: a GPIO wakeup is NOT activity (a glitch must
 * not buy seconds of radio time), but it's not an immediate
 * fall-back-asleep either. A key with a slow pre-contact wakes the board
 * before its capture sees it (two empty passes, bench 2026-09-16); without
 * grace, the loop re-read a stale inactivity value and sent it back to sleep in
 * ~15 ms, BEFORE the re-created driver had seen the key — lost. 300 ms of wakefulness
 * is enough for the driver (3 ms debounce) and costs ~2 uAh per glitch. */
static void test_grace_apres_reveil(void)
{
    TEST_ASSERT(veille_en_grace(1000, 1000, VEILLE_GRACE_REVEIL_MS), "at the moment of wakeup: in grace");
    TEST_ASSERT(veille_en_grace(1299, 1000, VEILLE_GRACE_REVEIL_MS), "299 ms after: still in grace");
    TEST_ASSERT(!veille_en_grace(1300, 1000, VEILLE_GRACE_REVEIL_MS), "300 ms after: grace is over");
    TEST_ASSERT(!veille_en_grace(5000, 0, VEILLE_GRACE_REVEIL_MS), "never woken (0): no grace");
    TEST_ASSERT(veille_en_grace(50, 0xFFFFFFF0u, VEILLE_GRACE_REVEIL_MS), "counter overflow (wakeup 16 ms before wraparound, now = 50): 66 ms elapsed, in grace");
    TEST_ASSERT(VEILLE_GRACE_REVEIL_MS >= 100 && VEILLE_GRACE_REVEIL_MS <= 1000,
                "between 100 ms (driver + long bounce) and 1 s (a glitch must not cost more)");
}

void test_veille(void)
{
    printf("\n-- sleep level choice (B7) --\n");
    test_activite_recente_ne_dort_pas();
    test_etage_leger();
    test_etage_profond();
    test_le_blocage_prime_sur_tout();
    test_les_seuils_sont_ordonnes();
    test_grace_apres_reveil();
}
