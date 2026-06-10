/* main/security/openpgp_crypto.c */
#include "openpgp_crypto.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include <string.h>

static const char *TAG = "pgp_crypto";

/* mbedtls RNG callback backed by the HW TRNG. The dongle keeps WiFi enabled
 * (ESP-NOW), so esp_fill_random() draws from the hardware entropy source. */
static int rng_cb(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

bool openpgp_crypto_p256_sign(const uint8_t d[32],
                              const uint8_t *hash, uint16_t hash_len,
                              uint8_t sig[64])
{
    if (!d || !hash || !sig || hash_len < 20 || hash_len > 64)
        return false;

    mbedtls_ecp_group grp;
    mbedtls_mpi r, s, dd;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&s); mbedtls_mpi_init(&dd);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1)) break;
        if (mbedtls_mpi_read_binary(&dd, d, 32)) break;
        /* reject d == 0 or d >= n (invalid scalar) */
        if (mbedtls_mpi_cmp_int(&dd, 0) <= 0 ||
            mbedtls_mpi_cmp_mpi(&dd, &grp.N) >= 0) break;
        if (mbedtls_ecdsa_sign(&grp, &r, &s, &dd, hash, hash_len,
                               rng_cb, NULL)) break;
        if (mbedtls_mpi_write_binary(&r, sig, 32)) break;
        if (mbedtls_mpi_write_binary(&s, sig + 32, 32)) break;
        ok = true;
    } while (0);

    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s);
    mbedtls_mpi_lset(&dd, 0);  /* scrub the scalar copy before free */
    mbedtls_mpi_free(&dd);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool openpgp_crypto_p256_pubkey(const uint8_t d[32], uint8_t out_pub[65])
{
    if (!d || !out_pub) return false;

    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi dd;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&dd);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1)) break;
        if (mbedtls_mpi_read_binary(&dd, d, 32)) break;
        /* reject d == 0 or d >= n (invalid scalar) */
        if (mbedtls_mpi_cmp_int(&dd, 0) <= 0 ||
            mbedtls_mpi_cmp_mpi(&dd, &grp.N) >= 0) break;
        if (mbedtls_ecp_mul(&grp, &Q, &dd, &grp.G, rng_cb, NULL)) break;
        size_t olen = 0;
        if (mbedtls_ecp_point_write_binary(&grp, &Q,
                                           MBEDTLS_ECP_PF_UNCOMPRESSED,
                                           &olen, out_pub, 65)) break;
        if (olen != 65) break;
        ok = true;
    } while (0);

    mbedtls_mpi_lset(&dd, 0);  /* scrub the scalar copy before free */
    mbedtls_mpi_free(&dd);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool openpgp_crypto_selftest(void)
{
    /* RFC 6979 A.2.5 P-256 test key; hash = SHA-256("sample") */
    static const uint8_t d[32] = {
        0xC9,0xAF,0xA9,0xD8,0x45,0xBA,0x75,0x16,0x6B,0x5C,0x21,0x57,0x67,0xB1,0xD6,0x93,
        0x4E,0x50,0xC3,0xDB,0x36,0xE8,0x9B,0x12,0x7B,0x8A,0x62,0x2B,0x12,0x0F,0x67,0x21,
    };
    static const uint8_t h[32] = {
        0xAF,0x2B,0xDB,0xE1,0xAA,0x9B,0x6E,0xC1,0xE2,0xAD,0xE1,0xD6,0x94,0xF4,0x1F,0xC7,
        0x1A,0x83,0x1D,0x02,0x68,0xE9,0x89,0x15,0x62,0x11,0x3D,0x8A,0x62,0xAD,0xD1,0xBF,
    };
    uint8_t sig[64];
    if (!openpgp_crypto_p256_sign(d, h, 32, sig)) {
        ESP_LOGE(TAG, "selftest: sign failed");
        return false;
    }
    mbedtls_ecp_group grp; mbedtls_ecp_point Q;
    mbedtls_mpi r, s, dd;
    mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&s); mbedtls_mpi_init(&dd);
    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1)) break;
        if (mbedtls_mpi_read_binary(&dd, d, 32)) break;
        if (mbedtls_ecp_mul(&grp, &Q, &dd, &grp.G, rng_cb, NULL)) break;
        if (mbedtls_mpi_read_binary(&r, sig, 32)) break;
        if (mbedtls_mpi_read_binary(&s, sig + 32, 32)) break;
        ok = (mbedtls_ecdsa_verify(&grp, h, 32, &Q, &r, &s) == 0);
    } while (0);
    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s); mbedtls_mpi_free(&dd);
    mbedtls_ecp_point_free(&Q); mbedtls_ecp_group_free(&grp);
    if (!ok) {
        ESP_LOGE(TAG, "selftest: sign KAT failed");
        return false;
    }

    /* KAT 2: pubkey derivation — Q = d·G for the same RFC 6979 A.2.5 scalar.
     * Known public key coordinates (NIST / RFC 6979 A.2.5):
     *   Ux = 60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6
     *   Uy = 7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299 */
    static const uint8_t Ux[32] = {
        0x60,0xFE,0xD4,0xBA,0x25,0x5A,0x9D,0x31,0xC9,0x61,0xEB,0x74,0xC6,0x35,0x6D,0x68,
        0xC0,0x49,0xB8,0x92,0x3B,0x61,0xFA,0x6C,0xE6,0x69,0x62,0x2E,0x60,0xF2,0x9F,0xB6,
    };
    static const uint8_t Uy[32] = {
        0x79,0x03,0xFE,0x10,0x08,0xB8,0xBC,0x99,0xA4,0x1A,0xE9,0xE9,0x56,0x28,0xBC,0x64,
        0xF2,0xF1,0xB2,0x0C,0x2D,0x7E,0x9F,0x51,0x77,0xA3,0xC2,0x94,0xD4,0x46,0x22,0x99,
    };
    uint8_t pub[65];
    bool pub_ok = openpgp_crypto_p256_pubkey(d, pub) &&
                  pub[0] == 0x04 &&
                  memcmp(pub + 1,  Ux, 32) == 0 &&
                  memcmp(pub + 33, Uy, 32) == 0;
    if (!pub_ok) {
        ESP_LOGE(TAG, "selftest: pubkey KAT failed");
        return false;
    }

    ESP_LOGI(TAG, "selftest: PASS");
    return true;
}
