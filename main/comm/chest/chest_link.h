#pragma once
#include <stdbool.h>
#include <stdint.h>
void    chest_link_start(void);             /* after the radio created the SPI bus */
void    chest_link_presence(bool usb_host); /* from the sleep task, every 1 s; cheap when unchanged */
/* Screen view: 0 = no chest; else MEMLCD_COFFRE_PRESENT (0x80) | BADVER (0x40)
 * | CHEST_STATE_* bits; *op = pending op (0 none). */
uint8_t chest_link_view(uint16_t *op);
