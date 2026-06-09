#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sec_store.h"

bool sec_cmd_parse_set_slot(const uint8_t *p, uint16_t len,
                            uint8_t *idx, uint8_t *type,
                            char label[SEC_LABEL_LEN],
                            uint8_t secret[SEC_SECRET_MAX], uint8_t *secret_len);
void sec_cmd_build_list(uint8_t *out, uint16_t *out_len);
void cdc_sec_cmds_init(void);
