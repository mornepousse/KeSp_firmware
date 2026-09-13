/* Cohérence de config entre les deux moteurs (dongle + gauche) — fusion phase 3.
 *
 * Deux moteurs ne doivent pas diverger. Plutôt qu'un numéro de version tenu à la
 * main (le design initial), on prend une EMPREINTE du blob de config : un CRC32
 * du contenu. Deux configs identiques → même empreinte ; toute différence, même
 * d'un octet, change l'empreinte. Le contrôleur lit l'empreinte des deux
 * appareils (égales = synchronisées) ; le dongle comparera plus tard l'empreinte
 * annoncée par la gauche à la sienne pour refuser de tourner sur une divergence.
 *
 * Design : docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md
 * (règle 2, affinée : empreinte de contenu au lieu d'un numéro).
 */
#include "test_framework.h"
#include "../main/input/config_sync.h"
#include <string.h>

static void test_fp_stable_et_sensible(void)
{
    uint8_t a[64];
    for (int i = 0; i < 64; i++) a[i] = (uint8_t)i;
    uint32_t fa = config_fp_crc32(a, sizeof(a));

    /* Stable : même contenu → même empreinte. */
    uint8_t b[64];
    memcpy(b, a, sizeof(b));
    TEST_ASSERT_EQ(config_fp_crc32(b, sizeof(b)), fa, "même contenu → même empreinte");

    /* Sensible : un seul octet change → empreinte différente. */
    b[30] ^= 0x01;
    TEST_ASSERT(config_fp_crc32(b, sizeof(b)) != fa, "1 octet diff → empreinte diff");

    /* Sensible à la longueur. */
    TEST_ASSERT(config_fp_crc32(a, 63) != fa, "longueur diff → empreinte diff");
}

/* Vecteur CRC-32 (IEEE, reflété, poly 0xEDB88320) connu : "123456789" = 0xCBF43926. */
static void test_fp_vecteur_connu(void)
{
    const uint8_t v[] = "123456789";
    TEST_ASSERT_EQ(config_fp_crc32(v, 9), 0xCBF43926u, "CRC-32 IEEE de 123456789");
}

static void test_fp_match(void)
{
    /* Deux empreintes égales et non nulles → cohérent. */
    TEST_ASSERT(config_fp_match(0x1234abcd, 0x1234abcd), "égales non nulles → cohérent");
    /* Différentes → incohérent. */
    TEST_ASSERT(!config_fp_match(0x1234abcd, 0x1234abce), "différentes → incohérent");
    /* Zéro = « inconnu / pas encore annoncé » → jamais cohérent, même si égal. */
    TEST_ASSERT(!config_fp_match(0, 0), "0 vs 0 → incohérent (inconnu)");
    TEST_ASSERT(!config_fp_match(0x10, 0), "x vs 0 → incohérent");
}

void test_config_sync(void)
{
    TEST_SUITE("Cohérence de config (empreinte)");
    test_fp_stable_et_sensible();
    test_fp_vecteur_connu();
    test_fp_match();
}
