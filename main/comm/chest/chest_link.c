/* Chest link, S3 master — spec docs/superpowers/specs/2026-09-29-chest-link-s3-master-design.md.
 * Presence = a USB host (the chest is powered by the left half's USB only).
 * Absent: no SPI device, GPIO3 an input (R48 pulls CS to the chest's rail —
 * driving it into a dead rail costs ~0.33 mA), IRQ off, the task blocked.
 * Present: device added, IRQ armed, a read every CHEST_POLL_MS or at once on
 * an IRQ edge, every transaction under the radio owner's bus lock. */
#include "chest_link.h"
#include "chest_proto.h"
#include "chest_gate.h"
#include "rf_bus.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "chest";
#define CHEST_SPI_HZ   1000000
#define CHEST_POLL_MS  250u
#define CHEST_DEV_RETRY_MS 1000u   /* spi_bus_add_device failed: retry, don't block forever */

static TaskHandle_t         s_task;
static spi_device_handle_t  s_dev;
static volatile bool        s_want;          /* presence asked by the sleep task */
static volatile uint8_t     s_view;
static volatile uint16_t    s_view_op;
static chest_confirm_t      s_confirm;
static uint8_t               s_badver_logged;  /* once per presence session, reset in go_absent */
static uint8_t               s_corrupt_logged; /* once per presence session, reset in go_absent */
static WORD_ALIGNED_ATTR uint8_t s_rx[CHEST_REG_SIZE];
static WORD_ALIGNED_ATTR uint8_t s_tx[4];

static void cs_release(void)
{
    const gpio_config_t c = { .pin_bit_mask = 1ULL << BOARD_CHEST_CS, .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&c);
}

