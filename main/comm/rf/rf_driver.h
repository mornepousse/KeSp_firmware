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
/* SETUP_RETR rule (0x04): high nibble = ARD, steps of 250 µs minus one;
 * low nibble = ARC, number of retransmissions.
 *
 * WARNING: the driver initializes 0x1F (ARD=500 µs, ARC=15), sized for the
 * KEYBOARD whose state is ABSOLUTE: resending a lost keystroke is always
 * right, and waiting is worth it. For a RELATIVE movement it is the
 * opposite — the replacement frame arrives before the retransmission
 * succeeds, and meanwhile `rf_driver_send()` blocks its caller.
 *
 * `rf_driver_send()` derives its polling delay from this register: lowering
 * it automatically shortens the wait, there is no second constant to
 * keep in sync. */
void rf_driver_set_retr(rf_radio_t *radio, uint8_t setup_retr);

uint8_t rf_driver_read_reg(rf_radio_t *radio, uint8_t reg);
void    rf_driver_write_reg(rf_radio_t *radio, uint8_t reg, uint8_t val);

/* Change channel at runtime (Plan 4 pairing). */
void rf_driver_set_channel(rf_radio_t *radio, uint8_t ch);

/* Reprogram the live PRX pipe-0 RX address (5 bytes) without re-init.
 * ce_low → write REG_RX_ADDR_P0 → ce_high. Used by the dongle pairing hot-switch. */
void rf_driver_set_rx_address(rf_radio_t *r, const uint8_t addr[5]);

/* Re-assert the full PRX RX config on a live radio (no SPI re-add). Un-wedges an
 * NRF that stopped ACKing/receiving over time. Used by the dongle radio watchdog. */
void rf_driver_rearm_rx(rf_radio_t *r, const rf_radio_cfg_t *cfg);

/* Persistent switch to PTX (channel/address from cfg) on an already
 * initialized chip, without re-claim or SPI re-init. Counterpart of rf_driver_rearm_rx.
 * Fusion phase 2: the left half alternates PRX(USB)<->PTX(wireless) depending on the route. */
void rf_driver_set_ptx(rf_radio_t *r, const rf_radio_cfg_t *cfg);

/* PRX: loads the payload that will go out in the NEXT ACK sent on `pipe`
 * (W_ACK_PAYLOAD, nRF24L01+ PS §7.4.2). ESB guarantees the outbound leg (the
 * PTX's frame is retransmitted until ACK) but NOT the return leg: the ACK —
 * and its payload — can be lost without anyone knowing. The protocol above
 * must therefore be idempotent (the PTX re-asks for what it didn't get).
 * Requires EN_ACK_PAY (FEATURE bit1), enabled in every init/re-arm. len <= 32.
 * Auto keymap sync: docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md */
void rf_driver_load_ack_payload(rf_radio_t *r, uint8_t pipe, const uint8_t *data, uint8_t len);

/* Boot-time sanity check on a freshly init'd PRX radio: read back CONFIG/EN_AA/
 * EN_RXADDR/RF_CH/RF_SETUP/RX_ADDR_P0.lsb and compare to expected init values.
 * Logs "verify OK" with the read-back values, or "verify FAIL" with the per-register
 * diff (exp vs got) — useful to detect a radio whose SPI writes silently didn't
 * fully land (marginal solder joint, signal integrity, etc.). Returns true on match. */
bool rf_driver_verify_rx(rf_radio_t *r, const rf_radio_cfg_t *cfg);

/* Out-of-band one-shot PTX for the dongle PKT_PAIR_ACK (spec §5.4).
 * Switches to PTX on ch+addr, transmits payload once (CE pulse, poll TX_DS/MAX_RT),
 * then restores PRX on restore_ch+restore_addr and re-asserts CE high. Returns TX_DS.
 * Compiled in both roles (no KASE_HAS_RF_TX guard). */
