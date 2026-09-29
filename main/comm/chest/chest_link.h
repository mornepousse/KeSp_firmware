#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "chest_view.h"       /* chest_view_t, CHEST_VIEW_PRESENT/BADVER, chest_view_build (re-exported) */
void    chest_link_start(void);             /* after the radio created the SPI bus */
void    chest_link_presence(bool usb_host); /* from the sleep task, every 1 s; cheap when unchanged */
/* CHEST_VIEW_PRESENT/BADVER (chest_view.h) MUST equal the memlcd model's
 * MEMLCD_COFFRE_PRESENT/BADVER (memlcd_model.h) — the two are defined
 * independently on either side of the display_backend vtable boundary;
 * memlcd_backend.c _Static_asserts they match. */
/* Screen view: a snapshot of chest_view_build()'s last result, copied under
 * the link task's lock (chest_link.c). */
void    chest_link_view(chest_view_t *v);
