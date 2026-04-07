/* CDC ACM core: binary protocol dispatch, send helpers */
#include "cdc_internal.h"

const char *TAG_CDC = "CDC_CMD";

/* ── Send helpers ────────────────────────────────────────────────── */

void cdc_send_binary(const uint8_t *data, size_t len)
{
    tinyusb_cdcacm_write_queue(CDC_ITF, data, len);
    tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

/* ── Receive: USB CDC chunks → binary protocol ──────────────────── */

void receive_data(const char *data, uint16_t len)
{
    /* Feed everything to the binary protocol parser */
    uint16_t consumed = ks_rx_feed(data, len);
    if (consumed < len) {
        ESP_LOGD(TAG_CDC, "Ignored %u non-binary bytes", len - consumed);
    }
}

/* ── Processing task ─────────────────────────────────────────────── */

void cdc_process_commands_task(void *arg)
{
    (void)arg;
    for (;;) {
        while (ks_process_one())
            ;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void init_cdc_commands(void)
{
    xTaskCreate(cdc_process_commands_task, "cdc_cmd", 6144, NULL, 4, NULL);
}
