#include "keymap.h"
#include "key_definitions.h"
#include "keyboard_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "KEYMAP";

/* keymaps[] and default_layout_names[] are defined in board_keymap.c */

macro_t macros_list[MAX_MACROS] = {
    { .name = "Copier",  .steps = {{0}}, .keys = {K_LCTRL, K_C}, .key_definition = MACRO_1 },
    { .name = "Coller",  .steps = {{0}}, .keys = {K_LCTRL, K_V}, .key_definition = MACRO_2 },
};
size_t macros_count = 2;

void recalc_macros_count(void) {
    for (int i = MAX_MACROS - 1; i >= 0; --i) {
        if (macros_list[i].name[0] != '\0') {
            macros_count = i + 1;
            return;
        }
    }
    macros_count = 0;
}

void save_keymaps(uint16_t *data, size_t size_bytes) {
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS!", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(my_handle, "keymaps", data, size_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) writing keymaps!", esp_err_to_name(err));
    }

    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing keymaps!", esp_err_to_name(err));
    }

    nvs_close(my_handle);
}

void load_keymaps(uint16_t *data, size_t size_bytes) {
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) opening NVS, using defaults", esp_err_to_name(err));
        return;
    }

    size_t required_size = size_bytes;
    err = nvs_get_blob(my_handle, "keymaps", data, &required_size);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Keymaps loaded from NVS");
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved keymaps, using defaults");
    } else {
        ESP_LOGE(TAG, "Error (%s) reading keymaps!", esp_err_to_name(err));
    }

    nvs_close(my_handle);
}

void save_layout_names(char names[][MAX_LAYOUT_NAME_LENGTH], size_t layer_count) {
    nvs_handle_t my_handle;
    esp_err_t err;
    size_t size_bytes = layer_count * MAX_LAYOUT_NAME_LENGTH;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS!", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(my_handle, "layout_names", names, size_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) writing layout names!", esp_err_to_name(err));
    }

    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing layout names!", esp_err_to_name(err));
    }

    nvs_close(my_handle);
}

void load_layout_names(char names[][MAX_LAYOUT_NAME_LENGTH], size_t layer_count) {
    nvs_handle_t my_handle;
    esp_err_t err;
    size_t required_size = layer_count * MAX_LAYOUT_NAME_LENGTH;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) opening NVS, using defaults", esp_err_to_name(err));
        return;
    }

    esp_err_t blob_err = nvs_get_blob(my_handle, "layout_names", names, &required_size);
    if (blob_err == ESP_OK) {
        ESP_LOGI(TAG, "Layout names loaded from NVS");
    } else if (blob_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved layout names, using defaults");
    } else {
        ESP_LOGE(TAG, "Error (%s) reading layout names!", esp_err_to_name(blob_err));
    }

    nvs_close(my_handle);
}

void save_macros(macro_t *macros, size_t count) {
    nvs_handle_t my_handle;
    esp_err_t err;
    size_t size_bytes = MAX_MACROS * sizeof(macro_t);

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS!", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(my_handle, "macros", macros, size_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) writing macros!", esp_err_to_name(err));
    }

    err = nvs_set_u32(my_handle, "macros_count", (uint32_t)count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) writing macros count!", esp_err_to_name(err));
    }

    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing macros!", esp_err_to_name(err));
    }

    nvs_close(my_handle);
}

void load_macros(macro_t *macros, size_t count) {
    nvs_handle_t my_handle;
    esp_err_t err;
    size_t required_size = MAX_MACROS * sizeof(macro_t);

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) opening NVS, using defaults", esp_err_to_name(err));
        return;
    }

    /* Check stored size first — if it doesn't match current struct, skip (format changed) */
    size_t stored_size = 0;
    esp_err_t size_err = nvs_get_blob(my_handle, "macros", NULL, &stored_size);
    if (size_err == ESP_OK && stored_size != required_size) {
        ESP_LOGW(TAG, "Macros NVS size mismatch (stored=%u, expected=%u), using defaults",
                 (unsigned)stored_size, (unsigned)required_size);
        nvs_close(my_handle);
        return;
    }

    esp_err_t blob_err = nvs_get_blob(my_handle, "macros", macros, &required_size);
    if (blob_err == ESP_OK) {
        ESP_LOGI(TAG, "Macros loaded from NVS");
    } else if (blob_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved macros, using defaults");
    } else {
        ESP_LOGE(TAG, "Error (%s) reading macros!", esp_err_to_name(blob_err));
    }

    uint32_t stored_count = (uint32_t)count;
    esp_err_t count_err = nvs_get_u32(my_handle, "macros_count", &stored_count);
    if (count_err == ESP_OK && stored_count <= MAX_MACROS) {
        macros_count = stored_count;
    } else {
        recalc_macros_count();
    }

    nvs_close(my_handle);
}

void keymap_init_nvs() {
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}
