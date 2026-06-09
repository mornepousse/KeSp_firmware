#pragma once
#include <stdint.h>
#include <stdbool.h>
/* HMAC-SHA1(key, msg) -> out20 (20 bytes). Returns true on success. */
bool cr_hmac_sha1(const uint8_t *key, uint16_t key_len,
                  const uint8_t *msg, uint16_t msg_len, uint8_t out20[20]);
