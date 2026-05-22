/*
 * half_scan_task.c — KaSe half firmware main task.
 *
 * Owns the keyboard_button component (matrix scan) and the NRF24L01+ PTX
 * radio. On each key event, encodes a PKT_KEY and transmits it to the dongle.
 * A periodic esp_timer sends PKT_HEARTBEAT with the current pressed-key bitmap.
 *
 * Architecture: single FreeRTOS task (half_scan_task, prio 10, core 0).
 * The task body blocks forever on portMAX_DELAY — all work is done in callbacks.
 *
 * half_diff_emit is a pure function (no FreeRTOS deps) exposed for host testing
 * under TEST_HOST. The rest of the file is excluded from host builds.
 */

#include "half_scan_task.h"
#include "rf_packet.h"   /* rf_bitmap_set/get, RF_HALF_ROWS, RF_HALF_COLS, etc. */

/* In production: include keyboard_button.h for the real keyboard_btn_data_t.
 * In TEST_HOST: keyboard_btn_data_t is defined in half_scan_task.h. */
#ifndef TEST_HOST
#include "board.h"           /* BOARD_NRF_*, COLS*, ROWS*, BOARD_DEBOUNCE_TICKS */
#include "keyboard_button.h"
#include "rf_driver.h"
#include "half_spi.h"        /* half_spi_lock_init / half_spi_lock / half_spi_unlock */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#if CONFIG_KASE_HAS_TRACKPAD
#include "trackpad.h"
#endif
#if CONFIG_KASE_HAS_EINK
#include "eink.h"
#endif
#if CONFIG_KASE_HAS_ESPNOW
#include "espnow_info.h"
#include "espnow_link.h"
#include "espnow_msg.h"   /* en_battery_t */
#endif

static const char *TAG = "half_scan";

/* ── Radio state ────────────────────────────────────────────── */
/* Non-static: shared with trackpad.c via extern rf_radio_t s_radio.
 * trackpad_task calls rf_driver_send(&s_radio, ...) to send PKT_TRACKPAD. */
rf_radio_t s_radio;

/* Serializes NRF SPI access: rf_driver_send is called from BOTH the
 * keyboard_button callback task (on key events) and the esp_timer task
 * (heartbeat). spi_device_polling_transmit aborts if two transactions
 * overlap, so all sends must be mutually exclusive.
 * The lock is now owned by half_spi.c (shared with e-ink and trackpad). */

/* ── Packet sequence counter — shared by PKT_KEY and PKT_HEARTBEAT ─ */
static volatile uint8_t s_seq = 0;

/* ── Local pressed-key bitmap (maintained for PKT_HEARTBEAT) ── */
static uint8_t s_pressed_bitmap[RF_HALF_BITMAP_BYTES];

/* ── Retry state: one pending retry stored on MAX_RT ──────────── */
static volatile bool           s_has_pending_retry = false;
static volatile rf_key_event_t s_pending_retry;

/* ── Link quality counter (MAX_RT events since last heartbeat) ─ */
/* rf_tx_max_rt_count is the global in rf_driver.c (PTX section). */
extern uint32_t rf_tx_max_rt_count;

/* ── NRF radio config (built from board.h defines) ─────────── */
static rf_radio_cfg_t board_nrf_cfg(void)
{
    rf_radio_cfg_t c = {
        .spi_host         = BOARD_NRF_SPI_HOST,
        .pin_mosi         = BOARD_NRF_SPI_MOSI,
        .pin_miso         = BOARD_NRF_SPI_MISO,
        .pin_sck          = BOARD_NRF_SPI_SCK,
        .clock_hz         = BOARD_NRF_SPI_CLOCK_HZ,
        .pin_csn          = BOARD_NRF_CSN_GPIO,
        .pin_ce           = BOARD_NRF_CE_GPIO,
        .pin_irq          = BOARD_NRF_IRQ_GPIO,
        .channel          = BOARD_NRF_CHANNEL,
        .rx_addr          = { 'K', 'a', 'S', 'e' },   /* base address (4 bytes) */
        .addr_suffix      = BOARD_NRF_ADDR_SUFFIX,     /* 0x01 left, 0x02 right */
        .shares_bus_first = true,                       /* half has exactly one radio */
    };
    return c;
}
#endif /* TEST_HOST */

