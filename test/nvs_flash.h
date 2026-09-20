/* stub: nvs_flash.h — host builds (TEST_HOST) only.
 * Provides esp_err_t, the NVS flash error codes, and ESP_ERROR_CHECK.
 * The real implementations are in nvs_fake.c. */
#pragma once
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                          0
#define ESP_FAIL                        (-1)
#define ESP_ERR_NVS_NO_FREE_PAGES       0x1101
#define ESP_ERR_NVS_NEW_VERSION_FOUND   0x1102
#define ESP_ERR_NVS_INVALID_LENGTH      0x1109

/* No-op — the fake NVS always returns ESP_OK */
#define ESP_ERROR_CHECK(x)   do { (void)(x); } while(0)

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
