#ifndef RF_DRIVER_H
#define RF_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

typedef struct {
    spi_host_device_t spi_host;
    int pin_mosi, pin_miso, pin_sck, pin_csn, pin_ce, pin_irq;
    int clock_hz;
    uint8_t channel;
    uint8_t rx_addr[4];     /* base, 4 bytes */
    uint8_t addr_suffix;    /* 5th address byte (0x01=L, 0x02=R) */
    bool shares_bus_first;  /* true → this radio calls spi_bus_initialize */
} rf_radio_cfg_t;

typedef struct {
    rf_radio_cfg_t cfg;
    spi_device_handle_t spi;
    bool present;           /* chip register sanity passed */
    void *irq_sem;          /* SemaphoreHandle_t, set by consumer */
    uint32_t pkt_rx;        /* diagnostics counters */
    uint32_t pkt_dup;
    uint32_t fifo_ovf;
} rf_radio_t;

/* Initialize SPI (if shares_bus_first), the chip, RX mode (PRX), channel,
 * address, ESB + DPL. Returns ESP_OK; sets radio->present. */
esp_err_t rf_driver_init(rf_radio_t *radio, const rf_radio_cfg_t *cfg);

/* Sanity check: read CONFIG/RF_SETUP back, verify not all-0x00/0xFF. */
bool rf_driver_probe(rf_radio_t *radio);

/* Attach an IRQ semaphore: the GPIO ISR gives it on falling edge of IRQ pin. */
void rf_radio_set_irq_sem(rf_radio_t *radio, void *sem);

/* Read one RX payload (DPL). Returns length (0 if FIFO empty). Clears RX_DR. */
uint16_t rf_driver_read_rx(rf_radio_t *radio, uint8_t *buf, uint16_t maxlen);

/* True if RX FIFO has data pending (FIFO_STATUS). */
bool rf_driver_rx_available(rf_radio_t *radio);

/* Register access (exposed for probe/diagnostics). */
uint8_t rf_driver_read_reg(rf_radio_t *radio, uint8_t reg);
void    rf_driver_write_reg(rf_radio_t *radio, uint8_t reg, uint8_t val);

/* Change channel at runtime (Plan 4 pairing). */
void rf_driver_set_channel(rf_radio_t *radio, uint8_t ch);

/* Reprogram the live PRX pipe-0 RX address (5 bytes) without re-init.
 * ce_low → write REG_RX_ADDR_P0 → ce_high. Used by the dongle pairing hot-switch. */
void rf_driver_set_rx_address(rf_radio_t *r, const uint8_t addr[5]);

/* Out-of-band one-shot PTX for the dongle PKT_PAIR_ACK (spec §5.4).
 * Switches to PTX on ch+addr, transmits payload once (CE pulse, poll TX_DS/MAX_RT),
 * then restores PRX on restore_ch+restore_addr and re-asserts CE high. Returns TX_DS.
 * Compiled in both roles (no KASE_HAS_RF_TX guard). */
bool rf_driver_oob_tx(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                      const uint8_t *payload, uint8_t len,
                      uint8_t restore_ch, const uint8_t restore_addr[5]);

/* ── PTX mode — compiled only when KASE_HAS_RF_TX=y ─────────────── */
#if CONFIG_KASE_HAS_RF_TX

/* Initialize the radio in PTX mode (transmitter).
 * Sets TX_ADDR + RX_ADDR_P0 to the same 5-byte address (required for ESB auto-ACK).
 * Channel: cfg->channel. Data rate: 2 Mbps, 0 dBm, ARC=3, ARD=500 µs, DPL pipe 0.
 * cfg->shares_bus_first=true → initializes the SPI bus (set true for the single half radio).
 * Returns ESP_OK on success; sets radio->present to true. */
esp_err_t rf_driver_init_tx(rf_radio_t *radio, const rf_radio_cfg_t *cfg);

/* Transmit one payload (PTX, polled).
 * Writes W_TX_PAYLOAD, pulses CE high ~15 µs, polls STATUS until TX_DS (ACK received)
 * or MAX_RT (3 retries exhausted). Clears IRQ flags. Flushes TX FIFO on MAX_RT.
 * Timeout ~5 ms (ARC=3 × ARD=500 µs × 2 + margin).
 * Returns true on TX_DS (ACK from dongle). */
bool rf_driver_send(rf_radio_t *radio, const uint8_t *buf, uint8_t len);

/* Count of MAX_RT events accumulated since last reset.
 * half_scan_task reads this to fill PKT_HEARTBEAT.link_q, then clears it. */
extern uint32_t rf_tx_max_rt_count;

/* Reprogram the half's TX address (REG_TX_ADDR + REG_RX_ADDR_P0, 5 bytes, CE low).
 * Used to retarget the half radio to RF_PAIR_ADDR for the PKT_PAIR_REQ burst. */
void rf_driver_set_tx_address(rf_radio_t *r, const uint8_t addr[5]);

/* Pairing-only: switch the half radio to PRX on ch+addr, wait up to timeout_ms for
 * one RX payload (PKT_PAIR_ACK), copy to buf (max maxlen). Returns length (0 on
 * timeout). Leaves radio in PRX — caller restores PTX via set_tx_address. */
uint16_t rf_driver_pair_listen(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                               uint8_t *buf, uint16_t maxlen, uint32_t timeout_ms);

#endif /* CONFIG_KASE_HAS_RF_TX */

#endif /* RF_DRIVER_H */
