/* OTA firmware update over CDC (binary protocol only) */
#include "cdc_internal.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

/* ── OTA state (shared via cdc_internal.h) ───────────────────────── */
volatile ota_state_t ota_state = OTA_IDLE;
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *ota_partition = NULL;
size_t ota_total_size = 0;
volatile size_t ota_received = 0;

/* ── Binary OTA helpers (called from cdc_binary_cmds.c) ──────────── */

esp_err_t ota_bin_begin(uint32_t size)
{
    ota_partition = esp_ota_get_next_update_partition(NULL);
    if (!ota_partition) return ESP_ERR_NOT_FOUND;

    if (ota_handle != 0) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
    }

    esp_err_t err = esp_ota_begin(ota_partition, size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_CDC, "OTA bin begin failed: %s", esp_err_to_name(err));
        return err;
    }

    ota_total_size = size;
    ota_received = 0;
    ota_state = OTA_RECEIVING;
    return ESP_OK;
}

esp_err_t ota_bin_write(const uint8_t *data, uint16_t len)
{
    /* Reject writes that exceed declared total_size */
    if (ota_received + len > ota_total_size) {
        ESP_LOGE(TAG_CDC, "OTA write exceeds declared size (%u+%u > %u)",
                 (unsigned)ota_received, (unsigned)len, (unsigned)ota_total_size);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_write(ota_handle, data, len);
    if (err == ESP_OK) ota_received += len;
    return err;
}

esp_err_t ota_bin_finish(void)
{
    /* esp_ota_end consumes the handle regardless of success/failure */
    esp_err_t err = esp_ota_end(ota_handle);
    ota_handle = 0;
    if (err != ESP_OK) {
        ota_state = OTA_IDLE;
        return err;
    }
    err = esp_ota_set_boot_partition(ota_partition);
    ota_state = OTA_IDLE;
    return err;
}

void ota_bin_abort(void)
{
    if (ota_handle != 0) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
    }
    ota_state = OTA_IDLE;
}
