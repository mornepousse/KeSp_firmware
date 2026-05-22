#pragma once

/*
 * eink.h — SSD1681 e-ink skeleton API for KaSe half firmware.
 *
 * Hardware: WeAct 1.54" SSD1681, 200×200 pixels, 1bpp.
 * GPIO assignments from board.h (BOARD_EINK_* defines):
 *   CS=GPIO18, DC=GPIO12, RST=GPIO17, BUSY=GPIO1
 * Shares SPI2 bus with NRF24 (MOSI=GPIO48, MISO=GPIO47, SCK=GPIO45).
 *
 * Both halves compile this module (reversible PCB — runtime probe determines presence).
 *
 * Skeleton status:
 *   REAL:  SPI device registration, GPIO config, RST pulse, BUSY probe.
 *   REAL:  half_spi_lock/unlock ordering (unlock before BUSY wait).
 *   STUB:  SSD1681 command sequences (init, write RAM, trigger refresh).
 *   STUB:  1bpp rendering (layer name, icons). Approach: direct bitmaps or LVGL — TBD.
 *
 * CRITICAL: The SPI bus lock is held ONLY during SPI transactions.
 * The BUSY wait (~1-2 s) is performed OUTSIDE the lock so NRF24 can
 * continue transmitting key events during panel refresh.
 */

#include <stdbool.h>
#include <stdint.h>

#define EINK_WIDTH    200
#define EINK_HEIGHT   200
#define EINK_FB_SIZE  (EINK_WIDTH * EINK_HEIGHT / 8)   /* 5000 bytes, 1bpp MSB-first */

/* Initialize e-ink hardware:
 *   - Add SSD1681 as a second spi_device on SPI2_HOST (CS=GPIO18, 4 MHz, mode 0)
 *   - Configure DC=GPIO12 (output), RST=GPIO17 (output), BUSY=GPIO1 (input)
 *   - Reset pulse: RST high 10 ms → low 10 ms → high 10 ms
 *   - Probe: wait BUSY=GPIO1 low within 200 ms (panel ready after reset)
 *   - Initialize framebuffer to 0xFF (all white, SSD1681 convention)
 * Returns true if panel is physically present (BUSY went low within timeout).
 * Returns false if BUSY stays high (panel not mounted, timeout exceeded). */
bool eink_init(void);

/* Clear the panel to white (all bits = 0xFF in framebuffer, then push + refresh).
 * Blocks until BUSY goes low (refresh complete, ~1-2 s full refresh). */
void eink_clear(void);

/* Push a 5000-byte 1bpp framebuffer to the panel and trigger a full refresh.
 * fb must point to exactly EINK_FB_SIZE bytes (5000).
 *
 * Lock ordering (CRITICAL — do not change):
 *   1. half_spi_lock()          — acquire bus
 *   2. SPI write (commands + 5000-byte RAM)  — ~10 ms
 *   3. half_spi_unlock()        — release bus
 *   4. BUSY poll (vTaskDelay)   — up to ~2 s (bus is FREE, NRF can send)
 *
 * This function initiates the refresh. It does NOT block on BUSY internally;
 * eink_task polls BUSY after calling eink_push. */
void eink_push(const uint8_t *fb);

/* Initialize LVGL and start the e-ink LVGL handler task.
 * Only call if eink_init() returned true.
 * Calls eink_lvgl_init() (LVGL setup + static screen) then eink_lvgl_start()
 * (handler task, prio 3, stack 4096, core 0). */
void eink_start(void);

/* ── 1bpp pixel packing helper ───────────────────────────────── */

/* Pack one pixel into the SSD1681 B&W RAM framebuffer (MSB-first, white=1).
 *
 * Called from the LVGL set_px_cb for every pixel LVGL renders.
 *
 * Layout contract:
 *   fb[row * (EINK_WIDTH/8) + col/8]  bit  (7 - col%8) = is_white
 *   MSB = leftmost pixel in each byte. Row 0 = top of panel.
 *   200 pixels wide → 25 bytes per row → 5000 bytes total (EINK_FB_SIZE).
 *
 * SSD1681 native: bit=1 → white, bit=0 → black.
 * LVGL v8 at LV_COLOR_DEPTH=1: lv_color_white().full == 1 (white),
 * lv_color_black().full == 0 (black). Polarity is direct — no inversion.
 *
 * Exposed non-static for host-side testing under TEST_HOST.
 * In firmware, called only from eink_lvgl_set_px_cb (eink_lvgl.c). */
void eink_fb_set_px(uint8_t *fb, int col, int row, int is_white);
