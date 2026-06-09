#pragma once
#include <stdint.h>
uint16_t cr_crc16(const uint8_t *data, uint16_t len);  /* YubiKey CRC-16, init 0xFFFF */
