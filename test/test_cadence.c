/* Cadences des moitiés : au repos, aucune attente périodique sous 3 ticks
 * (CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP) — sinon le light sleep automatique
 * n'arrive jamais, sans erreur. Les _Static_assert de cadence.h sont la vraie
 * garde ; ce test dit POURQUOI et vérifie les valeurs actives. */
#include "test_framework.h"
#include "../main/power/cadence.h"

static void test_le_repos_laisse_trois_ticks(void)
{
    TEST_ASSERT_EQ(CADENCE_REPOS_MIN_MS, 3 * CADENCE_TICK_MS, "3 ticks a 100 Hz");
    TEST_ASSERT(KBD_CADENCE_REPOS_MS >= CADENCE_REPOS_MIN_MS, "tache clavier");
    TEST_ASSERT(KBD_RELAY_REPOS_MS   >= CADENCE_REPOS_MIN_MS, "relais gauche");
    TEST_ASSERT(HALF_TX_REPOS_MS     >= CADENCE_REPOS_MIN_MS, "rafraichissement droite");
    TEST_ASSERT(LINK_REPOS_MS        >= CADENCE_REPOS_MIN_MS, "lien TRRS");
    TEST_ASSERT(MEMLCD_DROITE_PERIODE_MS >= CADENCE_REPOS_MIN_MS, "ecran droite");
    TEST_ASSERT(LVGL_TICK_MS         >= CADENCE_REPOS_MIN_MS, "tick LVGL");
    TEST_ASSERT(LVGL_REFR_MS         >= CADENCE_REPOS_MIN_MS, "rafraichissement LVGL");
}

static void test_l_actif_reste_reactif(void)
{
    /* Touche tenue / réparation : sous 20 ms, sinon les réaffirmations à 100 ms
     * et la réparation bornée (5 × 10 ms) perdent leur sens. */
    TEST_ASSERT(KBD_CADENCE_ACTIF_MS <= 20, "tache clavier active");
    TEST_ASSERT(KBD_RELAY_REFRESH_MS <= 20, "relais actif");
    TEST_ASSERT(HALF_TX_TENU_MS      <= 20, "droite touche tenue");
    TEST_ASSERT(LINK_TICK_MS         <= 20, "lien en poignee de main");
}

void test_cadence(void)
{
    TEST_SUITE("cadences des moities (tickless)");
    TEST_RUN(test_le_repos_laisse_trois_ticks);
    TEST_RUN(test_l_actif_reste_reactif);
}
