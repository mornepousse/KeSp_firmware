#pragma once

/*
 * half_spi.h — Shared SPI2 bus lock for the KaSe half firmware.
 *
 * The SPI2 bus (MOSI=GPIO48, MISO=GPIO47, SCK=GPIO45) is shared between:
 *   - NRF24L01+ (CSN=GPIO35) — accessed by tx_key_event + heartbeat_timer_cb
 *   - SSD1681 e-ink (CS=GPIO18) — accessed by eink_push() in eink_task
 *
 * All SPI transactions must hold this lock. The BUSY wait of the SSD1681
 * (~1-2 s) is performed OUTSIDE the lock (bus is free while panel refreshes).
 *
 * I2C transactions (trackpad IQS5xx) use a separate bus and do NOT need this lock.
 *
 * Init: half_spi_lock_init() must be called once before any half_spi_lock() call,
 *       i.e. before rf_driver_init_tx(), trackpad_init(), and eink_init().
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Initialize the shared SPI2 bus mutex.
 * Must be called exactly once, at the start of half_scan_task (before NRF init). */
void half_spi_lock_init(void);

/* Acquire the SPI2 bus for exclusive use. Blocks until available (portMAX_DELAY). */
void half_spi_lock(void);

/* Release the SPI2 bus. */
void half_spi_unlock(void);
