/* main/security/sec_store.h — write-only secret slots (dongle). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SEC_N_SLOTS     4
#define SEC_LABEL_LEN   16
#define SEC_SECRET_MAX  64

enum { SEC_SLOT_EMPTY = 0, SEC_SLOT_HMAC_SHA1 = 1 };

typedef struct {
    uint8_t type;
    uint8_t flags;            /* bit0 = require_keypress (always 1 in Phase 1) */
    uint8_t secret_len;
    uint8_t reserved;
    char    label[SEC_LABEL_LEN];
    uint8_t secret[SEC_SECRET_MAX];
} sec_slot_t;

void    sec_store_init(void);
bool    sec_store_set_slot(uint8_t idx, uint8_t type, const char *label,
                           const uint8_t *secret, uint8_t secret_len);
bool    sec_store_clear_slot(uint8_t idx);
uint8_t sec_store_count(void);
uint8_t sec_store_type(uint8_t idx);
const char *sec_store_label(uint8_t idx);
/* INTERNAL — firmware-only, never exposed over CDC.
 * `out` must be at least SEC_SECRET_MAX bytes. */
bool    sec_store_get_secret(uint8_t idx, uint8_t *out, uint8_t *out_len);
