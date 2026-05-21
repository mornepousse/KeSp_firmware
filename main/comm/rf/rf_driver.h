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

#endif /* RF_DRIVER_H */
