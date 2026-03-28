/* CDC ACM command framework — generic, reusable in any ESP32 TinyUSB project.
   Provides: FIFO line assembly, pluggable command dispatch, send helpers. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Configuration ───────────────────────────────────────────────── */

/* Line buffer size (text command) */
#define CDC_CMD_MAX_LEN 1024
/* Number of queued lines */
#define CDC_CMD_FIFO_DEPTH 4
/* OTA over CDC */
#define OTA_CHUNK_SIZE 4096
#define OTA_TIMEOUT_MS 30000

/* ── Command FIFO ────────────────────────────────────────────────── */

typedef struct {
	char line[CDC_CMD_MAX_LEN];
	uint16_t len;
} cdc_cmd_t;

void cdc_cmd_fifo_init(void);
bool cdc_cmd_push(const char *line, uint16_t len);
bool cdc_cmd_pop(cdc_cmd_t *out);
bool cdc_cmd_peek(cdc_cmd_t *out);
uint16_t cdc_cmd_count(void);

/* ── Pluggable command dispatch ──────────────────────────────────── */

/* Handler signature: receives the argument part after the prefix */
typedef void (*cdc_cmd_handler_t)(const char *arg);

typedef struct {
	const char     *prefix;      /* command prefix to match (case-insensitive) */
	uint8_t         prefix_len;  /* strlen(prefix) */
	bool            has_arg;     /* true: pass remainder after prefix to handler */
	cdc_cmd_handler_t handler;
} cdc_cmd_entry_t;

/* Register a command table. Can be called multiple times to add tables
   from different modules. Tables are searched in registration order. */
#define CDC_MAX_CMD_TABLES 4
void cdc_register_commands(const cdc_cmd_entry_t *table, size_t count);

/* ── Send helpers ────────────────────────────────────────────────── */

void cdc_send_line(const char *text);
void cdc_send_binary(const uint8_t *data, size_t len);
void cdc_send_large(const char *data, size_t len);
void trim_spaces(char *str);

/* Binary response framing: sends [C][>][type][len_lo][len_hi] header */
void start_command_queue(unsigned char type, size_t total_size);

/* ── USB callbacks ───────────────────────────────────────────────── */

/* Called by TinyUSB ISR/callback with arbitrary chunks */
void receive_data(const char *data, uint16_t len);

/* Processing task — create from main with xTaskCreate */
void cdc_process_commands_task(void *arg);

/* Initialize FIFO + start processing task */
void init_cdc_commands(void);

/* ── Convenience (uses binary framing) ───────────────────────────── */

void cdc_send_layer(uint8_t layer);
void cdc_ping(void);
