/* main/comm/cdc/cdc_sec_cmds.h — CDC provisioning for write-only secret slots (dongle). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sec_store.h"

/* Max bytes sec_cmd_build_list() may write — the LIST response buffer size. */
#define SEC_CMD_LIST_MAX  (1 + SEC_N_SLOTS * (2 + SEC_LABEL_LEN))

bool sec_cmd_parse_set_slot(const uint8_t *p, uint16_t len,
                            uint8_t *idx, uint8_t *type,
                            char label[SEC_LABEL_LEN],
                            uint8_t secret[SEC_SECRET_MAX], uint8_t *secret_len);
/* `out` must be at least SEC_CMD_LIST_MAX bytes. Emits labels/types only — never secrets. */
void sec_cmd_build_list(uint8_t *out, uint16_t *out_len);
void cdc_sec_cmds_init(void);