/* ────────────────────────────────────────────────────────────── */
/* half_diff_emit — pure bitmap diff helper (also host-tested).   */
/* keyboard_btn_data_t is either the real type (production) or    */
/* the stub typedef (TEST_HOST). Bitmap is the authoritative      */
/* source — only truly new presses (not already in bitmap) emit.  */
/* ────────────────────────────────────────────────────────────── */
void half_diff_emit(
    uint8_t *bitmap,
    const keyboard_btn_data_t *pressed,  uint32_t press_cnt,
    const keyboard_btn_data_t *released, uint32_t release_cnt,
    void (*emit_cb)(uint8_t row, uint8_t col, bool pressed, void *ctx),
    void *ctx)
{
    /* ROW2COL: keyboard_button drives rows (output) and senses cols (input),
     * so output_index = row (0..4), input_index = col (0..6). */
    /* Process releases first (matches keyboard convention) */
    for (uint32_t i = 0; i < release_cnt; i++) {
        uint8_t row = released[i].output_index;
        uint8_t col = released[i].input_index;
        if (row < RF_HALF_ROWS && col < RF_HALF_COLS) {
            rf_bitmap_set(bitmap, row, col, false);
            if (emit_cb) emit_cb(row, col, false, ctx);
        }
    }
    /* New presses: only emit if not already set in bitmap.
     * key_data[] contains ALL currently pressed keys (not just new ones),
     * so check the bitmap to avoid duplicate press events. */
    for (uint32_t i = 0; i < press_cnt; i++) {
        uint8_t row = pressed[i].output_index;
        uint8_t col = pressed[i].input_index;
        if (row < RF_HALF_ROWS && col < RF_HALF_COLS) {
            if (!rf_bitmap_get(bitmap, row, col)) {
                rf_bitmap_set(bitmap, row, col, true);
                if (emit_cb) emit_cb(row, col, true, ctx);
            }
        }
    }
}

#ifndef TEST_HOST

/* ── Transmit one key event — called from emit_cb ─────────── */
static void tx_key_event(uint8_t row, uint8_t col, bool key_pressed, void *ctx)
{
    (void)ctx;
    rf_key_event_t e = {
        .row      = row,
        .col      = col,
        .pressed  = key_pressed,
        .is_retry = false,
        .seq      = s_seq++,   /* post-increment; wraps 0xFF→0x00 naturally */
    };
    uint8_t buf[3];
    rf_encode_key(buf, &e);

    half_spi_lock();
    bool ok = rf_driver_send(&s_radio, buf, 3);
    half_spi_unlock();
    if (!ok) {
        /* Store for retry in next heartbeat tick */
        s_pending_retry = e;
        s_pending_retry.is_retry = true;
        s_has_pending_retry = true;
        ESP_LOGD(TAG, "TX failed row=%u col=%u pressed=%u — queued retry", row, col, (unsigned)key_pressed);
    }
}

/* ── keyboard_button callback: called from the kb_button task ── */
static void keyboard_btn_cb(keyboard_btn_handle_t kbd_handle,
                            keyboard_btn_report_t  kbd_report,
                            void                  *user_data)
{
    (void)kbd_handle;
    (void)user_data;

    /* Delegate to half_diff_emit: it updates the bitmap and calls tx_key_event
     * for each change (releases first, then new presses). */
    half_diff_emit(
        s_pressed_bitmap,
        kbd_report.key_data,         kbd_report.key_pressed_num,
        kbd_report.key_release_data, kbd_report.key_release_num,
        tx_key_event, NULL);
}

