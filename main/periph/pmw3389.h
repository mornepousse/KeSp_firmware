/* PMW3389DM-T3QU — optical sensor of the Conchodytes mouse.
 *
 * PixArt datasheet, Version 1.0 | 07 Sep 2017. Validated on hardware on 2026-08-25:
 * Product_ID 0x47, Inverse 0xB8, Revision 0x01, SROM_ID 0xE8 after
 * upload, motion read, SQUAL ~80 on surface.
 *
 * WARNING: THIS IS NOT A PMW3360, despite what some file names in the
 * hardware repo still say. The differences that matter for this driver:
 *
 *   - entirely different SROM blob (99.6% of the bytes);
 *   - `tSRAD` = 160 µs and `tSWW`/`tSWR` = 180 µs, against 35 and 120 on the 3360.
 *     The reference code this driver derives from waits 100 µs: out of spec
 *     here, and it's the kind of gap that passes on the bench then fails intermittently;
 *   - resolution is set via `Resolution_L`/`_H` (0x0E/0x0F) over 16 bits,
 *     not via an 8-bit `Config1`.
 *
 * The SPI bus is SHARED with the nRF24, which works in mode 0 when this
 * sensor wants mode 3. ESP-IDF reconfigures the mode per device, but
 * the mutual exclusion remains our responsibility — see pmw3389.c.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Identity values, Table of §5.1, p. 20.
 *
 * WARNING: the datasheet states `Inverse_Product_ID` = 0xB9, but 0x47 ^ 0xFF = 0xB8,
 * and 0xB8 is indeed what the chip returns. A PixArt typo: we expect the
 * exact complement, which is also a free bus check —
 * `id ^ inv == 0xFF` fails on a cut line just as on a stuck line. */
#define PMW3389_PRODUCT_ID          0x47
#define PMW3389_INVERSE_PRODUCT_ID  0xB8

typedef struct {
    int16_t dx, dy;    /* counts since the previous read */
    uint8_t squal;     /* surface quality; ~80 on a good pad */
    uint16_t shutter;  /* shutter; at ceiling = the sensor sees nothing */
    bool     motion;   /* MOT bit of the burst: false => dx/dy forced to 0, see the .c */
} pmw3389_motion_t;

/* Initializes the bus (if needed), resets the sensor's serial port,
 * checks the identity, then uploads the SROM.
 *
 * The order matters and is not that of the reference code: the reset
 * must precede THE VERY FIRST register read. Found on hardware —
 * reading `Product_ID` first, the chip returned 0x11 where it should be 0x47,
 * that is `0x47 >> 2`: two clock bits of shift. Subsequent reads
 * were aligned, which makes the defect all the more insidious. */
esp_err_t pmw3389_init(void);

/* Sets the resolution in cpi (50 to 16000, rounded down to the nearest 50).
 * `pmw3389_init()` calls it with BOARD_SNS_CPI. See the body: the encoding
 * is not documented in the available datasheet and is validated empirically. */
void pmw3389_set_cpi(uint16_t cpi);

/* Reads the identity without modifying anything. Useful for diagnostics; `pmw3389_init`
 * already checks it and fails if it does not match. */
esp_err_t pmw3389_probe(uint8_t *id, uint8_t *inverse, uint8_t *revision);

/* Reads the accumulated motion and resets the counters to zero, in ONE
 * transaction (`Motion_Burst`, 0x50).
 *
 * WARNING: never mix this call with reads of motion registers
 * in the same loop: both consume the same counters and steal each other's
 * motion. Found on the bench on 2026-08-25 — a loop that did both
 * only saw crumbs.
 *
 * The motion is returned as the CHIP sees it. On v1, the sensor is
 * mounted at 180°, so both axes are inverted relative to the HID
 * convention (+X to the right, +Y downward). Measured: to the right -> X negative,
 * forward -> Y positive. The correction will happen either in the v2 layout
 * (180° rotation), or here — but NOT in both places. See
 * Conchodytes/NOTES-V2.md §1. */
esp_err_t pmw3389_read_motion(pmw3389_motion_t *out);

/* true when the MOTION pin is active (low): the chip has motion to
 * provide. This is what lets a battery-powered mouse sleep instead
 * of polling the sensor in a loop. */
bool pmw3389_motion_pending(void);
