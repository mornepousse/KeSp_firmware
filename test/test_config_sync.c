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

/* État de cohérence côté dongle : il retient la dernière empreinte annoncée par
 * la gauche et ne signale qu'au CHANGEMENT — journaliser à chaque trame (1/s)
 * noierait la console et l'utilisateur. Même discipline que « émettre sur
 * changement » du lien radio. */
static void test_coherence_once_par_changement(void)
{
    config_coherence_t c = {0};
    TEST_ASSERT(config_coherence_note(&c, 0xAAAAAAAAu, 100), "1re annonce = changement");
    TEST_ASSERT_EQ(c.left_fp, 0xAAAAAAAAu, "empreinte mémorisée");
    TEST_ASSERT_EQ(c.left_ms, 100u, "horodatage mémorisé");
    TEST_ASSERT(!config_coherence_note(&c, 0xAAAAAAAAu, 200), "même empreinte = pas un changement");
    TEST_ASSERT_EQ(c.left_ms, 200u, "horodatage rafraîchi même sans changement");
    TEST_ASSERT(config_coherence_note(&c, 0xBBBBBBBBu, 300), "empreinte différente = changement");
    TEST_ASSERT(!config_coherence_note(&c, 0xBBBBBBBBu, 400), "puis stable = pas de changement");
}

/* Le drapeau « déjà vue » est nécessaire : la toute première annonce est un
 * changement même si l'empreinte vaut 0 (impossible à distinguer de l'init sans
 * ce drapeau). Sans lui, une gauche qui annoncerait 0 au premier contact
 * passerait inaperçue. */
static void test_coherence_premiere_annonce_meme_a_zero(void)
{
    config_coherence_t c = {0};
    TEST_ASSERT(config_coherence_note(&c, 0u, 50), "1re annonce à 0 = changement (jamais vue)");
    TEST_ASSERT(!config_coherence_note(&c, 0u, 60), "0 de nouveau = pas un changement");
}

void test_config_sync(void)
{
    TEST_SUITE("Cohérence de config (empreinte)");
    test_fp_stable_et_sensible();
    test_fp_vecteur_connu();
    test_fp_match();
    test_coherence_once_par_changement();
    test_coherence_premiere_annonce_meme_a_zero();
}
