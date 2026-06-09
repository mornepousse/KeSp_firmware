/* main/security/cr_hmac.c — HMAC-SHA1 wrapper (dongle, target only).
 * Uses mbedtls which is bundled with ESP-IDF; no extra dependency required. */
#include "cr_hmac.h"
#include <stdbool.h>
#include "mbedtls/md.h"

bool cr_hmac_sha1(const uint8_t *key, uint16_t key_len,
                  const uint8_t *msg, uint16_t msg_len, uint8_t out20[20])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!info) return false;
    return mbedtls_md_hmac(info, key, key_len, msg, msg_len, out20) == 0;
}
