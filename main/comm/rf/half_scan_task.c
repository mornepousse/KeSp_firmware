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
#include "rf_pairing.h"      /* rf_pairing_load_set_id_half / rf_apply_set_id */
#include "half_power.h"      /* half_power_next / half_power_hb_divisor */
#include "half_sleep.h"      /* half_sleep_enter */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_mac.h"         /* esp_read_mac, ESP_MAC_WIFI_STA */
#include "esp_system.h"      /* esp_restart */
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
#include "eink_lvgl.h"   /* eink_lvgl_show_paired() — pairing confirmation splash */
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

/* ── BOOT-button pairing trigger (GPIO0, active-low) ── */
#define HALF_BOOT_GPIO        GPIO_NUM_0
#define HALF_BOOT_HOLD_TICKS  30      /* 30 × 100 ms heartbeat ticks ≈ 3 s */
static volatile bool s_pairing_active = false;

/* ── Local pressed-key bitmap (maintained for PKT_HEARTBEAT) ── */
static uint8_t s_pressed_bitmap[RF_HALF_BITMAP_BYTES];

/* Last keyboard activity (ms, esp_timer). Drives the power state machine. */
static volatile uint32_t s_last_activity_ms = 0;

/* ── Sleep/wake plumbing: handles + configs kept at file scope ─ */
static esp_timer_handle_t       s_hb_timer  = NULL;
static keyboard_btn_handle_t    s_kbd       = NULL;
static keyboard_btn_config_t    s_kbd_cfg;        /* kept for recreate after wake */
static keyboard_btn_cb_config_t s_kbd_cb_cfg;     /* kept for re-register after wake */

/* ── Retry state: one pending retry stored on MAX_RT ──────────── */
static volatile bool           s_has_pending_retry = false;
static volatile rf_key_event_t s_pending_retry;

/* ── Link quality counter (MAX_RT events since last heartbeat) ─ */
/* rf_tx_max_rt_count is the global in rf_driver.c (PTX section). */
extern uint32_t rf_tx_max_rt_count;

/* ── 10 s console status accounting (printed from heartbeat_timer_cb) ────────
 * A "half_stat" line every 10 s on the debug UART: identity + TX health (ACK %
 * of heartbeats reaching the dongle) + dongle-side view + heap + presence.
 * Window counters reset each print → values are "over the last 10 s". */
static uint16_t s_stat_set_id      = 0;
static uint8_t  s_stat_slot        = 0;
static bool     s_trackpad_present = false;
static bool     s_eink_present     = false;
static uint32_t s_stat_hb_total    = 0;   /* heartbeats sent this window  */
static uint32_t s_stat_hb_ok       = 0;   /* heartbeats ACKed this window */
static uint32_t s_stat_maxrt       = 0;   /* MAX_RT events this window    */
static uint8_t  s_stat_last_lq     = 0;   /* last link_q (retry %)        */

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

    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* Delegate to half_diff_emit: it updates the bitmap and calls tx_key_event
     * for each change (releases first, then new presses). */
    half_diff_emit(
        s_pressed_bitmap,
        kbd_report.key_data,         kbd_report.key_pressed_num,
        kbd_report.key_release_data, kbd_report.key_release_num,
        tx_key_event, NULL);
}

/* ── Half pairing task: REQ loop on the rendezvous, await PKT_PAIR_ACK ── */
static void half_pairing_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pairing: window open (30 s) — sending PKT_PAIR_REQ");

    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    /* Declare our own slot (board identity: half_left=0x01, half_right=0x02) so
     * the dongle assigns L/R by identity, not by pairing order. */
    uint8_t req[8];
    rf_encode_pair_req(req, my_mac, BOARD_NRF_ADDR_SUFFIX);

    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + 30000;
    bool acked = false;
    rf_pair_ack_t ack;

    while (!acked && (uint32_t)(esp_timer_get_time() / 1000) < deadline) {
        /* Burst one REQ as PTX on the rendezvous. */
        half_spi_lock();
        rf_driver_set_tx_address(&s_radio, pair_addr);
        rf_driver_set_channel(&s_radio, RF_PAIR_CHANNEL);
        rf_driver_send(&s_radio, req, 8);
        half_spi_unlock();

        /* Listen ~150 ms for the ACK as PRX on the rendezvous. */
        uint8_t rxb[32];
        half_spi_lock();
        uint16_t n = rf_driver_pair_listen(&s_radio, RF_PAIR_CHANNEL, pair_addr,
                                           rxb, sizeof(rxb), 150);
        half_spi_unlock();
        if (n && rf_decode_pair_ack(rxb, n, &ack)) {
            acked = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));   /* ~200 ms REQ cadence (spec §3.3) */
    }

    if (acked) {
        ESP_LOGI(TAG, "pairing: ACK set_id=0x%04X slot=0x%02X — saving NVS + reboot",
                 ack.set_id, ack.slot);
        rf_pairing_save_half(ack.set_id, ack.slot, ack.dongle_wifi_mac);
#if CONFIG_KASE_HAS_EINK
        /* Visual confirmation on the e-ink (no-op on the half without a panel).
         * The radio TX is already done (ACK received) so the SPI bus is free for
         * the eink refresh. */
        eink_lvgl_show_paired(ack.set_id, ack.slot);
#endif
        vTaskDelay(pdMS_TO_TICKS(3000));   /* let splash render (~1.5s) + stay visible, then reboot */
        esp_restart();
    } else {
        ESP_LOGW(TAG, "pairing: timed out (no ACK) — restoring normal TX");
        /* Restore the half's data address/channel from NVS (or factory). */
        uint8_t slot = BOARD_NRF_ADDR_SUFFIX;
        uint16_t set_id = rf_pairing_load_set_id_half(BOARD_NRF_ADDR_SUFFIX, &slot);
        rf_radio_cfg_t cfg = board_nrf_cfg();
        rf_apply_set_id(&cfg, set_id, slot);
        uint8_t addr[5] = { cfg.rx_addr[0], cfg.rx_addr[1], cfg.rx_addr[2],
                            cfg.rx_addr[3], cfg.addr_suffix };
        half_spi_lock();
        rf_driver_set_tx_address(&s_radio, addr);
        rf_driver_set_channel(&s_radio, cfg.channel);
        half_spi_unlock();
        s_pairing_active = false;
    }
    vTaskDelete(NULL);
}

