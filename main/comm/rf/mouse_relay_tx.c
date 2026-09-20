/* See mouse_relay_tx.h. Modeled on comm/rf/kbd_relay_tx.c for the pairing
 * sequence, but without anything belonging to the keyboard. */

#include "mouse_relay_tx.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "board.h"
#include "rf_driver.h"
#include "rf_pairing.h"
#include "rf_packet.h"

static const char *TAG = "mouse_rf";

static rf_radio_t s_radio;
static bool s_paired;
static uint32_t s_tx, s_tx_ack;

/* Conchodytes radio configuration.
 *
 * WARNING: `shares_bus_first = false` — and this is not a detail. The SPI
 * bus is SHARED with the PMW3389, and it is `pmw3389_init()` that calls
 * `spi_bus_initialize`. Setting `true` here would make a second call on a
 * bus already initialized. */
static rf_radio_cfg_t mouse_nrf_cfg(void)
{
    rf_radio_cfg_t c = {
        .spi_host         = BOARD_NRF_SPI_HOST,
        .pin_mosi         = BOARD_NRF_MOSI,
        .pin_miso         = BOARD_NRF_MISO,
        .pin_sck          = BOARD_NRF_SCK,
        .clock_hz         = BOARD_NRF_CLOCK_HZ,
        .pin_csn          = BOARD_NRF_CSN,
        .pin_ce           = BOARD_NRF_CE,
        .pin_irq          = BOARD_NRF_IRQ,
        .channel          = BOARD_NRF_CHANNEL,
        .rx_addr          = { 'K', 'a', 'S', 'e' },   /* 4-byte base */
        .addr_suffix      = BOARD_NRF_ADDR_SUFFIX,    /* 0x02 = mouse slot */
        .shares_bus_first = false,                    /* the sensor already did it */
    };
    return c;
}

esp_err_t mouse_relay_init(void)
{
    rf_radio_cfg_t cfg = mouse_nrf_cfg();

    /* Restores pairing BEFORE initializing the radio: the set_id determines
     * the working address and channel, which are not those of the rendezvous. */
    uint8_t slot = BOARD_NRF_ADDR_SUFFIX;
    uint16_t set_id = rf_pairing_load_set_id_half(BOARD_NRF_ADDR_SUFFIX, &slot);
    if (set_id) {
        rf_apply_set_id(&cfg, set_id, slot);
        ESP_LOGI(TAG, "pairing restored: set_id=0x%04X slot=0x%02X", set_id, slot);
    } else {
        ESP_LOGW(TAG, "unpaired — reports will be dropped. Open the "
                      "dongle window (KS_CMD_RF_PAIR_START) then pair.");
    }

    esp_err_t err = rf_driver_init_tx(&s_radio, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rf_driver_init_tx : %s", esp_err_to_name(err));
        return err;
    }
    if (!s_radio.present) {
        /* The probe reads CONFIG/RF_SETUP and rejects all-zero as well as
         * all-one. On this board, the 0xFF meant "module not
         * powered" for hours — a cold solder joint on its 3.3 V. */
        ESP_LOGE(TAG, "the radio does not respond: incoherent registers. "
                      "Check the power supply of module U8.");
        return ESP_ERR_NOT_FOUND;
    }

    /* WARNING: ZERO RETRANSMISSION — especially NOT the driver's 0x1F (ARD=500 us,
     * ARC=15, ~13 ms worst case), tailored for the KEYBOARD whose state is ABSOLUTE.
     *
     * Here reports go out at 1 kHz and carry a RELATIVE displacement, hence
     * NON-IDEMPOTENT. The two sides of the trade-off are asymmetric:
     *   - a LOST frame costs 1 ms of gesture, made invisible by the next one;
     *   - a DUPLICATED frame applies the same displacement twice, and it
     *     shows — the cursor jumps.
     * And that is exactly what a retry whose ACK alone was lost produces:
     * the dongle receives the same frame twice. Measured on the bench on
     * 2026-08-26 with ARC=1: 903 frames sent for 1018 accepted on the dongle side.
     *
     * As a bonus, `rf_driver_send()` now only blocks for the duration of one
     * attempt, which fits within the 1 ms report period. */
    rf_driver_set_retr(&s_radio, 0x00);

    s_paired = (set_id != 0);
    ESP_LOGI(TAG, "radio ready, channel 0x%02X, %s",
             cfg.channel, s_paired ? "paired" : "NOT paired");

    return ESP_OK;
}

bool mouse_relay_active(void)
{
    return s_paired && s_radio.present;
}

