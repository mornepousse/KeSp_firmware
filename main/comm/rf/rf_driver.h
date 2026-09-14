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
/* Règle SETUP_RETR (0x04) : quartet haut = ARD, pas de 250 µs moins un ;
 * quartet bas = ARC, nombre de retransmissions.
 *
 * ⚠ Le pilote initialise 0x1F (ARD=500 µs, ARC=15), taillé pour le CLAVIER dont
 * l'état est ABSOLU : réémettre une frappe perdue est toujours juste, et attendre
 * en vaut la peine. Pour un déplacement RELATIF c'est l'inverse — la trame de
 * remplacement arrive avant que la retransmission n'aboutisse, et pendant ce
 * temps `rf_driver_send()` bloque son appelant.
 *
 * `rf_driver_send()` déduit son délai de scrutation de ce registre : le baisser
 * raccourcit l'attente automatiquement, il n'y a pas de seconde constante à
 * garder en phase. */
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

/* Bascule persistante vers PTX (canal/adresse du cfg) sur une puce déjà
 * initialisée, sans re-claim ni ré-init SPI. Pendant de rf_driver_rearm_rx.
 * Fusion phase 2 : la gauche alterne PRX(USB)↔PTX(sans-fil) selon la route. */
void rf_driver_set_ptx(rf_radio_t *r, const rf_radio_cfg_t *cfg);

/* PRX : charge la charge utile qui partira dans le PROCHAIN ACK émis sur `pipe`
 * (W_ACK_PAYLOAD, nRF24L01+ PS §7.4.2). L'ESB garantit l'aller (la trame du
 * PTX est retransmise jusqu'à ACK) mais PAS le retour : l'ACK — et sa charge —
 * peut se perdre sans que personne ne le sache. Le protocole au-dessus doit donc
 * être idempotent (le PTX redemande ce qu'il n'a pas reçu). Requiert EN_ACK_PAY
 * (FEATURE bit1), activé dans toutes les inits/réarmements. len ≤ 32.
 * Sync auto keymap : docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md */
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
/* Issues des excursions : acquittees / MAX_RT / scrutin expire. Un compteur de
 * timeouts qui monte veut dire que le dongle ne repond pas dans les 5 ms. */
extern uint32_t rf_oob_ok, rf_oob_maxrt, rf_oob_timeout;

bool rf_driver_oob_tx(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                      const uint8_t *payload, uint8_t len,
                      uint8_t restore_ch, const uint8_t restore_addr[5]);
/* Même excursion, en récupérant la charge utile de l'ACK (EN_ACK_PAY) — le
 * seul canal descendant vers une moitié en PRX sur un autre canal (gauche en
 * mode USB : trame DISPLAY). ack_out ≥ 32 o ; *ack_len = 0 si l'ACK était nu. */
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

/* Comme rf_driver_send, et récupère en plus la charge utile portée par l'ACK
 * (EN_ACK_PAY) si le PRX en avait chargé une : copiée dans ack_out (≤ 32 o),
 * longueur dans *ack_len (0 = ACK nu). ack_out/ack_len peuvent être NULL — la
 * FIFO RX est alors vidée pour ne pas s'encrasser. Retourne TX_DS comme send.
 * Sync auto keymap : docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md */
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
