#ifndef BOARD_RF_H
#define BOARD_RF_H

#include "board.h"
#include "rf_driver.h"

/* Two radios on a shared SPI bus, one per half. Channels/addresses are
 * loaded from NVS at runtime (Plan 4) — these are compile-time fallbacks
 * matching spec Section 4 factory defaults. */
#define RF_CH_LEFT_DEFAULT    0x4C   /* 2476 MHz */
#define RF_CH_RIGHT_DEFAULT   0x52   /* 2482 MHz */

static inline rf_radio_cfg_t board_rf_left_cfg(void)
{
    rf_radio_cfg_t c = {
        .spi_host = BOARD_NRF_SPI_HOST,
        .pin_mosi = BOARD_NRF_SPI_MOSI, .pin_miso = BOARD_NRF_SPI_MISO,
        .pin_sck  = BOARD_NRF_SPI_SCK,  .clock_hz = BOARD_NRF_SPI_CLOCK_HZ,
        .pin_csn  = BOARD_NRF1_CSN_GPIO, .pin_ce = BOARD_NRF1_CE_GPIO,
        .pin_irq  = BOARD_NRF1_IRQ_GPIO,
        .channel  = RF_CH_LEFT_DEFAULT,
        .rx_addr  = { 'K', 'a', 'S', 'e' },
        .addr_suffix = 0x01,
        .shares_bus_first = true,    /* this radio initializes the SPI bus */
    };
    return c;
}

static inline rf_radio_cfg_t board_rf_right_cfg(void)
{
    rf_radio_cfg_t c = {
        .spi_host = BOARD_NRF_SPI_HOST,
        .pin_mosi = BOARD_NRF_SPI_MOSI, .pin_miso = BOARD_NRF_SPI_MISO,
        .pin_sck  = BOARD_NRF_SPI_SCK,  .clock_hz = BOARD_NRF_SPI_CLOCK_HZ,
        .pin_csn  = BOARD_NRF2_CSN_GPIO, .pin_ce = BOARD_NRF2_CE_GPIO,
        .pin_irq  = BOARD_NRF2_IRQ_GPIO,
        .channel  = RF_CH_RIGHT_DEFAULT,
        .rx_addr  = { 'K', 'a', 'S', 'e' },
        .addr_suffix = 0x02,
        .shares_bus_first = false,   /* bus already initialized by left radio */
    };
    return c;
}

#endif /* BOARD_RF_H */
