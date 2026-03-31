/* CDC ACM core: FIFO, line assembly, pluggable dispatch, send helpers */
#include "cdc_internal.h"

const char *TAG_CDC = "CDC_CMD";

/* ── Pluggable command tables ────────────────────────────────────── */

static struct {
	const cdc_cmd_entry_t *table;
	size_t count;
} cmd_tables[CDC_MAX_CMD_TABLES];
static size_t cmd_tables_count = 0;

void cdc_register_commands(const cdc_cmd_entry_t *table, size_t count)
{
	if (cmd_tables_count < CDC_MAX_CMD_TABLES) {
		cmd_tables[cmd_tables_count].table = table;
		cmd_tables[cmd_tables_count].count = count;
		cmd_tables_count++;
		ESP_LOGI(TAG_CDC, "Registered %u CDC commands", (unsigned)count);
	} else {
		ESP_LOGE(TAG_CDC, "CDC command table limit reached (%d)", CDC_MAX_CMD_TABLES);
	}
}

static void parse_and_execute(const char *line)
{
	while (*line == ' ' || *line == '\t')
		line++;
	if (*line == '\0')
		return;

	for (size_t t = 0; t < cmd_tables_count; t++) {
		const cdc_cmd_entry_t *table = cmd_tables[t].table;
		size_t count = cmd_tables[t].count;
		for (size_t i = 0; i < count; i++) {
			const cdc_cmd_entry_t *cmd = &table[i];
			if (strncasecmp(line, cmd->prefix, cmd->prefix_len) != 0)
				continue;
			if (!cmd->handler)
				continue;
			if (cmd->has_arg)
				cmd->handler(line + cmd->prefix_len);
			else
				cmd->handler(NULL);
			return;
		}
	}

	ESP_LOGW(TAG_CDC, "Unknown command: %s", line);
}

/* ── Circular FIFO for command lines ─────────────────────────────── */

static cdc_cmd_t cmd_fifo[CDC_CMD_FIFO_DEPTH];
static uint16_t fifo_w = 0;
static uint16_t fifo_r = 0;
static uint16_t fifo_count = 0;

static char assemble_buf[CDC_CMD_MAX_LEN];
static uint16_t assemble_len = 0;

static inline bool fifo_full(void)  { return fifo_count == CDC_CMD_FIFO_DEPTH; }
static inline bool fifo_empty(void) { return fifo_count == 0; }

void cdc_cmd_fifo_init(void)
{
	fifo_w = fifo_r = fifo_count = 0;
	assemble_len = 0;
}

bool cdc_cmd_push(const char *line, uint16_t len)
{
	if (len == 0) return false;
	if (len >= CDC_CMD_MAX_LEN) len = CDC_CMD_MAX_LEN - 1;
	if (fifo_full()) {
		ESP_LOGW(TAG_CDC, "FIFO full - dropping command");
		return false;
	}
	cdc_cmd_t *slot = &cmd_fifo[fifo_w];
	memcpy(slot->line, line, len);
	slot->line[len] = '\0';
	slot->len = len;
	fifo_w = (fifo_w + 1) % CDC_CMD_FIFO_DEPTH;
	fifo_count++;
	return true;
}

bool cdc_cmd_pop(cdc_cmd_t *out)
{
	if (fifo_empty()) return false;
	*out = cmd_fifo[fifo_r];
	fifo_r = (fifo_r + 1) % CDC_CMD_FIFO_DEPTH;
	fifo_count--;
	return true;
}

bool cdc_cmd_peek(cdc_cmd_t *out)
{
	if (fifo_empty()) return false;
	*out = cmd_fifo[fifo_r];
	return true;
}

uint16_t cdc_cmd_count(void)
{
	return fifo_count;
}

/* ── Send helpers ────────────────────────────────────────────────── */

