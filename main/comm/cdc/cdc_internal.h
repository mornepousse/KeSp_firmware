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
#include "board.h"

/* CDC interface (0 by default) */
#ifndef CDC_ITF
#define CDC_ITF TINYUSB_CDC_ACM_0
#endif

/* Shared log tag */
extern const char *TAG_CDC;

/* ── Send helpers (defined in cdc_acm_com.c) ─────────────────────── */
void cdc_send_line(const char *text);
void cdc_send_binary(const uint8_t *data, size_t len);
void cdc_send_large(const char *data, size_t len);
void trim_spaces(char *str);

/* Binary response framing */
void start_command_queue(unsigned char type, size_t total_size);

/* Binary response type IDs */
enum cdc_response_type
{
	CDC_RESP_NONE = 0,
	CDC_RESP_LAYER,
	CDC_RESP_CURRENT_LAYER,
	CDC_RESP_LAYER_NAME,
	CDC_RESP_MACROS,
	CDC_RESP_ALL_LAYOUT_NAMES,
	CDC_RESP_PING,
	CDC_RESP_DEBUG,
};

/* ── Command dispatcher (defined in cdc_commands.c) ──────────────── */
void parse_and_execute(const char *line);

/* ── OTA (defined in cdc_ota.c) ──────────────────────────────────── */
typedef enum { OTA_IDLE, OTA_RECEIVING } ota_state_t;

extern volatile ota_state_t ota_state;
extern uint8_t ota_buf[];
extern size_t ota_buf_pos;
extern size_t ota_total_size;
extern size_t ota_received;
extern volatile bool ota_chunk_ready;
extern uint32_t ota_last_activity_ms;

void cmd_ota_start(const char *arg);
void ota_abort(const char *reason);
void ota_process_chunk(void);     /* called from cdc_process_commands_task */
void ota_receive_bytes(const char *data, uint16_t len); /* called from receive_data */
