/* CDC ACM core: binary protocol dispatch, send helpers */
#include "cdc_internal.h"
#include "cadence.h"   /* CDC_ATTENTE_MAX_MS */

const char *TAG_CDC = "CDC_CMD";

/* ── Send helpers ────────────────────────────────────────────────── */

void cdc_send_binary(const uint8_t *data, size_t len)
{
    tinyusb_cdcacm_write_queue(CDC_ITF, data, len);
    tinyusb_cdcacm_write_flush(CDC_ITF, 0);
}

/* ── Receive: USB CDC chunks → binary protocol ──────────────────── */

static TaskHandle_t s_cdc_task;

void receive_data(const char *data, uint16_t len)
{
    /* Feed everything to the binary protocol parser */
    uint16_t consumed = ks_rx_feed(data, len);
    if (consumed < len) {
        ESP_LOGD(TAG_CDC, "Ignored %u non-binary bytes", len - consumed);
    }
    if (s_cdc_task) xTaskNotifyGive(s_cdc_task);   /* a frame may be complete: go */
}

/* ── Processing task ─────────────────────────────────────────────── */

/* Woken by receive_data(), not by a clock (2026-09-25). It used to poll every
 * 50 ms — with no USB cable at all on a half on battery, twenty wake-ups a
 * second for nothing, one of the six interleaved pollers that kept the
 * processor from ever getting the 30 ms of calm automatic light sleep needs
 * (measured: 24 mA between keystrokes, 6-10 mA with the pollers stretched).
 * A notification given while a batch is being processed is latched, so the
 * next wait returns at once: no frame is left behind. The 1 s timeout is a
 * safety net only (CDC_ATTENTE_MAX_MS). */
void cdc_process_commands_task(void *arg)
{
    (void)arg;
    s_cdc_task = xTaskGetCurrentTaskHandle();
    for (;;) {
        while (ks_process_one())
            ;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CDC_ATTENTE_MAX_MS));
    }
}

void init_cdc_commands(void)
{
    ks_rx_init();   /* creates the RX handoff mutex before starting the dispatch task */
    xTaskCreate(cdc_process_commands_task, "cdc_cmd", 6144, NULL, 4, NULL);
}
