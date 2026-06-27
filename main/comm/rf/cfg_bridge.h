#pragma once
#include <stdint.h>
#include <stdbool.h>

/* True if a KS command must be handled by the dongle itself; false if it is
 * config-class and must be forwarded to the paired smart keyboard.
 *
 * Pure function — no side effects, no ESP-IDF dependencies. */
bool cfg_is_dongle_local(uint8_t cmd_id);
