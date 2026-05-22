/*
 * half_spi.c — Shared SPI2 bus lock implementation.
 * See half_spi.h for rationale and usage contract.
 */

#include "half_spi.h"
#include "esp_log.h"

static const char *TAG = "half_spi";
static SemaphoreHandle_t s_spi_mutex = NULL;

void half_spi_lock_init(void)
{
    s_spi_mutex = xSemaphoreCreateMutex();
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed — out of heap");
    }
}

void half_spi_lock(void)
{
    xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
}

void half_spi_unlock(void)
{
    xSemaphoreGive(s_spi_mutex);
}
