/* CDC ACM core: FIFO, line assembly, send helpers, processing task */
#include "cdc_internal.h"

const char *TAG_CDC = "CDC_CMD";

/* Custom log handler to redirect ESP_LOG to CDC (currently disabled) */
static vprintf_like_t original_log_vprintf = NULL;

__attribute__((unused))
static int cdc_log_vprintf(const char *fmt, va_list args) {
    int ret = 0;
    if (original_log_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = original_log_vprintf(fmt, args_copy);
        va_end(args_copy);
    }
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        if ((size_t)len > sizeof(buf) - 1) len = sizeof(buf) - 1;
        if (tud_cdc_n_connected(CDC_ITF)) {
            tinyusb_cdcacm_write_queue(CDC_ITF, (uint8_t*)buf, len);
            tinyusb_cdcacm_write_flush(CDC_ITF, 0);
        }
    }
    return ret;
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
	ESP_LOGI(TAG_CDC, "CDC RX line len=%u", (unsigned)len);
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
	if (total_size > 0) {
		data_tmp[3] = (unsigned char)(total_size & 0xFF);
		data_tmp[4] = (unsigned char)((total_size >> 8) & 0xFF);
	} else {
		data_tmp[3] = 0;
		data_tmp[4] = 0;
	}
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)data_tmp, 5);
}

void cdc_send_layer(uint8_t layer)
{
	start_command_queue(CDC_RESP_CURRENT_LAYER, 1);
	tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)&layer, 1);
	tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

void cdc_ping(void)
{
	start_command_queue(CDC_RESP_PING, 0);
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
	/* OTA binary receive mode — bypass line assembly */
	if (ota_state == OTA_RECEIVING) {
		ota_receive_bytes(data, len);
		return;
	}

	static bool last_was_cr = false;
	static bool overflowed = false;
	for (size_t i = 0; i < len; ++i)
	{
		char c = data[i];
		if (c == '\r')
		{
			if (!overflowed && assemble_len > 0) {
				cdc_cmd_push(assemble_buf, assemble_len);
				assemble_len = 0;
			}
			overflowed = false;
			last_was_cr = true;
			continue;
		}
		if (c == '\n')
		{
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
			ESP_LOGW(TAG_CDC, "CDC RX overflow (len>=%u)", (unsigned)CDC_CMD_MAX_LEN);
		}
	}
}

/* ── Processing task ─────────────────────────────────────────────── */

void cdc_process_commands_task(void *arg)
{
	(void)arg;
	cdc_cmd_t cmd;
	for (;;)
	{
		if (ota_state == OTA_RECEIVING) {
			ota_process_chunk();
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}

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
