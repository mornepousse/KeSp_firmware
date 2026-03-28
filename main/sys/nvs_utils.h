/* Generic NVS persistence helpers.
   Reusable in any ESP-IDF project — no project-specific dependencies. */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Save a binary blob and an associated u32 counter to NVS.
   Opens the given namespace in read-write mode, writes both, commits, and closes.
   Returns ESP_OK on success, or the first error encountered. */
esp_err_t nvs_save_blob_with_total(const char *ns, const char *blob_key, const void *blob,
                                    size_t blob_size, const char *total_key, uint32_t total);

/* Load a binary blob and an associated u32 counter from NVS.
   Opens the given namespace in read-only mode.
   *total is set to 0 if the total key doesn't exist.
   Returns ESP_OK if the blob was loaded, ESP_ERR_NVS_NOT_FOUND if fresh start. */
esp_err_t nvs_load_blob_with_total(const char *ns, const char *blob_key, void *blob,
                                    size_t blob_size, const char *total_key, uint32_t *total);
