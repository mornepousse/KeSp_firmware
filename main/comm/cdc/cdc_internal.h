/* Internal header shared between CDC source files.
   NOT part of the public API. */
#pragma once

#include "cdc_acm_com.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_log.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* CDC interface (0 by default, overridable per board) */
#ifndef CDC_ITF
#define CDC_ITF TINYUSB_CDC_ACM_0
#endif

/* Shared log tag */
extern const char *TAG_CDC;

/* ── OTA state (defined in cdc_ota.c) ────────────────────────────── */
typedef enum { OTA_IDLE, OTA_RECEIVING } ota_state_t;

extern volatile ota_state_t ota_state;
extern volatile bool ota_binary_mode; /* true = OTA via KS frames, false = legacy raw */
extern uint8_t ota_buf[];
extern volatile size_t ota_buf_pos;
extern size_t ota_total_size;
extern volatile size_t ota_received;
extern volatile bool ota_chunk_ready;
extern volatile uint32_t ota_last_activity_ms;

void cmd_ota_start(const char *arg);

/* ── Binary protocol (defined in cdc_binary_protocol.c) ──────────── */
#include "cdc_binary_protocol.h"
void ota_abort(const char *reason);
void ota_process_chunk(void);
void ota_receive_bytes(const char *data, uint16_t len);

/* Binary OTA helpers (called from cdc_binary_cmds.c) */
esp_err_t ota_bin_begin(uint32_t size);
esp_err_t ota_bin_write(const uint8_t *data, uint16_t len);
esp_err_t ota_bin_finish(void);
void ota_bin_abort(void);
