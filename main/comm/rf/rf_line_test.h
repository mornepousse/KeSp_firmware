#ifndef RF_LINE_TEST_H
#define RF_LINE_TEST_H

/* NRF line short-circuit bring-up diagnostic.
 *
 * Drives each NRF line (SPI MOSI/MISO/SCK + per-radio CSN/CE/IRQ) and detects
 * solder bridges between them. Bidirectional (drives HIGH with pulldown reader,
 * then LOW with pullup reader) so it is immune to a line's own idle pull-up
 * (e.g. IRQ lines idle high) — a pair is only reported SHORT if the reader
 * follows the driver in BOTH directions.
 *
 * Logs results with ESP_LOGW under tag "rf_line_test", so it is visible even
 * when the app log level is WARN. Needs a console to read it
 * (CONFIG_ESP_CONSOLE_UART_DEFAULT on the dongle, which is CONSOLE_NONE in
 * production). Pins are read from board_rf_left_cfg()/board_rf_right_cfg(), so
 * it is board-agnostic.
 *
 * Gated by CONFIG_KASE_NRF_LINE_TEST (default n); called once at boot from
 * rf_rx_start() before radio init. Resets all touched pins afterwards so the
 * SPI bus / radio init can reclaim them. Dongle role only (needs both radio
 * cfgs). Leave the Kconfig option off in production. */
void rf_line_test_run(void);

#endif /* RF_LINE_TEST_H */