/* Excursion outcomes: acknowledged / MAX_RT / poll timed out. A rising
 * timeout counter means the dongle isn't responding within the 5 ms. */
extern uint32_t rf_oob_ok, rf_oob_maxrt, rf_oob_timeout;

bool rf_driver_oob_tx(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                      const uint8_t *payload, uint8_t len,
                      uint8_t restore_ch, const uint8_t restore_addr[5]);
/* Same excursion, additionally recovering the ACK's payload (EN_ACK_PAY) —
 * the only downlink channel to a half in PRX on another channel (left half in
 * USB mode: DISPLAY frame). ack_out >= 32 B; *ack_len = 0 if the ACK was bare. */
bool rf_driver_oob_tx_ap(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                         const uint8_t *payload, uint8_t len,
                         uint8_t restore_ch, const uint8_t restore_addr[5],
                         uint8_t *ack_out, uint8_t *ack_len);

/* Power-down/up the NRF chip (works for both PTX and PRX roles).
 * power_down: CE low (standby) → clear PWR_UP (CONFIG bit1) → chip draws ~900 nA.
 * power_up:   set PWR_UP → wait Tpd2stby (~5 ms margin; datasheet min 1.5 ms) → CE
 *             state is NOT changed here; caller must assert CE for TX/RX if needed.
 * These operate on REG_CONFIG directly via the public rf_driver_write/read_reg API.
 * BENCH-NOTE: the exact CONFIG value written back by power_up preserves the
 * current PRIM_RX/mask bits via read-modify-write. */
void rf_driver_power_down(rf_radio_t *r);
void rf_driver_power_up(rf_radio_t *r);

/* ── PTX mode — compiled when KASE_HAS_RF_TX=y (half) or KASE_KBD_WIRELESS=y (keyboard relay) ── */
#if CONFIG_KASE_HAS_RF_TX || CONFIG_KASE_KBD_WIRELESS

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

/* Like rf_driver_send, and additionally recovers the payload carried by the ACK
 * (EN_ACK_PAY) if the PRX had loaded one: copied into ack_out (<= 32 B),
 * length in *ack_len (0 = bare ACK). ack_out/ack_len may be NULL — the
 * RX FIFO is then flushed so it doesn't clog up. Returns TX_DS like send.
 * Auto keymap sync: docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md */
bool rf_driver_send_ap(rf_radio_t *radio, const uint8_t *buf, uint8_t len,
                       uint8_t *ack_out, uint8_t *ack_len);

/* Count of MAX_RT events accumulated since last reset (FIFO-flush logic + debug). */
extern uint32_t rf_tx_max_rt_count;

/* ARC_CNT accumulators since last reset: Σ per-packet retransmits and TX count.
 * half_scan_task reads both to compute PKT_HEARTBEAT.link_q as a retry percentage
 * (rf_tx_retr_sum × 100 / (rf_tx_count × 3)), then clears them. */
extern uint32_t rf_tx_retr_sum;
extern uint32_t rf_tx_count;

/* Reprogram the half's TX address (REG_TX_ADDR + REG_RX_ADDR_P0, 5 bytes, CE low).
 * Used to retarget the half radio to RF_PAIR_ADDR for the PKT_PAIR_REQ burst. */
void rf_driver_set_tx_address(rf_radio_t *r, const uint8_t addr[5]);

/* Pairing-only: switch the half radio to PRX on ch+addr, wait up to timeout_ms for
 * one RX payload (PKT_PAIR_ACK), copy to buf (max maxlen). Returns length (0 on
 * timeout). Leaves radio in PRX — caller restores PTX via set_tx_address. */
uint16_t rf_driver_pair_listen(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                               uint8_t *buf, uint16_t maxlen, uint32_t timeout_ms);

#endif /* CONFIG_KASE_HAS_RF_TX || CONFIG_KASE_KBD_WIRELESS */

#endif /* RF_DRIVER_H */