/* ── half_status_print() — one "half_stat" line for the debug console ────────
 * Called every 10 s from heartbeat_timer_cb. Window counters reset after. */
static void half_status_print(void)
{
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t ack  = s_stat_hb_total ? (s_stat_hb_ok * 100u / s_stat_hb_total) : 0;
    char slot_c   = (s_stat_slot == 0x02) ? 'R' : 'L';

    char lay[17] = "-";
    int  alive = 0, usb = 0, sl = 0, sr = 0;
#if CONFIG_KASE_HAS_ESPNOW
    if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        memcpy(lay, g_half_state.layer_name, 16); lay[16] = '\0';
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        alive = (now - g_half_state.last_status_ms) < 15000u;
        usb   = g_half_state.usb_active ? 1 : 0;
        sl    = g_half_state.sig_left;
        sr    = g_half_state.sig_right;
        xSemaphoreGive(g_half_state_mutex);
    }
    if (lay[0] == '\0') { lay[0] = '-'; lay[1] = '\0'; }
#endif

    ESP_LOGI("half_stat",
        "up=%lus slot=%c set=0x%04X | TX hb=%lu ack=%lu%% maxrt=%lu lq=%u%% | "
        "DONGLE alive=%d lay=%s usb=%d L=%d R=%d | heap=%luK tp=%d eink=%d",
        (unsigned long)up_s, slot_c, s_stat_set_id,
        (unsigned long)s_stat_hb_total, (unsigned long)ack,
        (unsigned long)s_stat_maxrt, (unsigned)s_stat_last_lq,
        alive, lay, usb, sl, sr,
        (unsigned long)(esp_get_free_heap_size() / 1024u),
        s_trackpad_present, s_eink_present);

    s_stat_hb_total = 0;
    s_stat_hb_ok    = 0;
    s_stat_maxrt    = 0;
}

