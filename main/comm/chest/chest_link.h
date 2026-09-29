#pragma once
#include <stdbool.h>
#include <stdint.h>
void    chest_link_start(void);             /* after the radio created the SPI bus */
void    chest_link_presence(bool usb_host); /* from the sleep task, every 1 s; cheap when unchanged */
/* Values MUST equal the memlcd model's MEMLCD_COFFRE_PRESENT/BADVER
 * (memlcd_model.h) — the two are defined independently on either side of the
 * display_backend vtable boundary; memlcd_backend.c _Static_asserts they
 * match. */
#define CHEST_VIEW_PRESENT 0x80
#define CHEST_VIEW_BADVER  0x40
/* Screen view: 0 = no chest; else CHEST_VIEW_PRESENT | CHEST_VIEW_BADVER
 * | CHEST_STATE_* bits; *op = pending op (0 none). */
uint8_t chest_link_view(uint16_t *op);
