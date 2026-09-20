/* Config consistency between the two engines (dongle + left) — fusion phase 3.
 *
 * Two engines must not diverge. Rather than a version number kept by hand
 * (the initial design), we take a FINGERPRINT of the config blob: a CRC32
 * of the content. Two identical configs → same fingerprint; any difference,
 * even a single byte, changes the fingerprint. The controller reads the
 * fingerprint of both devices (equal = synchronized); the dongle will later
 * compare the fingerprint announced by the left half to its own to refuse to run on a divergence.
 *
 * Design: docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md
 * (rule 2, refined: content fingerprint instead of a number).
 */
#include "test_framework.h"
#include "../main/input/config_sync.h"
#include <string.h>

static void test_fp_stable_et_sensible(void)
{
    uint8_t a[64];
    for (int i = 0; i < 64; i++) a[i] = (uint8_t)i;
    uint32_t fa = config_fp_crc32(a, sizeof(a));

    /* Stable: same content → same fingerprint. */
    uint8_t b[64];
    memcpy(b, a, sizeof(b));
    TEST_ASSERT_EQ(config_fp_crc32(b, sizeof(b)), fa, "same content -> same fingerprint");

    /* Sensitive: a single byte changes → different fingerprint. */
    b[30] ^= 0x01;
    TEST_ASSERT(config_fp_crc32(b, sizeof(b)) != fa, "1 byte diff -> fingerprint diff");

    /* Sensitive to length. */
    TEST_ASSERT(config_fp_crc32(a, 63) != fa, "length diff -> fingerprint diff");
}

/* Known CRC-32 (IEEE, reflected, poly 0xEDB88320) vector: "123456789" = 0xCBF43926. */
static void test_fp_vecteur_connu(void)
{
    const uint8_t v[] = "123456789";
    TEST_ASSERT_EQ(config_fp_crc32(v, 9), 0xCBF43926u, "CRC-32 IEEE of 123456789");
}

static void test_fp_match(void)
{
    /* Two equal, non-zero fingerprints → consistent. */
    TEST_ASSERT(config_fp_match(0x1234abcd, 0x1234abcd), "equal non-zero -> consistent");
    /* Different → inconsistent. */
    TEST_ASSERT(!config_fp_match(0x1234abcd, 0x1234abce), "different -> inconsistent");
    /* Zero = "unknown / not yet announced" → never consistent, even if equal. */
    TEST_ASSERT(!config_fp_match(0, 0), "0 vs 0 -> inconsistent (unknown)");
    TEST_ASSERT(!config_fp_match(0x10, 0), "x vs 0 -> inconsistent");
}

/* Consistency state on the dongle side: it keeps the last fingerprint
 * announced by the left half and only reports on CHANGE — logging every
 * frame (1/s) would drown the console and the user. Same discipline as
 * "emit on change" of the radio link. */
static void test_coherence_once_par_changement(void)
{
    config_coherence_t c = {0};
    TEST_ASSERT(config_coherence_note(&c, 0xAAAAAAAAu, 100), "1st announcement = change");
    TEST_ASSERT_EQ(c.left_fp, 0xAAAAAAAAu, "fingerprint stored");
    TEST_ASSERT_EQ(c.left_ms, 100u, "timestamp stored");
    TEST_ASSERT(!config_coherence_note(&c, 0xAAAAAAAAu, 200), "same fingerprint = not a change");
    TEST_ASSERT_EQ(c.left_ms, 200u, "timestamp refreshed even without a change");
    TEST_ASSERT(config_coherence_note(&c, 0xBBBBBBBBu, 300), "different fingerprint = change");
    TEST_ASSERT(!config_coherence_note(&c, 0xBBBBBBBBu, 400), "then stable = no change");
}

/* The "already seen" flag is necessary: the very first announcement is a
 * change even if the fingerprint is 0 (impossible to distinguish from init
 * without this flag). Without it, a left half announcing 0 on first contact
 * would go unnoticed. */
static void test_coherence_premiere_annonce_meme_a_zero(void)
{
    config_coherence_t c = {0};
    TEST_ASSERT(config_coherence_note(&c, 0u, 50), "1st announcement at 0 = change (never seen)");
    TEST_ASSERT(!config_coherence_note(&c, 0u, 60), "0 again = not a change");
}

void test_config_sync(void)
{
    TEST_SUITE("Config consistency (fingerprint)");
    test_fp_stable_et_sensible();
    test_fp_vecteur_connu();
    test_fp_match();
    test_coherence_once_par_changement();
    test_coherence_premiere_annonce_meme_a_zero();
}
