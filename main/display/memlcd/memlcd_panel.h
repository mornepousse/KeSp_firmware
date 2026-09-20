#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "memlcd_model.h"   /* MEMLCD_W/H, MEMLCD_LINE_BYTES, memlcd_rev8 */

/* Driver for the Sharp LS011B7DH03 (nice!view module) on the SPI bus SHARED with
 * the nRF24. Every transaction goes through rf_bus_lock(): never during a radio
 * frame. Write-only, CS ACTIVE HIGH driven by hand, raw command word (M0 = bit 7), line
 * address in rev8 (CA0 first), software VCOM toggled on every write and by
 * memlcd_panel_vcom_tick() (~1 Hz awake; nothing in sleep: image is kept).
 * Spec: docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md §1 */

/* Call VERY early at boot (before the radio): CS as output and LOW, so the
 * screen never listens to bus traffic. Idempotent, harmless with no screen. */
void memlcd_cs_idle(void);

/* Adds the SPI device on the radio bus (rf_bus_host). After the radio. */
esp_err_t memlcd_panel_init(void);

bool memlcd_panel_clear(void);                       /* M2: all white */
/* Writes `count` PANEL LINES starting at `first` (0..67); lines = count × 20
 * bytes already transposed (memlcd_fb_to_panel): bit 7 = D1, 1 = white. */
bool memlcd_panel_write_lines(uint16_t first, uint16_t count, const uint8_t *lines);
/* Displays a PORTRAIT buffer (MEMLCD_H × MEMLCD_LINE_BYTES, 1 = ink):
 * transpose + write of the 68 lines. ~12 ms at 1 MHz. */
bool memlcd_panel_show(const uint8_t *fb);
bool memlcd_panel_vcom_tick(void);                   /* M1 alone: VCOM upkeep */
void memlcd_panel_test_pattern(void);                /* bring-up test pattern: frame + top-left block + checkerboard */
