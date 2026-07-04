/* stub: nvs_flash.h — builds hôte (TEST_HOST) uniquement.
 * Fournit esp_err_t, les codes d'erreur NVS flash et ESP_ERROR_CHECK.
 * Les implémentations réelles sont dans nvs_fake.c. */
#pragma once
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                          0
#define ESP_FAIL                        (-1)
#define ESP_ERR_NVS_NO_FREE_PAGES       0x1101
#define ESP_ERR_NVS_NEW_VERSION_FOUND   0x1102
#define ESP_ERR_NVS_INVALID_LENGTH      0x1109

/* No-op — le fake NVS retourne toujours ESP_OK */
#define ESP_ERROR_CHECK(x)   do { (void)(x); } while(0)

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