bool mouse_relay_send(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!mouse_relay_active()) return false;

    uint8_t buf[6];
    uint16_t n = rf_encode_hidreport_mouse(buf, buttons, dx, dy, wheel);
    /* Never retransmitted: the displacement is RELATIVE, hence not
     * idempotent. Replaying a report would advance the cursor a second
     * time. This is the fundamental difference with the keyboard relay,
     * whose state is absolute and refreshes without risk. A lost frame costs
     * a few counts of movement; a replayed frame makes the cursor jump. */
    /* Serializes the whole exchange against the sensor, which shares this bus:
     * `rf_driver_send()` writes the payload, pulses CE, then polls STATUS —
     * several transactions separated by waits.
     *
     * WARNING: this is NOT enough to make two devices with different SPI
     * MODES coexist, and did not fix anything about the dead link measured
     * on 2026-08-26: the cause was the mode switch under an already-lowered
     * CSN, fixed in `rf_driver.c` (see `spi_parquer_mode`). It is kept
     * because serializing remains correct, not because it healed anything. */
    spi_device_acquire_bus(s_radio.spi, portMAX_DELAY);
    s_tx++;
    bool ok = rf_driver_send(&s_radio, buf, (uint8_t)n);
    if (ok) s_tx_ack++;
    spi_device_release_bus(s_radio.spi);
    return ok;
}

void mouse_relay_stats(uint32_t *envoyes, uint32_t *acquittes)
{
    if (envoyes)   *envoyes   = s_tx;
    if (acquittes) *acquittes = s_tx_ack;
}

esp_err_t mouse_relay_pair(void)
{
    if (!s_radio.present) return ESP_ERR_INVALID_STATE;

    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);

    /* v2 request: it carries the device type in addition to the slot. The
     * dongle honors the declared slot (rf_pairing_resolve_slot), so the
     * mouse gets slot 2 regardless of pairing order. */
    uint8_t req[9];
    uint16_t reqlen = rf_encode_pair_req2(req, my_mac, BOARD_NRF_ADDR_SUFFIX,
                                          RF_DEV_MOUSE);

    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;
    rf_radio_cfg_t cfg = mouse_nrf_cfg();

    ESP_LOGI(TAG, "pairing: sending on rendezvous channel 0x%02X",
             RF_PAIR_CHANNEL);

    for (int essai = 1; essai <= 20; essai++) {
        rf_driver_set_channel(&s_radio, RF_PAIR_CHANNEL);
        rf_driver_set_tx_address(&s_radio, pair_addr);
        bool ack = rf_driver_send(&s_radio, req, (uint8_t)reqlen);

        uint8_t rep[16];
        uint16_t n = rf_driver_pair_listen(&s_radio, RF_PAIR_CHANNEL, pair_addr,
                                           rep, sizeof(rep), 300);
        if (n) {
            rf_pair_ack_t a;
            if (rf_decode_pair_ack(rep, n, &a)) {
                ESP_LOGI(TAG, "ACK received: set_id=0x%04X slot=0x%02X dongle=%02X:%02X:%02X:%02X:%02X:%02X",
                         a.set_id, a.slot, a.dongle_wifi_mac[0], a.dongle_wifi_mac[1],
                         a.dongle_wifi_mac[2], a.dongle_wifi_mac[3],
                         a.dongle_wifi_mac[4], a.dongle_wifi_mac[5]);
                esp_err_t e = rf_pairing_save_half(a.set_id, a.slot, a.dongle_wifi_mac);
                if (e != ESP_OK) {
                    ESP_LOGE(TAG, "NVS save: %s", esp_err_to_name(e));
                    return e;
                }
                ESP_LOGW(TAG, "paired. Restart for the link to activate.");
                return ESP_OK;
            }
            ESP_LOGW(TAG, "response of %u bytes, but it is not a PAIR_ACK", n);
        }

        /* `ack` is the nRF's TX_DS: it says that SOMEONE acknowledged the
         * frame at the ESB level, not that the dongle understood it.
         * Distinguishing the two helps diagnosis — no ACK at all points to
         * range, channel or address; ACK but no reply means the dongle's
         * window is closed. */
        ESP_LOGI(TAG, "attempt %d/20: %s, no PAIR_ACK",
                 essai, ack ? "frame acked at radio level" : "no acknowledgment");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Puts the radio back on its working address before returning. */
    rf_driver_set_channel(&s_radio, cfg.channel);
    ESP_LOGE(TAG, "pairing failed after 20 attempts. Is the dongle window "
                  "open (KS_CMD_RF_PAIR_START)?");
    return ESP_ERR_TIMEOUT;
}
