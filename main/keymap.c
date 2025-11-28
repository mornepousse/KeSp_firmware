#include "keymap.h"
#include "key_definitions.h"
#include "keyboard_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

// A bit different from QMK, default returns you to the first layer, LOWER and
// raise increase/lower layer by order.
#define DEFAULT 0x100
#define LOWER 0x101
#define RAISE 0x102

// // Keymaps are designed to be relatively interchangeable with QMK
// enum custom_keycodes
// {
//   QWERTY,
//   NUM,
//   PLUGINS,
// };

// // Set these for each layer and use when layers are needed in a hold-to use
// // layer
// enum layer_holds
// {
//   QWERTY_H = LAYER_HOLD_BASE_VAL,
//   NUM_H,
//   FUNCS_H
// };

// array to hold names of layouts for oled
char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH] = {
    "DVORAK",
    "EXTRA",
    "QWERTY",
    "EXTRA",
    "EXTRA",
    "EXTRA",
    "EXTRA",
    "EXTRA",
    "EXTRA",
    "EXTRA",
};

// Fillers to make layering more clearaoeuouoeu
#define _______ K_TRNS
#define XXXXXXX K_NO
#ifdef VERSION_1

uint16_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

    {// dvorak sous qwerty inter
     {K_ESC, K_ENT, /**/ K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, /**/ TO_L2,
      K_INT3, /**/ K_LBRC},
     {K_DEL, K_1, K_COMM, K_DOT, K_P, K_Y, K_F, K_G, K_C, K_R, K_0, K_EQUAL,
      /**/ MO_L1},
     {K_TAB, K_QUOT, K_O, K_E, K_U, K_I, K_D, K_H, K_T, K_N, K_L, K_SLSH,
      /**/ K_RBRC},
     {K_RALT, K_A, K_Q, K_J, K_K, K_X, K_B, K_M, K_W, K_V, K_S, K_MINUS,
      /**/ K_RSHIFT},
     {K_LCTRL, K_SCLN, K_LALT, K_LWIN, K_LSHIFT, K_SPACE, K_BSPACE, K_ENTER,
      K_BSLSH, K_Z, K_Z, K_GRV, /**/ K_NO}},
    {{K_NO, K_NO, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_NO, K_NO,
      K_L_PARENTHESIS},
     {K_NO, K_F1, K_HOME, K_NO, K_END, K_NO, K_NO, K_NO, K_NO, K_NO, K_F10, K_F11,
      K_NO},
     {K_NO, K_NO, K_LEFT, K_UP, K_RIGHT, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_F12,
      K_R_PARENTHESIS},
     {K_NO, K_NO, K_NO, K_DOWN, MACRO_2, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO,
      K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO,
      K_NO}},
    {// dvorak sous layeur custom pour windows/macos
     {K_ESC, K_ENT, /**/ K_2, K_3, K_4, K_5, K_6,
      K_7, K_8, K_9, /**/ TO_L0, K_INT3, /**/ K_MINUS},
     /////////////////
     /////////////////////////////////////////////
     {K_DEL, K_1, K_W, K_E, K_R, K_T, K_Y,
      K_U, K_I, K_O, K_0, K_RBRC, /**/ MO_L1},
     {K_TAB, K_Q, K_S, K_D, K_F, K_G, K_H,
      K_J, K_K, K_L, K_P, K_LBRC, /**/ K_EQUAL},
     {K_RALT, K_A, K_X, K_C, K_V, K_B, K_N,
      K_M, K_COMM, K_DOT, K_SCLN, K_QUOT, /**/ K_RSHIFT},
     {K_LCTRL, K_Z, K_LALT, K_LWIN, K_LSHIFT, K_SPACE, K_BSPACE, K_ENTER, K_BSLSH,
      K_DELETE, K_SLSH, K_GRV, /**/ K_NO}}};

#endif

#ifdef VERSION_2

uint16_t keymaps[LAYERS][MATRIX_ROWS][MATRIX_COLS] = {

    {// dvorak sous qwerty inter
     {K_DEL,  K_1,    K_2,    K_3,    K_4,      K_5,     K_LBRC,   K_6,     K_7,     K_8,      K_9,   K_0,    K_EQL},
     {K_TAB,  K_QUOT, K_COMM, K_DOT,  K_P,      K_Y,      MO_L1,    K_F,     K_G,     K_C,      K_R,   K_L,    K_SLSH},
     {K_RALT,  K_A,    K_O,    K_E,    K_U,      K_I,     K_RBRC,  K_D,     K_H,     K_T,     K_N,   K_S,   K_MINUS},
     {K_LCTRL, K_SCLN, K_Q,    K_J,    K_K,      K_X,     K_LWIN,  K_B,     K_M,     K_W,     K_V,   K_Z,   K_GRV},
     {K_ESC,  K_ENT,   K_LALT, K_LWIN, K_LSHIFT, K_SPACE, MO_L1,    K_BSPACE,K_ENT, K_BSLSH, K_RWIN,K_HELP,TO_L2}
    },

    {
      {K_NO, K_NO, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_NO, K_NO, K_L_PARENTHESIS},
     {K_NO, K_F1, K_HOME, K_NO, K_END, K_NO, K_NO, K_NO, K_NO, K_NO, K_F10, K_F11, K_NO},
     {K_NO, K_NO, K_LEFT, K_UP, K_RIGHT, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_F12, K_R_PARENTHESIS},
     {K_NO, K_NO, K_NO, K_DOWN, MACRO_2, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {// dvorak sous layeur custom pour windows/macos
     {K_ESC, K_ENT, /**/ K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, /**/ TO_L0, K_INT3, /**/ K_MINUS},
     {K_DEL, K_1, K_W, K_E, K_R, K_T, K_Y, K_U, K_I, K_O, K_0, K_RBRC, /**/ MO_L1},
     {K_TAB, K_Q, K_S, K_D, K_F, K_G, K_H, K_J, K_K, K_L, K_P, K_LBRC, /**/ K_EQUAL},
     {K_RALT, K_A, K_X, K_C, K_V, K_B, K_N, K_M, K_COMM, K_DOT, K_SCLN, K_QUOT, /**/ K_RSHIFT},
     {K_LCTRL, K_Z, K_LALT, K_LWIN, K_LSHIFT, K_SPACE, K_BSPACE, K_ENTER, K_BSLSH, K_DELETE, K_SLSH, K_GRV, /**/ K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    },
    {
      {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO},
     {K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO, K_NO}
    }
    };

#endif


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