/* ── Heartbeat timer callback (100 ms periodic) ─────────────── */
static void heartbeat_timer_cb(void *arg)
{
    (void)arg;

    /* Best-effort retry for the last failed key event */
    if (s_has_pending_retry) {
        uint8_t buf[3];
        rf_key_event_t retry_evt = s_pending_retry;   /* snapshot (volatile struct copy) */
        retry_evt.seq = s_seq++;
        rf_encode_key(buf, &retry_evt);
        half_spi_lock();
        rf_driver_send(&s_radio, buf, 3);   /* no second retry on failure */
        half_spi_unlock();
        s_has_pending_retry = false;
    }

    /* Build and transmit PKT_HEARTBEAT */
    rf_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));
    memcpy(hb.bitmap, s_pressed_bitmap, RF_HALF_BITMAP_BYTES);
    hb.batt_dV = 0;   /* MVP: battery not measured */
    hb.link_q  = (uint8_t)(rf_tx_max_rt_count > 255 ? 255 : rf_tx_max_rt_count);
    hb.seq     = s_seq++;
    rf_tx_max_rt_count = 0;   /* reset link quality counter */

    uint8_t buf[9];
    rf_encode_heartbeat(buf, &hb);
    half_spi_lock();
    bool ok = rf_driver_send(&s_radio, buf, 9);
    half_spi_unlock();
    if (!ok) {
        ESP_LOGD(TAG, "heartbeat TX failed (MAX_RT)");
    }

#if CONFIG_KASE_HAS_ESPNOW
    /* Battery TX: every ~30 s (300 × 100 ms ticks) */
    static uint32_t s_batt_ticks = 0;
    if (++s_batt_ticks >= 300) {
        s_batt_ticks = 0;
        en_battery_t b = {
            .batt_dV  = 0,   /* TODO STUB: read ADC GPIO15 (battery brick, Phase 2+) */
            .soc_pct  = 0,   /* TODO STUB: derive SoC from batt_dV */
            .charging = 0,   /* TODO STUB: gpio_get_level(GPIO46) BMS status */
        };
        /* TODO STUB: load mac_dongle from NVS rf.mac_dongle and call espnow_send().
         * Until MAC is configured, log only — do not send to zero MAC. */
        ESP_LOGD(TAG, "battery stub: dV=%u soc=%u%% chg=%u (not sent — mac_dongle not configured)",
                 b.batt_dV, b.soc_pct, b.charging);
        (void)b;   /* suppress unused warning */
    }
#endif /* CONFIG_KASE_HAS_ESPNOW */
}

/* ── Main task body ─────────────────────────────────────────── */
static void half_scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "half_scan_task started");

    /* Initialize shared SPI2 bus lock (used by NRF, and by e-ink/trackpad bricks). */
    half_spi_lock_init();

    /* Reset all matrix GPIO pins to detach bootloader/UART functions.
     * This mirrors the gpio_reset_pin() calls in matrix_scan.c::matrix_setup(). */
    const int col_gpios[MATRIX_COLS] = {
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6
    };
    const int row_gpios[MATRIX_ROWS] = {
        ROWS0, ROWS1, ROWS2, ROWS3, ROWS4
    };
    for (int i = 0; i < MATRIX_COLS; i++) gpio_reset_pin(col_gpios[i]);
    for (int i = 0; i < MATRIX_ROWS; i++) gpio_reset_pin(row_gpios[i]);

    /* Initialize NRF24L01+ in PTX mode */
    rf_radio_cfg_t nrf_cfg = board_nrf_cfg();
    esp_err_t err = rf_driver_init_tx(&s_radio, &nrf_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NRF PTX init failed: %d — halting (safe: USB console OK)", err);
        vTaskDelay(portMAX_DELAY);
        return;
    }

#if CONFIG_KASE_HAS_TRACKPAD
    /* Trackpad skeleton: init I2C, RST pulse, RDY IRQ, I2C probe.
     * Returns true if the trackpad is physically present on this half. */
    bool trackpad_present = trackpad_init();
    if (trackpad_present) {
        ESP_LOGI(TAG, "trackpad detected — starting trackpad task");
        trackpad_start();
    } else {
        ESP_LOGI(TAG, "trackpad not detected on this half (no I2C ACK) — skipping");
    }
#endif /* CONFIG_KASE_HAS_TRACKPAD */

