/*
 * kbd_relay_tx.c — Smart keyboard NRF24 HID relay (PTX to dongle).
 *
 * Mirrors the radio-init + pairing-restore flow from half_scan_task.c, adapted
 * for a standalone keyboard that sends final HID reports (PKT_TYPE_HIDREPORT)
 * instead of raw matrix events.  No shared-bus lock needed: the keyboard has no
 * other device sharing the NRF SPI bus.
 *
 * Compiled only when CONFIG_KASE_KBD_WIRELESS=y (CMakeLists.txt guard).
 */

#include "kbd_relay_tx.h"
#include "rf_driver.h"
#include "rf_packet.h"
#include "rf_pairing.h"
#include "board.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"        /* esp_read_mac */
#include "esp_system.h"     /* esp_restart */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kbd_relay";

/* ── Fallback NRF pin config ────────────────────────────────────────────────
 * Keyboard boards (V2, V2D) do not define BOARD_NRF_* — those are on the half
 * and dongle board headers.  The fallbacks here let the code compile and link
 * on any board; a real wireless-keyboard board.h would override them.
 * GPIO numbers are chosen from the free pool on the V2 PCB (no conflicts with
 * matrix, I2C display, or USB).
 * ─────────────────────────────────────────────────────────────────────────── */
#ifndef BOARD_NRF_SPI_HOST
#define BOARD_NRF_SPI_HOST      SPI2_HOST
#define BOARD_NRF_SPI_MOSI      GPIO_NUM_35
#define BOARD_NRF_SPI_MISO      GPIO_NUM_37
#define BOARD_NRF_SPI_SCK       GPIO_NUM_36
#define BOARD_NRF_SPI_CLOCK_HZ  (8 * 1000 * 1000)
#define BOARD_NRF_CSN_GPIO      GPIO_NUM_34
#define BOARD_NRF_CE_GPIO       GPIO_NUM_33
#define BOARD_NRF_IRQ_GPIO      GPIO_NUM_38
#define BOARD_NRF_CHANNEL       76
/* Slot 0x03 distinguishes the smart keyboard from half-left (0x01) / half-right (0x02). */
#define BOARD_NRF_ADDR_SUFFIX   0x03
#endif

/* ── Module state ───────────────────────────────────────────────────────── */

static rf_radio_t s_radio;
static bool s_paired = false;

/* ── Radio config helper (mirrors board_nrf_cfg in half_scan_task.c) ─────── */

static rf_radio_cfg_t kbd_nrf_cfg(void)
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
        .addr_suffix      = BOARD_NRF_ADDR_SUFFIX,
        .shares_bus_first = true,   /* keyboard has exactly one NRF radio */
    };
    return c;
}

/* ── Active pairing task (mirrors half_pairing_task; no SPI-bus lock) ──────
 * Sends PKT_PAIR_REQ on the rendezvous declaring this device as a smart
 * keyboard, awaits PKT_PAIR_ACK, saves the assigned set_id/slot to NVS, then
 * reboots so kbd_relay_init() comes up paired (relay active). Runs only while
 * unpaired; the dongle's pairing window must be open (KS_CMD_RF_PAIR_START). */
static void kbd_pairing_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pairing: window open (30 s) — sending PKT_PAIR_REQ (smart kbd)");

    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    /* 8-byte REQ, byte-identical to a half's (the dongle pairing RX expects this
     * width). Device-type defaults to dumb-half on the dongle; that's fine for
     * the typing path (the dongle routes by PACKET TYPE, HIDREPORT vs KEY). */
    uint8_t req[8];
    rf_encode_pair_req(req, my_mac, BOARD_NRF_ADDR_SUFFIX);

    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + 30000;
    bool acked = false;
    rf_pair_ack_t ack;

    while (!acked && (uint32_t)(esp_timer_get_time() / 1000) < deadline) {
        rf_driver_set_tx_address(&s_radio, pair_addr);
        rf_driver_set_channel(&s_radio, RF_PAIR_CHANNEL);
        rf_driver_send(&s_radio, req, 8);

        uint8_t rxb[32];
        uint16_t n = rf_driver_pair_listen(&s_radio, RF_PAIR_CHANNEL, pair_addr,
                                           rxb, sizeof(rxb), 150);
        if (n && rf_decode_pair_ack(rxb, n, &ack)) { acked = true; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (acked) {
        ESP_LOGI(TAG, "pairing: ACK set_id=0x%04X slot=0x%02X — saving NVS + reboot",
                 ack.set_id, ack.slot);
        rf_pairing_save_half(ack.set_id, ack.slot, ack.dongle_wifi_mac);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGW(TAG, "pairing: timed out (no ACK) — relay stays inactive");
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void kbd_relay_init(void)
{
    s_paired = false;

    rf_radio_cfg_t nrf_cfg = kbd_nrf_cfg();

    /* Load pairing state from NVS (mirrors half_scan_task.c init).
     * rf_pairing_load_set_id_half returns 0 if no NVS entry (unpaired);
     * rf_apply_set_id is a no-op for set_id 0/0xFFFF → cfg keeps board defaults. */
    uint8_t slot = BOARD_NRF_ADDR_SUFFIX;
    uint16_t set_id = rf_pairing_load_set_id_half(BOARD_NRF_ADDR_SUFFIX, &slot);
    rf_apply_set_id(&nrf_cfg, set_id, slot);

    esp_err_t err = rf_driver_init_tx(&s_radio, &nrf_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NRF PTX init failed (%d) — wireless relay disabled", err);
        return;   /* s_paired stays false */
    }

    if (set_id == 0 || set_id == 0xFFFF) {
        /* Unpaired: spawn the active pairing task. It REQs on the rendezvous and,
         * on ACK, saves NVS + reboots (relay comes up active). Open the dongle's
         * pairing window (KS_CMD_RF_PAIR_START) to complete it. */
        ESP_LOGW(TAG, "kbd_relay: unpaired — starting pairing task (open the dongle window)");
        xTaskCreate(kbd_pairing_task, "kbd_pair", 4096, NULL, 5, NULL);
        return;   /* s_paired becomes true after the post-pairing reboot */
    }

    ESP_LOGI(TAG, "kbd_relay: paired set_id=0x%04X slot=0x%02X — relay active",
             set_id, slot);
    s_paired = true;
}

bool kbd_relay_active(void)
{
    return s_paired;
}

void kbd_relay_send_kbd(uint8_t modifier, const uint8_t kb[6])
{
    uint8_t buf[9];
    rf_encode_hidreport_kbd(buf, modifier, kb);
    rf_driver_send(&s_radio, buf, 9);
}

void kbd_relay_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    uint8_t buf[6];
    rf_encode_hidreport_mouse(buf, buttons, x, y, wheel);
    rf_driver_send(&s_radio, buf, 6);
}