static void IRAM_ATTR irq_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static bool dev_add(void)
{
    const spi_device_interface_config_t d = {
        .command_bits = 8, .address_bits = 8, .dummy_bits = 8,   /* spi_slave_hd: cmd, addr, dummy */
        .mode = 0, .clock_speed_hz = CHEST_SPI_HZ,
        .spics_io_num = BOARD_CHEST_CS, .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    return spi_bus_add_device(rf_bus_host(), &d, &s_dev) == ESP_OK;
}

static bool xfer(spi_transaction_t *t)
{
    if (!s_dev || !rf_bus_lock(20)) return false;          /* radio busy: next round */
    bool ok = spi_device_polling_transmit(s_dev, t) == ESP_OK;
    rf_bus_unlock();
    return ok;
}

static bool read_block(void)
{
    spi_transaction_t t = { .cmd = CHEST_CMD_RDBUF, .addr = 0,
                            .rxlength = CHEST_REG_SIZE * 8, .rx_buffer = s_rx };
    return xfer(&t);
}

static void write_confirm(void)
{
    s_tx[0] = CHEST_CONFIRM_MAGIC;
    spi_transaction_t t = { .cmd = CHEST_CMD_WRBUF, .addr = CHEST_REG_USER_CONFIRM,
                            .length = 8, .tx_buffer = s_tx };
    if (!xfer(&t)) ESP_LOGW(TAG, "confirmation not written (bus busy) — retried by the rule");
}

static void go_absent(void)
{
    gpio_intr_disable(BOARD_CHEST_IRQ);
    if (s_dev) {
        esp_err_t e = spi_bus_remove_device(s_dev);
        if (e != ESP_OK) ESP_LOGW(TAG, "spi_bus_remove_device: %s", esp_err_to_name(e));
        s_dev = NULL;
    }
    cs_release();
    chest_gate_publish(0, 0);
    s_confirm.armed = false;
    s_view = 0; s_view_op = 0;
    s_badver_logged = 0; s_corrupt_logged = 0;   /* once-per-presence-session logs, next session starts fresh */
    ESP_LOGI(TAG, "chest gone: CS released");
}

static void chest_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* s_dev: poll cadence. s_want && !s_dev: device add failed, retry
         * instead of blocking forever. Otherwise (battery, no chest): block
         * until the sleep task's presence hand-off wakes us — unchanged. */
        TickType_t wait = portMAX_DELAY;
        if (s_dev) wait = pdMS_TO_TICKS(CHEST_POLL_MS);
        else if (s_want) wait = pdMS_TO_TICKS(CHEST_DEV_RETRY_MS);
        ulTaskNotifyTake(pdTRUE, wait);

        /* Taken on EVERY round, whatever happens below — a press queued for
         * an operation the owner saw must not survive a non-OK round (chest
         * reboot -> ABSENT, CORRUPT, BAD_VERSION, a bus-busy skipped read)
         * and confirm a different op at the next OK block (review Important,
         * 2026-09-29). Matched against the CURRENT chest state below. */
        uint32_t pressed = chest_gate_take_press();

        if (s_want && !s_dev) {
            if (!dev_add()) { ESP_LOGE(TAG, "spi_bus_add_device failed"); continue; }
            gpio_intr_enable(BOARD_CHEST_IRQ);
            ESP_LOGI(TAG, "USB host present: talking to the chest (CS GPIO%d, IRQ GPIO%d)",
                     BOARD_CHEST_CS, BOARD_CHEST_IRQ);
        } else if (!s_want && s_dev) {
            go_absent();
            continue;
        }
        if (!s_dev || !read_block()) continue;

        chest_status_t st;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        chest_block_t blk = chest_proto_parse(s_rx, CHEST_REG_SIZE, &st);
        switch (blk) {
        case CHEST_BLOCK_OK:
            s_view = CHEST_VIEW_PRESENT | (st.state & 0x07);
            s_view_op = st.pending_op;
            chest_gate_publish(st.pending_op, st.instance);
            if (chest_press_matches(pressed, blk, &st)) chest_confirm_request(&s_confirm, &st, now);
            if (chest_confirm_step(&s_confirm, &st, now)) write_confirm();
            break;
        case CHEST_BLOCK_BAD_VERSION:
            if (!s_badver_logged) { ESP_LOGW(TAG, "chest speaks protocol %u, we speak %u: ignored", s_rx[4], CHEST_PROTO_VERSION); s_badver_logged = 1; }
            s_view = CHEST_VIEW_PRESENT | CHEST_VIEW_BADVER; s_view_op = 0;
            chest_gate_publish(0, 0);
            break;
        case CHEST_BLOCK_CORRUPT:
            if (!s_corrupt_logged) { ESP_LOGW(TAG, "chest block corrupt (magic/CRC/short read): ignored"); s_corrupt_logged = 1; }
            s_view = 0; s_view_op = 0;
            chest_gate_publish(0, 0);
            break;
        default:   /* CHEST_BLOCK_ABSENT: booting or unpowered, the ordinary case, never logged */
            s_view = 0; s_view_op = 0;
            chest_gate_publish(0, 0);
            break;
        }
    }
}

void chest_link_start(void)
{
    cs_release();
    const gpio_config_t irq = { .pin_bit_mask = 1ULL << BOARD_CHEST_IRQ, .mode = GPIO_MODE_INPUT,
                                .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                .intr_type = GPIO_INTR_POSEDGE };   /* R49 is the pull-down */
    gpio_config(&irq);
    gpio_intr_disable(BOARD_CHEST_IRQ);
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(e)); return; }
    gpio_isr_handler_add(BOARD_CHEST_IRQ, irq_isr, NULL);
    xTaskCreatePinnedToCore(chest_task, "chest", 3072, NULL, 3, &s_task, 1);
}

void chest_link_presence(bool usb_host)
{
    if (usb_host == s_want || !s_task) return;
    s_want = usb_host;
    xTaskNotifyGive(s_task);
}

uint8_t chest_link_view(uint16_t *op)
{
    if (op) *op = s_view_op;
    return s_view;
}
