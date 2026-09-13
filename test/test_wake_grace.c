/* Grâce laissée au pilote de matrice après le réveil — logique pure.
 *
 * La réconciliation du réveil conclut « touche relâchée avant le premier
 * balayage » si le pilote recréé n'a rien signalé dans la grâce. Le pilote ne
 * signale qu'après son anti-rebond (debounce_ticks × interval). Une grâce plus
 * courte que l'anti-rebond relâche à tort une touche TENUE (Super+F perdu au
 * banc, 2026-09-13). La formule doit donc TOUJOURS couvrir l'anti-rebond, ne
 * jamais descendre sous l'ancienne valeur (10 ms), et rester bornée (un pilote
 * muet ne doit pas retarder le réveil indéfiniment).
 */
#include "test_framework.h"
#include "../main/input/wake_grace.h"

static void test_couvre_l_anti_rebond(void)
{
    /* Niphargus : 3 balayages × 1 ms. Anti-rebond + 2 balayages = 5 ms, + marge. */
    uint32_t g = wake_grace_ms(3, 1000);
    TEST_ASSERT(g >= 5, "couvre 3 balayages + 2 de démarrage à 1 ms");
    TEST_ASSERT_EQ(g, 15, "Niphargus : 5 ms d'anti-rebond + 10 ms de marge");
    /* KaSe V1 : 5 balayages × 1 ms. */
    TEST_ASSERT(wake_grace_ms(5, 1000) >= 7, "couvre 5 balayages + 2");
    /* Un anti-rebond plus long ne peut JAMAIS donner une grâce plus courte. */
    for (uint32_t d = 0; d < 20; d++)
        TEST_ASSERT(wake_grace_ms(d + 1, 1000) >= wake_grace_ms(d, 1000),
                    "monotone en debounce_ticks");
}

static void test_plancher_10_ms(void)
{
    /* Jamais moins que l'ancienne valeur, même pour un pilote instantané. */
    TEST_ASSERT_EQ(wake_grace_ms(0, 0), 10, "plancher 10 ms");
    TEST_ASSERT(wake_grace_ms(1, 100) >= 10, "≥ 10 ms pour 100 µs × 3");
}

static void test_plafond_50_ms(void)
{
    /* Un pilote muet ou une config absurde ne bloque pas le réveil. */
    TEST_ASSERT_EQ(wake_grace_ms(40, 1000), 50, "plafond 50 ms");
    TEST_ASSERT_EQ(wake_grace_ms(3, 100000), 50, "plafond même avec un intervalle énorme");
}

void test_wake_grace(void)
{
    TEST_SUITE("Grâce du pilote au réveil");
    test_couvre_l_anti_rebond();
    test_plancher_10_ms();
    test_plafond_50_ms();
}
