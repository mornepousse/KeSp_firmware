/* OTA firmware update over CDC */
#include "cdc_internal.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

/* ── OTA state (shared via cdc_internal.h) ───────────────────────── */
volatile ota_state_t ota_state = OTA_IDLE;
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *ota_partition = NULL;
size_t ota_total_size = 0;
volatile size_t ota_received = 0;
uint8_t ota_buf[OTA_CHUNK_SIZE];
volatile size_t ota_buf_pos = 0;
volatile bool ota_chunk_ready = false;
volatile uint32_t ota_last_activity_ms = 0;

void ota_abort(const char *reason)
{
	esp_ota_abort(ota_handle);
	ota_state = OTA_IDLE;
	ota_handle = 0;
	ESP_LOGE(TAG_CDC, "OTA aborted: %s", reason);
	char msg[64];
	snprintf(msg, sizeof(msg), "OTA_FAIL %s", reason);
	cdc_send_line(msg);
}

void cmd_ota_start(const char *arg)
{
	if (arg == NULL || *arg == '\0') {
		cdc_send_line("OTA_ERROR missing size");
		return;
	}
	uint32_t size = (uint32_t)strtoul(arg, NULL, 10);
	if (size == 0 || size > 0x200000) {
		cdc_send_line("OTA_ERROR invalid size");
		return;
	}

	ota_partition = esp_ota_get_next_update_partition(NULL);
	if (!ota_partition) {
		cdc_send_line("OTA_ERROR no update partition");
		return;
	}

	esp_err_t err = esp_ota_begin(ota_partition, size, &ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG_CDC, "OTA begin failed: %s", esp_err_to_name(err));
		cdc_send_line("OTA_ERROR begin failed");
		return;
	}

	ota_total_size = size;
	ota_received = 0;
	ota_buf_pos = 0;
	ota_chunk_ready = false;
	ota_last_activity_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
	ota_state = OTA_RECEIVING;

	ESP_LOGI(TAG_CDC, "OTA started: %lu bytes -> %s", (unsigned long)size, ota_partition->label);
	char resp[64];
	snprintf(resp, sizeof(resp), "OTA_READY %u", OTA_CHUNK_SIZE);
	cdc_send_line(resp);
}

void ota_receive_bytes(const char *data, uint16_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (ota_chunk_ready) break;
		ota_buf[ota_buf_pos++] = (uint8_t)data[i];
		ota_received++;
		if (ota_buf_pos >= OTA_CHUNK_SIZE || ota_received >= ota_total_size) {
			ota_chunk_ready = true;
		}
	}
	ota_last_activity_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void ota_process_chunk(void)
{
	if (ota_chunk_ready) {
		esp_err_t err = esp_ota_write(ota_handle, ota_buf, ota_buf_pos);
		if (err != ESP_OK) {
			ota_abort("write error");
		} else if (ota_received >= ota_total_size) {
			err = esp_ota_end(ota_handle);
			if (err != ESP_OK) {
				ota_abort("validation failed");
			} else {
				err = esp_ota_set_boot_partition(ota_partition);
				if (err != ESP_OK) {
					ota_abort("set boot failed");
				} else {
					ota_state = OTA_IDLE;
					cdc_send_line("OTA_DONE");
					ESP_LOGI(TAG_CDC, "OTA complete, rebooting...");
					vTaskDelay(pdMS_TO_TICKS(500));
					esp_restart();
				}
			}
		} else {
			ota_buf_pos = 0;
			ota_chunk_ready = false;
			char resp[48];
			snprintf(resp, sizeof(resp), "OTA_OK %u/%u",
					 (unsigned)ota_received, (unsigned)ota_total_size);
			cdc_send_line(resp);
		}
	} else {
		uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
		if ((now - ota_last_activity_ms) > OTA_TIMEOUT_MS) {
			ota_abort("timeout");
		}
	}
}