void start_command_queue(unsigned char type, size_t total_size)
{
	unsigned char data_tmp[5];
	data_tmp[0] = 'C';
	data_tmp[1] = '>';
	data_tmp[2] = type;
	data_tmp[3] = (unsigned char)(total_size & 0xFF);
	data_tmp[4] = (unsigned char)((total_size >> 8) & 0xFF);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)data_tmp, 5);
}

void cdc_send_layer(uint8_t layer)
{
	start_command_queue(2 /* CDC_RESP_CURRENT_LAYER */, 1);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)&layer, 1);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

void cdc_ping(void)
{
	start_command_queue(6 /* CDC_RESP_PING */, 0);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

void cdc_send_line(const char *text)
{
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)text, strlen(text));
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)"\r\n", 2);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

void cdc_send_binary(const uint8_t *data, size_t len)
{
	tinyusb_cdcacm_write_queue(CDC_ITF, data, len);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

void cdc_send_large(const char *data, size_t len)
{
	const size_t chunk = 256;
	while (len > 0) {
		size_t n = (len < chunk) ? len : chunk;
		tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)data, n);
		tinyusb_cdcacm_write_flush(CDC_ITF, pdMS_TO_TICKS(100));
		data += n;
		len -= n;
	}
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)"\r\n", 2);
	tinyusb_cdcacm_write_flush(CDC_ITF, pdMS_TO_TICKS(100));
}

void trim_spaces(char *str)
{
	if (str == NULL) return;
	char *start = str;
	while (*start && isspace((unsigned char)*start)) start++;
	char *end = start + strlen(start);
	while (end > start && isspace((unsigned char)*(end - 1))) end--;
	size_t len = (size_t)(end - start);
	if (start != str) memmove(str, start, len);
	str[len] = '\0';
}

/* ── Receive: USB CDC chunks → line assembly ─────────────────────── */

void receive_data(const char *data, uint16_t len)
{
	if (ota_state == OTA_RECEIVING && !ota_binary_mode) {
		ota_receive_bytes(data, len);
		return;
	}

	/* Try binary protocol first — if first bytes are KS magic, consume them */
	uint16_t bin_consumed = ks_rx_feed(data, len);
	if (bin_consumed > 0) {
		data += bin_consumed;
		len  -= bin_consumed;
		if (len == 0) return;
	}

	/* Legacy text line assembly for remaining bytes */
	static bool last_was_cr = false;
	static bool overflowed = false;
	for (size_t i = 0; i < len; ++i) {
		char c = data[i];
		if (c == '\r') {
			if (!overflowed && assemble_len > 0) {
				cdc_cmd_push(assemble_buf, assemble_len);
				assemble_len = 0;
			}
			overflowed = false;
			last_was_cr = true;
			continue;
		}
		if (c == '\n') {
			if (last_was_cr) { last_was_cr = false; continue; }
			if (!overflowed && assemble_len > 0) {
				cdc_cmd_push(assemble_buf, assemble_len);
				assemble_len = 0;
			}
			overflowed = false;
			continue;
		}
		last_was_cr = false;
		if (!overflowed && assemble_len < (CDC_CMD_MAX_LEN - 1)) {
			assemble_buf[assemble_len++] = c;
		} else {
			overflowed = true;
		}
	}
}

/* ── Processing task ─────────────────────────────────────────────── */

void cdc_process_commands_task(void *arg)
{
	(void)arg;
	cdc_cmd_t cmd;
	for (;;) {
		if (ota_state == OTA_RECEIVING) {
			if (ota_binary_mode)
				while (ks_process_one()) ; /* dispatch KS frames (OTA_DATA, OTA_ABORT) */
			ota_process_chunk(); /* legacy chunk write + timeout check */
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}
		/* Process binary commands first, then text */
		while (ks_process_one())
			;
		while (cdc_cmd_pop(&cmd))
			parse_and_execute(cmd.line);
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

void init_cdc_commands(void)
{
	cdc_cmd_fifo_init();
	xTaskCreate(cdc_process_commands_task, "cdc_cmd", 6144, NULL, 4, NULL);
}