#if CONFIG_KASE_HAS_EINK
    /* E-ink skeleton: add SSD1681 SPI device, GPIO config, probe.
     * Returns true if the panel is physically present on this half.
     * Must be called AFTER rf_driver_init_tx (SPI2 bus must exist). */
    bool eink_present = eink_init();
    if (eink_present) {
        ESP_LOGI(TAG, "e-ink detected — starting refresh task");
        eink_start();
    } else {
        ESP_LOGI(TAG, "e-ink not detected on this half (BUSY timeout) — skipping");
    }
#endif /* CONFIG_KASE_HAS_EINK */

#if CONFIG_KASE_HAS_ESPNOW
    /* Initialize info-channel state BEFORE ESP-NOW link (mutex must exist before recv fires) */
    espnow_info_init();
    bool espnow_ok = espnow_link_init();
    if (!espnow_ok) {
        ESP_LOGW(TAG, "ESP-NOW init failed — info channel disabled (NRF TX continues)");
    }
#endif /* CONFIG_KASE_HAS_ESPNOW */

    /* Initialize keyboard_button matrix driver.
     * The half PCB is ROW2COL: diodes conduct ROW→COL (confirmed by raw GPIO
     * probe — driving a row HIGH makes the pressed col read HIGH). So we DRIVE
     * the rows (output) and SENSE the cols (input). This is the opposite of the
     * V1/V2 keyboards (COL2ROW), which is why the inherited config saw nothing.
     * Consequently keyboard_button's output_index = row, input_index = col. */
    static int output_gpios[MATRIX_ROWS];
    static int input_gpios[MATRIX_COLS];
    for (int i = 0; i < MATRIX_ROWS; i++) output_gpios[i] = row_gpios[i];
    for (int i = 0; i < MATRIX_COLS; i++) input_gpios[i]  = col_gpios[i];

    keyboard_btn_config_t kbd_cfg = {
        .output_gpios     = output_gpios,
        .input_gpios      = input_gpios,
        .output_gpio_num  = MATRIX_ROWS,
        .input_gpio_num   = MATRIX_COLS,
        .active_level     = 1,                        /* ROW2COL: drive row high, sense col high */
        .debounce_ticks   = BOARD_DEBOUNCE_TICKS,
        .ticks_interval   = BOARD_MATRIX_SCAN_INTERVAL_US,
        .enable_power_save = false,
        .priority         = 5,
        .core_id          = 0,
    };

    keyboard_btn_handle_t s_kbd = NULL;
    esp_err_t res = keyboard_button_create(&kbd_cfg, &s_kbd);
    if (res != ESP_OK || s_kbd == NULL) {
        ESP_LOGE(TAG, "keyboard_button_create failed: %d", res);
        vTaskDelay(portMAX_DELAY);
        return;
    }

    keyboard_btn_cb_config_t cb_cfg = {
        .event     = KBD_EVENT_PRESSED,   /* fires on any matrix change */
        .callback  = keyboard_btn_cb,
        .user_data = NULL,
    };
    res = keyboard_button_register_cb(s_kbd, cb_cfg, NULL);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "keyboard_button_register_cb failed: %d", res);
    }

    /* Start heartbeat timer — 100 ms periodic */
    esp_timer_handle_t hb_timer;
    esp_timer_create_args_t timer_args = {
        .callback        = heartbeat_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "half_hb",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &hb_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(hb_timer, 100 * 1000));   /* 100 ms in µs */

    ESP_LOGI(TAG, "matrix + NRF PTX + heartbeat timer running");

    /* Task is event-driven via callbacks; block indefinitely */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

/* ── Public API ─────────────────────────────────────────────── */
void half_scan_task_start(void)
{
    ESP_LOGI(TAG, "creating half_scan_task (prio 10, stack 4KB, core 0)");
    BaseType_t ret = xTaskCreatePinnedToCore(
        half_scan_task, "half_scan",
        4096,   /* stack: 4 KB (no large buffers; NRF SPI max 33 bytes) */
        NULL, 10, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed: %d", (int)ret);
    }
}

#endif /* TEST_HOST */
