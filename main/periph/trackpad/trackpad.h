#pragma once

/*
 * trackpad.h — IQS5xx trackpad skeleton API.
 *
 * Hardware: TPS43-201A-S (Azoteq IQS5xx-compatible) on I2C.
 * GPIO assignments from board.h:
 *   SDA=BOARD_TRACK_SDA_GPIO (GPIO40), SCL=BOARD_TRACK_SCL_GPIO (GPIO38)
 *   RST=BOARD_TRACK_RST_GPIO (GPIO13), RDY=BOARD_TRACK_RDY_GPIO (GPIO14)
 *
 * Both halves compile this module (PCB is reversible — presence detected at runtime
 * by I2C ACK probe). Only call trackpad_start() if trackpad_init() returns true.
 *
 * Register reads (IQS5xx protocol) are stubs — see TODO in trackpad.c.
 */

#include <stdbool.h>

/* Initialize trackpad hardware:
 *   - I2C master init on SDA=GPIO40, SCL=GPIO38 at 400 kHz
 *   - Pulse RST=GPIO13 low 10 ms then high; settle 50 ms
 *   - Configure RDY=GPIO14 as input with ISR on NEGEDGE (→ binary semaphore)
 *   - I2C ACK probe at BOARD_TRACK_I2C_ADDR
 * Returns true if trackpad is physically present (ACK received).
 * Returns false if no ACK (trackpad not mounted on this half). */
bool trackpad_init(void);

/* Start the trackpad FreeRTOS task (prio 6, stack 3072, core 0).
 * Only call if trackpad_init() returned true.
 * The task waits on the RDY semaphore, reads (stubs) touch data, and
 * transmits PKT_TRACKPAD over NRF24 via half_spi_lock + rf_driver_send. */
void trackpad_start(void);
