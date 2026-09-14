/* Jauge batterie — logique pure (calculs et déductions).
 *
 * Matériel : VBAT_SENSE = pont 1 MΩ/1 MΩ + 100 nF → V_batt = 2 × V_adc.
 * Unité de transport : dV (42 = 4,2 V), 0 = inconnu (convention batt_dV).
 * « En charge » n'est PAS mesurable (pas de VBUS, TP4056 sans STDBY) : on
 * DÉDUIT — pleine = plateau ≥ 4,15 V tenu 2 min ; en charge probable = hausse
 * ≥ 0,1 V (une décharge ne monte jamais). Ces déductions sont ce qui protège
 * l'utilisateur d'une jauge qui ment ; elles sont testées ici.
 *
 * Design : docs/superpowers/specs/2026-09-14-batterie-jauge-design.md */
#include "test_framework.h"
#include "../main/power/batt_calc.h"

static void test_conversion_et_rejet(void)
{
    TEST_ASSERT_EQ(batt_mv_to_dv(2075), 42, "2075 mV ADC × 2 = 4,15 V → 42 dV (arrondi)");
    TEST_ASSERT_EQ(batt_mv_to_dv(1850), 37, "1850 × 2 = 3,70 V → 37");
    TEST_ASSERT_EQ(batt_mv_to_dv(1200), 0,  "2,4 V batterie : sous 2,5 V → inconnu");
    TEST_ASSERT_EQ(batt_mv_to_dv(2300), 0,  "4,6 V batterie : au-dessus de 4,5 V → inconnu");
    TEST_ASSERT_EQ(batt_mv_to_dv(0), 0,     "ADC à 0 (pont ouvert) → inconnu");
}

static void test_moyenne(void)
{
    uint32_t s[8] = { 1850, 1852, 1848, 1851, 1849, 1850, 1850, 1850 };
    TEST_ASSERT_EQ(batt_mv_from_samples(s, 8), 3700, "moyenne × 2 = 3700 mV");
    TEST_ASSERT_EQ(batt_dv_from_samples(s, 8), 37, "→ 37 dV");
    TEST_ASSERT_EQ(batt_mv_from_samples(s, 0), 0, "aucun échantillon → inconnu");
    uint32_t bad[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    TEST_ASSERT_EQ(batt_dv_from_samples(bad, 8), 0, "tout à 0 → inconnu");
}

static void test_soc(void)
{
    TEST_ASSERT_EQ(batt_soc_pct(0), 0xFF, "inconnu → 0xFF");
    TEST_ASSERT_EQ(batt_soc_pct(42), 100, "4,2 V → 100 %");
    TEST_ASSERT_EQ(batt_soc_pct(39), 70,  "3,9 V → 70 %");
    TEST_ASSERT_EQ(batt_soc_pct(37), 40,  "3,7 V → 40 %");
    TEST_ASSERT_EQ(batt_soc_pct(35), 15,  "3,5 V → 15 %");
    TEST_ASSERT_EQ(batt_soc_pct(33), 0,   "3,3 V → 0 %");
    TEST_ASSERT_EQ(batt_soc_pct(30), 0,   "sous 3,3 V : 0, jamais négatif");
    TEST_ASSERT(batt_soc_pct(38) > 40 && batt_soc_pct(38) < 70, "3,8 V interpolé entre 40 et 70");
    for (uint8_t d = 30; d < 45; d++)
        TEST_ASSERT(batt_soc_pct(d + 1) >= batt_soc_pct(d), "SoC monotone en tension");
}

static void test_pleine_apres_plateau(void)
{
    batt_state_t st = {0};
    TEST_ASSERT_EQ(batt_state_step(&st, 4160, 0), BATT_CHG_UNKNOWN, "première mesure haute : pas encore pleine");
    TEST_ASSERT_EQ(batt_state_step(&st, 4160, 60000), BATT_CHG_UNKNOWN, "60 s de plateau : pas encore");
    TEST_ASSERT_EQ(batt_state_step(&st, 4165, 121000), BATT_CHG_FULL, "≥ 120 s ≥ 4,15 V → PLEINE");
    TEST_ASSERT_EQ(batt_state_step(&st, 4120, 130000), BATT_CHG_FULL, "hystérésis : 4,12 V reste pleine");
    TEST_ASSERT_EQ(batt_state_step(&st, 4090, 140000), BATT_CHG_UNKNOWN, "sous 4,10 V : plus pleine");
}

static void test_en_charge_probable_si_ca_monte(void)
{
    batt_state_t st = {0};
    batt_state_step(&st, 3700, 0);
    TEST_ASSERT_EQ(batt_state_step(&st, 3750, 60000), BATT_CHG_UNKNOWN, "+50 mV : pas assez");
    TEST_ASSERT_EQ(batt_state_step(&st, 3810, 120000), BATT_CHG_PROBABLE, "+110 mV en 2 min → en charge probable");
    /* Une décharge ne fait jamais « monter » : inconnu. */
    batt_state_t d = {0};
    batt_state_step(&d, 3900, 0);
    TEST_ASSERT_EQ(batt_state_step(&d, 3850, 100000), BATT_CHG_UNKNOWN, "ça baisse : inconnu");
    TEST_ASSERT_EQ(batt_state_step(&d, 3800, 200000), BATT_CHG_UNKNOWN, "ça baisse encore : toujours inconnu");
}

static void test_lente_derive_ne_trompe_pas(void)
{
    /* +30 mV toutes les 5 min pendant longtemps : chaque pas est sous le seuil et
     * la fenêtre glissante repart — ce n'est PAS une charge (bruit, température). */
    batt_state_t st = {0};
    uint32_t mv = 3700;
    for (uint32_t t = 0; t <= 3000000; t += 300001) {
        mv += 30;
        TEST_ASSERT_EQ(batt_state_step(&st, mv, t), BATT_CHG_UNKNOWN, "dérive lente : jamais « en charge »");
    }
}

static void test_inconnu_reset(void)
{
    batt_state_t st = {0};
    batt_state_step(&st, 4160, 0);
    batt_state_step(&st, 4160, 130000);   /* pleine */
    TEST_ASSERT_EQ(batt_state_step(&st, 0, 140000), BATT_CHG_UNKNOWN, "mesure inconnue → état inconnu, plateau oublié");
    TEST_ASSERT_EQ(batt_state_step(&st, 4160, 150000), BATT_CHG_UNKNOWN, "il faut refaire 120 s de plateau");
}

void test_batt_calc(void)
{
    TEST_SUITE("Jauge batterie : calculs purs");
    test_conversion_et_rejet();
    test_moyenne();
    test_soc();
    test_pleine_apres_plateau();
    test_en_charge_probable_si_ca_monte();
    test_lente_derive_ne_trompe_pas();
    test_inconnu_reset();
}
