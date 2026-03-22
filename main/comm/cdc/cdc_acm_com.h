#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Line buffer size (text command) */
#define CDC_CMD_MAX_LEN 1024
/* Number of queued lines */
#define CDC_CMD_FIFO_DEPTH 4

typedef struct {
	char line[CDC_CMD_MAX_LEN];
	uint16_t len;
} cdc_cmd_t;

/* Command FIFO API */
void cdc_cmd_fifo_init(void);
bool cdc_cmd_push(const char *line, uint16_t len);
bool cdc_cmd_pop(cdc_cmd_t *out);
bool cdc_cmd_peek(cdc_cmd_t *out);
uint16_t cdc_cmd_count(void);

/* Called by TinyUSB ISR/callback with arbitrary chunks */
void receive_data(const char *data, uint16_t len);

/* Call periodically from a task to process queued commands */
void cdc_process_commands_task(void *arg);

void init_cdc_commands(void);

void cdc_send_layer(uint8_t layer);
void cdc_ping(void);