/* ── Heartbeat timer callback (100 ms periodic) ─────────────── */
static void heartbeat_timer_cb(void *arg)
{
    (void)arg;

    /* Power state -> heartbeat throttle. The 100 ms timer keeps firing (cheap);
     * we only TX the heartbeat every Nth tick when idle, cutting NRF TX. Key
     * events (event-driven) are unaffected -> typing latency unchanged. */
    static uint32_t s_hb_tick = 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    half_power_state_t pstate = half_power_next(s_last_activity_ms, now_ms);
    uint8_t hb_div = half_power_hb_divisor(pstate);
    bool emit_hb = (s_hb_tick % hb_div) == 0;
    s_hb_tick++;

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

    /* Build and transmit PKT_HEARTBEAT — gated by power state */
    if (emit_hb) {
        rf_heartbeat_t hb;
        memset(&hb, 0, sizeof(hb));
        memcpy(hb.bitmap, s_pressed_bitmap, RF_HALF_BITMAP_BYTES);
        hb.batt_dV = 0;   /* MVP: battery not measured */
        /* link_q = retransmit percentage (0..100): Σ ARC_CNT × 100 / (tx_count × 3).
         * 0 = pristine, 100 = every packet maxing all 3 retries. Catches a degrading
         * link before packets are fully lost. */
        {
            uint32_t txc = rf_tx_count, rsum = rf_tx_retr_sum;
            hb.link_q = (txc > 0) ? (uint8_t)((rsum * 100u) / (txc * 3u)) : 0;
            rf_tx_retr_sum = 0;
            rf_tx_count    = 0;
        }
        s_stat_last_lq = hb.link_q;
        hb.seq     = s_seq++;
        s_stat_maxrt += rf_tx_max_rt_count;   /* accumulate for the 10 s status */
        rf_tx_max_rt_count = 0;   /* still cleared (used elsewhere for debug) */

        uint8_t buf[9];
        rf_encode_heartbeat(buf, &hb);
        half_spi_lock();
        bool ok = rf_driver_send(&s_radio, buf, 9);
        half_spi_unlock();
        if (!ok) {
            ESP_LOGD(TAG, "heartbeat TX failed (MAX_RT)");
        }

        s_stat_hb_total++;
        if (ok) s_stat_hb_ok++;
    }

    /* ── 10 s console status (100 × 100 ms ticks) ─────────────── */
    static uint32_t s_stat_ticks = 0;
    if (++s_stat_ticks >= 100) {
        s_stat_ticks = 0;
        half_status_print();
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

    /* BOOT-button pairing trigger: spawn the pairing task after a ~3 s hold. */
    static uint8_t s_boot_low_ticks = 0;
    if (!s_pairing_active) {
        if (gpio_get_level(HALF_BOOT_GPIO) == 0) {   /* active-low: pressed */
            if (++s_boot_low_ticks >= HALF_BOOT_HOLD_TICKS) {
                s_boot_low_ticks = 0;
                s_pairing_active = true;
                xTaskCreatePinnedToCore(half_pairing_task, "half_pair",
                                        4096, NULL, 4, NULL, 0);
            }
        } else {
            s_boot_low_ticks = 0;   /* released early — reset hold counter */
        }
    }
}

/* ── Main task body ─────────────────────────────────────────── */
static void half_scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "half_scan_task started");

    /* Initialize shared SPI2 bus lock (used by NRF, and by e-ink/trackpad bricks). */
    half_spi_lock_init();

    /* BOOT button (GPIO0) as input with pull-up — pairing trigger (held 3 s). */
    gpio_config_t boot_cfg = {
        .pin_bit_mask = (1ULL << HALF_BOOT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,   /* polled in heartbeat_timer_cb */
    };
    gpio_config(&boot_cfg);

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

    /* Per-set addressing (Plan RF-1): if this half is paired (NVS rf.set_id set),
     * derive its address+channel from the stored set_id and slot. If unpaired,
     * rf_pairing_load_set_id_half() returns 0 with *slot = BOARD_NRF_ADDR_SUFFIX,
     * and rf_apply_set_id is a no-op → nrf_cfg keeps the board factory defaults
     * (KaSe.<suffix>, ch from BOARD_NRF_CHANNEL). */
    uint8_t slot = BOARD_NRF_ADDR_SUFFIX;
    uint16_t set_id = rf_pairing_load_set_id_half(BOARD_NRF_ADDR_SUFFIX, &slot);
    rf_apply_set_id(&nrf_cfg, set_id, slot);
    s_stat_set_id = set_id;   /* for the 10 s status line */
    s_stat_slot   = slot;

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
    s_trackpad_present = trackpad_present;   /* for the 10 s status line */
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
    s_eink_present = eink_present;   /* for the 10 s status line */
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

    s_kbd_cfg = (keyboard_btn_config_t){
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

    esp_err_t res = keyboard_button_create(&s_kbd_cfg, &s_kbd);
    if (res != ESP_OK || s_kbd == NULL) {
        ESP_LOGE(TAG, "keyboard_button_create failed: %d", res);
        vTaskDelay(portMAX_DELAY);
        return;
    }

    s_kbd_cb_cfg = (keyboard_btn_cb_config_t){
        .event     = KBD_EVENT_PRESSED,   /* fires on any matrix change */
        .callback  = keyboard_btn_cb,
        .user_data = NULL,
    };
    res = keyboard_button_register_cb(s_kbd, s_kbd_cb_cfg, NULL);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "keyboard_button_register_cb failed: %d", res);
    }

    /* Start heartbeat timer — 100 ms periodic */
    esp_timer_create_args_t timer_args = {
        .callback        = heartbeat_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "half_hb",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_hb_timer));
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_hb_timer, 100 * 1000));   /* 100 ms in µs */

    ESP_LOGI(TAG, "matrix + NRF PTX + heartbeat timer running");

    /* Task is event-driven via callbacks; poll for SLEEP state every 500 ms */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (half_power_next(s_last_activity_ms, now) == HALF_POWER_SLEEP) {
            half_sleep_enter();   /* blocks: quiesce -> (placeholder) -> restore */
        }
    }
}

/* ── Sleep/wake helpers ─────────────────────────────────────── */
void half_scan_stop_for_sleep(void)
{
    if (s_hb_timer) esp_timer_stop(s_hb_timer);
    if (s_kbd) { keyboard_button_delete(s_kbd); s_kbd = NULL; }
}

void half_scan_restart_after_wake(void)
{
    esp_err_t r = keyboard_button_create(&s_kbd_cfg, &s_kbd);
    if (r == ESP_OK && s_kbd) {
        keyboard_button_register_cb(s_kbd, s_kbd_cb_cfg, NULL);
    } else {
        /* Best-effort: heartbeat still restarts below; keys won't fire until the
         * next wake cycle recreates the scan. Surface it for debugging. */
        ESP_LOGE(TAG, "restart_after_wake: keyboard_button_create failed: %d", r);
        s_kbd = NULL;
    }
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_hb_timer) esp_timer_start_periodic(s_hb_timer, 100 * 1000);
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
