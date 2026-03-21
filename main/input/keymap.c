#include "keymap.h"
#include "key_definitions.h"
#include "keyboard_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

/* keymaps[] and default_layout_names[] are defined in board_keymap.c */

// Liste des macros prédéfinies
macro_t macros_list[MAX_MACROS] = {
    {"Copier", {K_LCTRL, K_C}, MACRO_1}, // Macro 1 = Ctrl + C (Copier)
    {"Coller", {K_LCTRL, K_V}, MACRO_2}, // Macro 2 = Ctrl + V (Coller)
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

#define STORAGE_NAMESPACE "storage"

void save_keymaps(uint16_t *data, size_t size_bytes) {
    nvs_handle_t my_handle;
    esp_err_t err;

    // Ouvrir NVS
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return;
    }

    // Écrire le tableau comme un blob
    err = nvs_set_blob(my_handle, "keymaps", data, size_bytes);
    if (err != ESP_OK) {
        printf("Error (%s) writing!\n", esp_err_to_name(err));
    }

    // Commit
    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) committing!\n", esp_err_to_name(err));
    }

    // Fermer
    nvs_close(my_handle);
}

void load_keymaps(uint16_t *data, size_t size_bytes) {
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return;
    }

    size_t required_size = size_bytes;
    err = nvs_get_blob(my_handle, "keymaps", data, &required_size);

    if (err == ESP_OK) {
        printf("Keymaps loaded successfully!\n");
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("No saved keymaps found.\n");
    } else {
        printf("Error (%s) reading!\n", esp_err_to_name(err));
    }

    nvs_close(my_handle);
}

void save_layout_names(char names[][MAX_LAYOUT_NAME_LENGTH], size_t layer_count) {
    nvs_handle_t my_handle;
    esp_err_t err;
    size_t size_bytes = layer_count * MAX_LAYOUT_NAME_LENGTH;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(my_handle, "layout_names", names, size_bytes);
    if (err != ESP_OK) {
        printf("Error (%s) writing layout names!\n", esp_err_to_name(err));
    }

    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) committing layout names!\n", esp_err_to_name(err));
    }

    nvs_close(my_handle);
}

void load_layout_names(char names[][MAX_LAYOUT_NAME_LENGTH], size_t layer_count) {
    nvs_handle_t my_handle;
    esp_err_t err;
    size_t required_size = layer_count * MAX_LAYOUT_NAME_LENGTH;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return;
    }

    esp_err_t blob_err = nvs_get_blob(my_handle, "layout_names", names, &required_size);
    if (blob_err == ESP_OK) {
        printf("Layout names loaded successfully!\n");
    } else if (blob_err == ESP_ERR_NVS_NOT_FOUND) {
        printf("No saved layout names found.\n");
    } else {
        printf("Error (%s) reading layout names!\n", esp_err_to_name(blob_err));
    }

    nvs_close(my_handle);
}

void save_macros(macro_t *macros, size_t count) {
    nvs_handle_t my_handle;
    esp_err_t err;
    size_t size_bytes = MAX_MACROS * sizeof(macro_t);

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(my_handle, "macros", macros, size_bytes);
    if (err != ESP_OK) {
        printf("Error (%s) writing macros!\n", esp_err_to_name(err));
    }

    err = nvs_set_u32(my_handle, "macros_count", (uint32_t)count);
    if (err != ESP_OK) {
        printf("Error (%s) writing macros count!\n", esp_err_to_name(err));
    }

    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) committing macros!\n", esp_err_to_name(err));
    }

    nvs_close(my_handle);
}

void load_macros(macro_t *macros, size_t count) {
    nvs_handle_t my_handle;
    esp_err_t err;
    size_t required_size = MAX_MACROS * sizeof(macro_t);

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return;
    }

    esp_err_t blob_err = nvs_get_blob(my_handle, "macros", macros, &required_size);
    if (blob_err == ESP_OK) {
        printf("Macros loaded successfully!\n");
    } else if (blob_err == ESP_ERR_NVS_NOT_FOUND) {
        printf("No saved macros found.\n");
    } else {
        printf("Error (%s) reading macros!\n", esp_err_to_name(blob_err));
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
    // Initialiser NVS
    ESP_LOGI("NVS", "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Si la partition NVS est pleine ou incompatible → effacer et réinitialiser
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}
