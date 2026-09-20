#ifndef RF_PROBE_H
#define RF_PROBE_H

/* Bring-up: does the nRF24 respond?
 *
 * Configures the GPIOs, opens the SPI bus, then reads CONFIG and RF_SETUP via
 * rf_driver_init() — which calls rf_driver_probe() and fails if the chip does
 * not respond. A missing or badly wired module reads 0x00 or 0xFF on both
 * registers, which the probe rejects.
 *
 * Logs at ESP_LOGW (tag "rf_probe") to stay visible even at WARN level.
 * Requires a console: CONFIG_ESP_CONSOLE_UART_DEFAULT, active on both
 * Niphargus halves.
 *
 * Guarded by CONFIG_KASE_NRF_PROBE (default n). Bench diagnostic only —
 * it installs no task and relays nothing. The real radio link is the
 * subject of B3/B4 (docs/superpowers/specs/2026-08-19-niphargus-firmware-design.md).
 */
void rf_probe_run(void);

#endif /* RF_PROBE_H */
