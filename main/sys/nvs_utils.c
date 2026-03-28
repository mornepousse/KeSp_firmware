/* Generic NVS persistence helpers */
#include "nvs_utils.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG_NVS = "NVS_UTILS";

esp_err_t nvs_save_blob_with_total(const char *ns, const char *blob_key, const void *blob,
                                    size_t blob_size, const char *total_key, uint32_t total)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "NVS open(%s) failed: %s", blob_key, esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, blob_key, blob, blob_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "NVS write(%s) failed: %s", blob_key, esp_err_to_name(err));
        nvs_close(h);
        return err;
    }
    nvs_set_u32(h, total_key, total);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t nvs_load_blob_with_total(const char *ns, const char *blob_key, void *blob,
                                    size_t blob_size, const char *total_key, uint32_t *total)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = blob_size;
    err = nvs_get_blob(h, blob_key, blob, &sz);
    if (err == ESP_OK) {
        ESP_LOGI(TAG_NVS, "%s loaded from NVS", blob_key);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG_NVS, "No saved %s, starting fresh", blob_key);
    } else {
        ESP_LOGE(TAG_NVS, "Error reading %s: %s", blob_key, esp_err_to_name(err));
    }

    if (total) {
        if (nvs_get_u32(h, total_key, total) != ESP_OK)
            *total = 0;
    }
    nvs_close(h);
    return err;
}
