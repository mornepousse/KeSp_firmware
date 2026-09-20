#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"

/* The SPI bus of a half belongs to the RADIO — "one chip, one owner"
 * (CLAUDE.md: two modules that each touched the chip on their own side
 * broke the link three times, silently). Another slave on this bus — the
 * Sharp memory-LCD screen — only accesses it under the owner's lock, so
 * never during an nRF transmission. Implemented ONCE per half: kbd_relay
 * (left) or half_link (right). Short timeout: if the radio is busy, the screen
 * yields and retries on the next tick — a keystroke is worth more than a pixel.
 *
 * ⚠ never call from the radio transmit path (it already holds this
 * lock, non-recursive) nor from an ISR. */
bool rf_bus_lock(uint32_t timeout_ms);
void rf_bus_unlock(void);
spi_host_device_t rf_bus_host(void);
