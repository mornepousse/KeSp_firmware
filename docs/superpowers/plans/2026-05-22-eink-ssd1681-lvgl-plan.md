# Plan Bricks-3 — SSD1681 E-Ink Driver + LVGL Integration (KaSe Half)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the full SSD1681 e-ink display pipeline on the KaSe half firmware: fill the `eink_push()` 10-step command sequence, integrate raw LVGL v8 at 1bpp, render a static "KaSe" + firmware version screen, and validate that NRF24 key events are not blocked during panel refresh. End state: flashing the half-right shows the "KaSe" label and version string on the e-ink panel; typing still works through the dongle.

**Architecture:** Three layers wired in sequence.

```
┌──────────────────────────────────────────────────────────┐
│  LVGL v8 (LV_COLOR_DEPTH=1, monochrome)                  │
│    lv_label "KaSe"   lv_label firmware version           │
│    flush_cb: packs LVGL 1bpp area → s_fb (MSB-first)    │
│    Tick: esp_timer 5 ms → lv_tick_inc(5)                 │
│    Handler: eink_lvgl_task (lv_timer_handler loop)       │
└─────────────────────┬────────────────────────────────────┘
                      │ eink_push(s_fb) on last flush fragment
┌─────────────────────▼────────────────────────────────────┐
│  SSD1681 driver (eink.c)                                  │
│    half_spi_lock()                                        │
│    10-step command sequence (steps detailed below)        │
│    half_spi_unlock()                                      │
│    BUSY poll outside lock (~1–2 s full refresh)           │
└─────────────────────┬────────────────────────────────────┘
                      │ SPI2_HOST, 4 MHz, CS=GPIO18
┌─────────────────────▼────────────────────────────────────┐
│  SSD1681 panel — 200×200 px B&W RAM                      │
│    Default OTP LUT (full refresh waveform)                │
│    BUSY pin: H=refreshing, L=ready                        │
└──────────────────────────────────────────────────────────┘
```

**Tech Stack:** ESP-IDF 5.5, raw LVGL v8 (no `esp_lvgl_port`), `esp_driver_spi` (already linked), `esp_timer`, FreeRTOS. No new external components — `lvgl/lvgl: ^8` is already in `main/idf_component.yml`.

**Spec reference:** `docs/superpowers/specs/2026-05-22-eink-ssd1681-lvgl-design.md` (all sections, read first).

**Depends on:** Plan Bricks-2 (eink.c skeleton exists and hardware-validated: `eink_init()` is real, `eink_push()` stubs the SPI path, `half_spi_lock/unlock` contract is in place, `KASE_HAS_EINK=y` in Kconfig).

**Build targets:**

```bash
# After rm -f sdkconfig (required when switching boards):
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build
```

Always `rm -f sdkconfig` before switching board variants. Build dirs: `build_half_left`, `build_half_right`, `build_dongle`.

**Hardware (from `boards/kase_half_left/board.h`):**

| Signal | GPIO | Direction | Notes |
|--------|------|-----------|-------|
| CS | GPIO18 (`BOARD_EINK_CS_GPIO`) | Output | Active-low, SPI chip select |
| DC | GPIO12 (`BOARD_EINK_DC_GPIO`) | Output | H=data, L=command |
| RST | GPIO17 (`BOARD_EINK_RST_GPIO`) | Output | Active-low, pulse at boot |
| BUSY | GPIO1 (`BOARD_EINK_BUSY_GPIO`) | Input | H=refreshing, L=ready |
| MOSI | GPIO48 | Output | Shared SPI2 with NRF24 |
| MISO | GPIO47 | Input | Shared SPI2 (e-ink write-only) |
| SCK | GPIO45 | Output | Shared SPI2 |

---

## Critical implementation traps — read before touching any file

These four traps are the most common failure modes for this brick. Each task below references the relevant trap by number.

**Trap 1 — LVGL v8 1bpp buffer sizing.**
`sizeof(lv_color_t) == 1` at `LV_COLOR_DEPTH=1`, so an array of `EINK_WIDTH * EINK_HEIGHT = 40000` elements allocates 40 KB — not 5000 bytes. Use a single-panel packed buffer instead:
```c
/* 5000 bytes at 1bpp: one byte covers 8 pixels */
static uint8_t s_lvgl_buf_raw[EINK_FB_SIZE];   /* EINK_FB_SIZE = 5000 */
static lv_color_t *s_lvgl_buf = (lv_color_t *)s_lvgl_buf_raw;
```
The cast is valid because LVGL v8 at 1bpp stores pixels packed MSB-first in bytes.

**Trap 2 — Polarity alignment.**
SSD1681 convention: bit=1 → white, bit=0 → black. LVGL v8 at `LV_COLOR_DEPTH=1`: white pixels render as 1, black as 0. These align naturally, but the host test (T1) must prove it: all-white input must produce `s_fb[i] == 0xFF` for all i. Do not proceed to T3 without this assertion passing.

**Trap 3 — `full_refresh = 1` is mandatory.**
Without `drv.full_refresh = 1`, LVGL sends only the dirty region on re-renders. The SSD1681 RAM window is set once at the start of `eink_push()` — a partial area write would corrupt the framebuffer mapping. Always set `full_refresh = 1` on the `lv_disp_drv_t`. With it set, every flush covers exactly `[0,0]–[199,199]` and `eink_pack_lvgl_area` always receives the full frame — simplifying packing significantly.

**Trap 4 — LVGL handler task stack.**
LVGL label layout + font rendering uses the task stack. Start at 4096 bytes. After first boot, log `uxTaskGetStackHighWaterMark(NULL)` from inside `eink_lvgl_task` and bump to 6144 if headroom is below 512 bytes. Do not skip this check.

---

## Learned facts baked in (from prior plan execution)

1. **`eink_send_cmd` and `eink_send_data` are already real helpers in `eink.c`** (static, caller must hold `half_spi_lock`). Do not rewrite them — just call them from the filled `eink_push`.

2. **`half_spi_lock/unlock` contract is fixed.** Lock before all SPI ops; unlock before BUSY poll. Reversing this blocks NRF24 TX for up to 2 s per refresh. Preserve the existing structure in `eink_push` verbatim.

3. **`TEST_ASSERT` takes 2 args, `TEST_ASSERT_EQ` takes 3 args.** Pattern: `TEST_ASSERT(cond, "msg")`, `TEST_ASSERT_EQ(actual, expected, "msg")`. Add `.c` files to `test/CMakeLists.txt`; add `extern void test_xxx(void)` + direct call in `test/test_main.c`.

4. **`rm -f sdkconfig` required before switching boards.** Shared `sdkconfig` retains previous board's settings. After rm, always pass `-DIDF_TARGET=esp32s3`.

5. **`lvgl/lvgl: ^8` already in `main/idf_component.yml`.** No new component entry needed. However, `lvgl` must be listed in `priv_requires` under `KASE_HAS_EINK` in `main/CMakeLists.txt` if it is not already there — check before T3.

6. **`LV_COLOR_DEPTH=1` is HALF-build-only.** The keyboard builds use `LV_COLOR_DEPTH=16` (baked into their sdkconfig). The half has independent build dirs (`build_half_left`, `build_half_right`) and independent `sdkconfig.defaults.half_left/right`. No conflict. The dongle has no LVGL.

7. **The `eink_task` stub (1 Hz `vTaskDelay` loop) is retired in this plan.** `eink_start()` is repurposed to call `eink_lvgl_start()` instead of creating the old polling task. The caller site in `half_scan_task.c` does not change — it still calls `eink_start()` after `eink_init()` returns true.

8. **clangd IDE errors about esp-idf/lvgl headers are false positives** — the build system injects include paths that clangd does not see. Trust `idf.py build` as the ground truth.

9. **`eink_pack_lvgl_area` must be non-static and declared in `eink.h` under a `TEST_HOST` guard** so the host test can link it without any ESP-IDF headers. It has zero FreeRTOS/GPIO/SPI deps — pure byte manipulation.

10. **LVGL heap: ~8 KB for v8 core + label + screen at 1bpp.** The half has 512 KB SRAM. Static BSS adds `s_fb` (5000 bytes, already present) + `s_lvgl_buf_raw` (5000 bytes) + task stack (4–6 KB). All within budget.

---

## File Structure

### Created

| Path | Responsibility |
|------|----------------|
| `main/periph/eink/eink_lvgl.c` | Raw LVGL init, disp driver, flush_cb, tick timer, handler task, static screen |
| `main/periph/eink/eink_lvgl.h` | Public API: `eink_lvgl_init()`, `eink_lvgl_start()` |
| `test/test_eink_pack.c` | Host tests for `eink_pack_lvgl_area()` — must pass before T2 (flush_cb) is written |

### Modified

| Path | Change |
|------|--------|
| `main/periph/eink/eink.c` | Fill `eink_push()` 10-step SSD1681 sequence; repurpose `eink_start()` to delegate to `eink_lvgl_start()` |
| `main/periph/eink/eink.h` | Expose `eink_pack_lvgl_area()` declaration under `TEST_HOST` guard |
| `main/CMakeLists.txt` | Add `eink_lvgl.c` under `KASE_HAS_EINK`; add `lvgl` to priv_requires under `KASE_HAS_EINK` if not present |
| `sdkconfig.defaults.half_left` | Add LVGL 1bpp config block |
| `sdkconfig.defaults.half_right` | Add LVGL 1bpp config block (identical to left) |
| `test/CMakeLists.txt` | Add `test_eink_pack.c` to `add_executable` |
| `test/test_main.c` | Add `extern void test_eink_pack(void)` + direct call |

### Untouched

```
main/comm/rf/half_scan_task.c     caller site unchanged (eink_start() still called there)
main/periph/half_spi.h/c          SPI lock — no changes
boards/kase_half_left/board.h     BOARD_EINK_* all already defined
main/idf_component.yml            lvgl/lvgl ^8 already listed
partitions.csv                    unchanged (2 MB factory partition; ~110 KB LVGL fits)
```

---

## Task 1: Host test — `eink_pack_lvgl_area()` pure packing function (TDD)

**Trap addressed:** Trap 2 (polarity), Trap 3 (full_refresh=1 means area is always [0,0]–[199,199]).

**Files:**
- Modify: `main/periph/eink/eink.h`
- Modify: `main/periph/eink/eink.c` (add `eink_pack_lvgl_area` + `#ifndef TEST_HOST` guards)
- Create: `test/test_eink_pack.c`
- Modify: `test/CMakeLists.txt`
- Modify: `test/test_main.c`

**Rationale for TDD:** `eink_pack_lvgl_area` is the only error-prone piece that can be exercised on the host before touching the SPI or LVGL stack. Write the test first, implement the function to make it pass, then wire the flush callback around it. The function lives in `eink.c` (guarded by `#ifndef TEST_HOST` for all ESP-IDF-dependent code); the test runner compiles `eink.c` with `-DTEST_HOST` (already set in `test/CMakeLists.txt` global flags).

- [ ] **Step 1: Add `eink_pack_lvgl_area` declaration to `eink.h`**

Add after the existing `eink_start()` declaration:

```c
/* ── 1bpp packing helper ─────────────────────────────────────── */

/* Convert LVGL's 1bpp pixel area into the SSD1681 B&W RAM layout.
 *
 * Called from flush_cb. With disp_drv.full_refresh=1, area always covers
 * [0,0]–[EINK_WIDTH-1, EINK_HEIGHT-1] and color_p covers the full frame.
 *
 * Layout contract (MSB-first, white=1):
 *   s_fb[row * 25 + byte_col] bit (7 - pixel_col%8) = 1 for white, 0 for black.
 *   Pixel at (col, row): byte index = row * (EINK_WIDTH/8) + col/8
 *                         bit  index = 7 - (col % 8)   [MSB = leftmost pixel]
 *
 * LVGL v8 at LV_COLOR_DEPTH=1 stores pixels as lv_color_t (1 byte each),
 * value 1 = white, value 0 = black. Stride = EINK_WIDTH pixels per row.
 *
 * Exposed non-static for host-side testing under TEST_HOST.
 * In firmware, called only from eink_lvgl_flush_cb (eink_lvgl.c). */
#ifdef TEST_HOST
#include <stdint.h>
typedef struct { int16_t x1, y1, x2, y2; } lv_area_t;
typedef uint8_t lv_color_t;
#else
#include "lvgl.h"
#endif

void eink_pack_lvgl_area(const lv_area_t *area,
                         const lv_color_t *color_p,
                         uint8_t *fb_out);
```

**Note:** The `#ifdef TEST_HOST` block provides minimal type stubs so the test can include `eink.h` without pulling in the entire ESP-IDF + LVGL include tree. In firmware builds, `lvgl.h` is included normally via the component system.

- [ ] **Step 2: Write `test/test_eink_pack.c`**

Write the test BEFORE the implementation (TDD). The assertions define the contract.

```c
/*
 * test_eink_pack.c — Host tests for eink_pack_lvgl_area() 1bpp packing.
 *
 * Run: cd test/build && cmake .. && make && ./test_runner
 *
 * Contract under test:
 *   - LVGL white pixel (value 1) → SSD1681 bit = 1 → s_fb byte bit = 1.
 *   - LVGL black pixel (value 0) → SSD1681 bit = 0 → s_fb byte bit = 0.
 *   - Pixel (col, row): byte = fb_out[row * 25 + col/8], bit = 7 - (col%8).
 *   - Output size = EINK_FB_SIZE = 5000 bytes exactly.
 */
#define TEST_HOST
#include "test_framework.h"
#include "../main/periph/eink/eink.h"
#include <string.h>

/* Helper: build a full-panel lv_area_t */
static lv_area_t full_area(void)
{
    lv_area_t a = { .x1=0, .y1=0, .x2=EINK_WIDTH-1, .y2=EINK_HEIGHT-1 };
    return a;
}

/* Helper: get the bit value for pixel (col, row) in fb */
static int fb_bit(const uint8_t *fb, int col, int row)
{
    int byte_idx = row * (EINK_WIDTH / 8) + col / 8;
    int bit_idx  = 7 - (col % 8);
    return (fb[byte_idx] >> bit_idx) & 1;
}

void test_eink_pack(void)
{
    TEST_SUITE("eink_pack_lvgl_area");

    static lv_color_t pixels[EINK_WIDTH * EINK_HEIGHT];
    static uint8_t fb[EINK_FB_SIZE];
    lv_area_t area = full_area();

    /* ── Case 1: all-white input → s_fb all 0xFF ───────────── */
    memset(pixels, 1, sizeof(pixels));   /* lv_color_t=1 → white */
    memset(fb, 0x00, EINK_FB_SIZE);
    eink_pack_lvgl_area(&area, pixels, fb);
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0xFF) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "all-white: s_fb must be 0xFF throughout");
    }

    /* ── Case 2: all-black input → s_fb all 0x00 ───────────── */
    memset(pixels, 0, sizeof(pixels));   /* lv_color_t=0 → black */
    memset(fb, 0xFF, EINK_FB_SIZE);
    eink_pack_lvgl_area(&area, pixels, fb);
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0x00) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "all-black: s_fb must be 0x00 throughout");
    }

    /* ── Case 3: single black pixel at (0,0), all others white ── */
    memset(pixels, 1, sizeof(pixels));
    pixels[0] = 0;   /* pixel (col=0, row=0) = black */
    memset(fb, 0x00, EINK_FB_SIZE);
    eink_pack_lvgl_area(&area, pixels, fb);
    /* Byte 0 = row 0, cols 0-7. Col 0 = MSB (bit 7). Black=0 → bit 7 clear.
     * All other pixels white → remaining bits set.
     * Expected: fb[0] = 0b01111111 = 0x7F */
    TEST_ASSERT_EQ(fb[0], 0x7F, "single black px (0,0): fb[0] must be 0x7F");
    /* Row 0, byte 1 (cols 8-15): all white */
    TEST_ASSERT_EQ(fb[1], 0xFF, "single black px (0,0): fb[1] must be 0xFF");
    /* Row 1, byte 0: all white */
    TEST_ASSERT_EQ(fb[25], 0xFF, "single black px (0,0): fb[25] (row1 byte0) must be 0xFF");

    /* ── Case 4: single black pixel at (7,0) — LSB of first byte ── */
    memset(pixels, 1, sizeof(pixels));
    pixels[7] = 0;   /* col=7, row=0 */
    memset(fb, 0x00, EINK_FB_SIZE);
    eink_pack_lvgl_area(&area, pixels, fb);
    /* Col 7 = bit (7-7) = bit 0 of byte 0 → clear. Others set. */
    TEST_ASSERT_EQ(fb[0], 0xFE, "single black px (7,0): fb[0] must be 0xFE");

    /* ── Case 5: single black pixel at (8,0) — MSB of second byte ── */
    memset(pixels, 1, sizeof(pixels));
    pixels[8] = 0;   /* col=8, row=0 */
    memset(fb, 0x00, EINK_FB_SIZE);
    eink_pack_lvgl_area(&area, pixels, fb);
    TEST_ASSERT_EQ(fb[0], 0xFF, "single black px (8,0): fb[0] unaffected");
    TEST_ASSERT_EQ(fb[1], 0x7F, "single black px (8,0): fb[1] must be 0x7F");

    /* ── Case 6: single black pixel at (0,1) — first byte of row 1 ── */
    memset(pixels, 1, sizeof(pixels));
    pixels[EINK_WIDTH] = 0;   /* col=0, row=1 → index 200 */
    memset(fb, 0x00, EINK_FB_SIZE);
    eink_pack_lvgl_area(&area, pixels, fb);
    TEST_ASSERT_EQ(fb[0], 0xFF, "single black px (0,1): fb[0] (row0) unaffected");
    TEST_ASSERT_EQ(fb[25], 0x7F, "single black px (0,1): fb[25] (row1,byte0) must be 0x7F");

    /* ── Case 7: checkerboard — alternating 0xAA/0x55 per byte ── */
    /* Pixels: even col within byte = black (0), odd col = white (1).
     * Each byte: bit7=0, bit6=1, bit5=0, ... = 0b01010101 = 0x55 */
    for (int row = 0; row < EINK_HEIGHT; row++) {
        for (int col = 0; col < EINK_WIDTH; col++) {
            pixels[row * EINK_WIDTH + col] = (col % 2 == 0) ? 0 : 1;
        }
    }
    memset(fb, 0x00, EINK_FB_SIZE);
    eink_pack_lvgl_area(&area, pixels, fb);
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0x55) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "checkerboard (even=black): all fb bytes must be 0x55");
    }

    /* Inverse checkerboard: even col = white, odd = black → 0xAA per byte */
    for (int row = 0; row < EINK_HEIGHT; row++) {
        for (int col = 0; col < EINK_WIDTH; col++) {
            pixels[row * EINK_WIDTH + col] = (col % 2 == 0) ? 1 : 0;
        }
    }
    memset(fb, 0x00, EINK_FB_SIZE);
    eink_pack_lvgl_area(&area, pixels, fb);
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0xAA) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "checkerboard (odd=black): all fb bytes must be 0xAA");
    }

    /* ── Case 8: output size = EINK_FB_SIZE ───────────────────── */
    /* Implicit: the loop above touches all 5000 bytes. No buffer overrun
     * if static fb[EINK_FB_SIZE] is used (compiler checks). */
    TEST_ASSERT(EINK_FB_SIZE == 5000, "EINK_FB_SIZE must be 5000 bytes");

    /* ── Case 9: bit access helper self-check ──────────────────── */
    memset(pixels, 1, sizeof(pixels));
    pixels[0] = 0;
    memset(fb, 0x00, EINK_FB_SIZE);
    eink_pack_lvgl_area(&area, pixels, fb);
    TEST_ASSERT_EQ(fb_bit(fb, 0, 0), 0, "fb_bit(0,0) black pixel must read 0");
    TEST_ASSERT_EQ(fb_bit(fb, 1, 0), 1, "fb_bit(1,0) white pixel must read 1");
    TEST_ASSERT_EQ(fb_bit(fb, 0, 1), 1, "fb_bit(0,1) white pixel must read 1");
}
```

- [ ] **Step 3: Add to `test/CMakeLists.txt`**

In the `add_executable(test_runner ...)` list, add `test_eink_pack.c`. Also add the implementation file:

```cmake
add_executable(test_runner
    ... existing files ...
    test_eink_pack.c
    ../main/periph/eink/eink.c   # provides eink_pack_lvgl_area via TEST_HOST guard
    test_main.c
)
```

Add to `target_include_directories`:
```cmake
target_include_directories(test_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../boards/kase_v2_debug
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/periph/eink   # for eink.h
)
```

**Note:** `eink.c` included in the test runner will pull in `eink_pack_lvgl_area`. Since `eink.c` uses FreeRTOS/SPI/GPIO headers in other functions, those functions must be guarded with `#ifndef TEST_HOST` or be separately compiled. See Step 6 for how to extract the pure function.

- [ ] **Step 4: Add to `test/test_main.c`**

Add at the end of the extern declaration block:
```c
extern void test_eink_pack(void);
```

Add before the final `printf("Results")` line:
```c
test_eink_pack();
```

- [ ] **Step 5: Write `eink_pack_lvgl_area` implementation in `eink.c`**

Add before the `eink_init` function in `eink.c`:

```c
/* ── 1bpp packing: LVGL area → SSD1681 B&W RAM format ──────── */
/*
 * Converts LVGL's lv_color_t array (1 byte per pixel at LV_COLOR_DEPTH=1,
 * value 1=white / 0=black) into the SSD1681 packed 1bpp format:
 *   - MSB-first: leftmost pixel occupies bit 7 of each byte.
 *   - Bit=1 → white (SSD1681 native white convention).
 *   - Row stride: EINK_WIDTH/8 = 25 bytes per row.
 *   - Total output: EINK_FB_SIZE = 5000 bytes.
 *
 * With disp_drv.full_refresh=1, area always covers [0,0]–[199,199],
 * so the loop always writes all 5000 bytes. The area argument is kept
 * for future partial-refresh support.
 *
 * No FreeRTOS / GPIO / SPI deps — pure byte manipulation. Host-testable.
 */
void eink_pack_lvgl_area(const lv_area_t *area,
                         const lv_color_t *color_p,
                         uint8_t *fb_out)
{
    int x1 = area->x1, y1 = area->y1;
    int x2 = area->x2, y2 = area->y2;

    for (int row = y1; row <= y2; row++) {
        for (int col = x1; col <= x2; col++) {
            /* LVGL pixel index (row-major, full-width stride) */
            int px_idx = (row - y1) * (EINK_WIDTH) + (col - x1);
            /* SSD1681 byte index */
            int byte_idx = row * (EINK_WIDTH / 8) + col / 8;
            int bit_idx  = 7 - (col % 8);   /* MSB = leftmost pixel */

            /* LVGL: 1=white, 0=black. SSD1681: bit=1→white, bit=0→black. */
            uint8_t px = color_p[px_idx].full;   /* .full for 1bpp lv_color_t */
            if (px) {
                fb_out[byte_idx] |=  (1 << bit_idx);   /* white: set bit */
            } else {
                fb_out[byte_idx] &= ~(1 << bit_idx);   /* black: clear bit */
            }
        }
    }
}
```

**Important:** At `LV_COLOR_DEPTH=1`, `lv_color_t` is a struct with a single `full` field (1-bit value stored in 1 byte by the compiler). In the `TEST_HOST` stub in `eink.h`, `lv_color_t` is typedef'd as `uint8_t`, so `color_p[i]` works directly (no `.full`). To make the implementation compile under both, use a compile-time switch:

```c
#ifdef TEST_HOST
            uint8_t px = color_p[px_idx];
#else
            uint8_t px = color_p[px_idx].full;
#endif
```

Alternatively, verify at implementation time whether LVGL v8's 1bpp `lv_color_t.full` is accessible without the `.full` cast. Adjust if needed — the host test result is authoritative.

- [ ] **Step 6: Guard ESP-IDF-dependent code in `eink.c` for host compilation**

The test runner compiles `eink.c` with `-DTEST_HOST`. All functions that use FreeRTOS, GPIO, or SPI must be excluded:

```c
#ifndef TEST_HOST
#include "board.h"
#include "half_spi.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif /* TEST_HOST */

/* ... eink_pack_lvgl_area (no guard — pure, host-safe) ... */

#ifndef TEST_HOST
bool eink_init(void) { ... }
void eink_push(const uint8_t *fb) { ... }
void eink_clear(void) { ... }
static void eink_task(void *arg) { ... }
void eink_start(void) { ... }
#endif /* TEST_HOST */
```

Wrap the `static const char *TAG`, `s_eink_dev`, `s_fb`, `s_present`, `s_layer_idx_stub` statics and the `eink_send_cmd/eink_send_data` helpers inside `#ifndef TEST_HOST` as well. The only symbol visible to the test runner is `eink_pack_lvgl_area`.

- [ ] **Step 7: Run host tests — must pass before proceeding to T2**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -15
./test_runner 2>&1 | grep -E "eink_pack|FAIL|passed|failed"
```

Expected: all `test_eink_pack` assertions pass. **Do not proceed to Task 2 until this is green.** If a case fails, fix `eink_pack_lvgl_area` (not the test).

- [ ] **Step 8: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink.h main/periph/eink/eink.c \
        test/test_eink_pack.c test/CMakeLists.txt test/test_main.c
git commit -m "test(eink): eink_pack_lvgl_area 1bpp packing + host tests (TDD)"
```

---

## Task 2: Fill `eink_push()` — full 10-step SSD1681 command sequence

**Trap addressed:** Trap 2 (BUSY wait inside lock for SW reset only; main BUSY wait outside lock).

**Files:**
- Modify: `main/periph/eink/eink.c`

The existing stub in `eink_push()` sends a single `eink_send_cmd(0x12)` SW reset and immediately unlocks. This task replaces that stub with the full sequence. The `half_spi_lock/unlock` structure is preserved exactly.

- [ ] **Step 1: Read the current `eink_push` stub**

```bash
grep -n "eink_push\|STEP\|TODO\|unlock\|lock\|BUSY" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/periph/eink/eink.c
```

Confirm: `half_spi_lock()` at line ~N, stub SW reset, `(void)eink_send_data; (void)fb;`, `half_spi_unlock()`, then BUSY poll loop. Note the exact line numbers to replace precisely.

- [ ] **Step 2: Replace the stub body in `eink_push()` with the full sequence**

Replace everything between `half_spi_lock();` and `half_spi_unlock();` with:

```c
    /* ── Step 1: Software Reset (cmd 0x12) ───────────────────────
     * Restores all registers to OTP defaults before each full update.
     * BUSY goes HIGH briefly (~10 ms). Poll inside the lock (short). */
    eink_send_cmd(0x12);
    {
        int sw_wait = 0;
        while (gpio_get_level(BOARD_EINK_BUSY_GPIO) == 1 && sw_wait < 50) {
            vTaskDelay(pdMS_TO_TICKS(1));
            sw_wait++;
        }
    }

    /* ── Step 2: Driver Output Control (cmd 0x01) ────────────────
     * Gate lines: 200 rows → 200-1 = 0xC7. Scanning dir: 0 (default). */
    eink_send_cmd(0x01);
    {
        static const uint8_t d01[] = {0xC7, 0x00, 0x00};
        eink_send_data(d01, sizeof(d01));
    }

    /* ── Step 3: Border Waveform Control (cmd 0x3C) ─────────────
     * 0x05 = fix border at VSS (black). Prevents border flicker. */
    eink_send_cmd(0x3C);
    {
        static const uint8_t d3c[] = {0x05};
        eink_send_data(d3c, sizeof(d3c));
    }

    /* ── Step 4: Set RAM-X Address Start/End (cmd 0x44) ─────────
     * Col 0 to col 24 → 25 bytes × 8 bits = 200 pixels. */
    eink_send_cmd(0x44);
    {
        static const uint8_t d44[] = {0x00, 0x18};
        eink_send_data(d44, sizeof(d44));
    }

    /* ── Step 5: Set RAM-Y Address Start/End (cmd 0x45) ─────────
     * Y counts downward: start=199 (0x00C7), end=0 (0x0000). */
    eink_send_cmd(0x45);
    {
        static const uint8_t d45[] = {0xC7, 0x00, 0x00, 0x00};
        eink_send_data(d45, sizeof(d45));
    }

    /* ── Step 6: Set RAM-X Address Counter (cmd 0x4E) ───────────
     * Reset X pointer to column 0 before RAM write. */
    eink_send_cmd(0x4E);
    {
        static const uint8_t d4e[] = {0x00};
        eink_send_data(d4e, sizeof(d4e));
    }

    /* ── Step 7: Set RAM-Y Address Counter (cmd 0x4F) ───────────
     * Reset Y pointer to row 199 (top of display, RAM written top-down). */
    eink_send_cmd(0x4F);
    {
        static const uint8_t d4f[] = {0xC7, 0x00};
        eink_send_data(d4f, sizeof(d4f));
    }

    /* ── Step 8: Write B&W RAM (cmd 0x24) ───────────────────────
     * Send 5000-byte framebuffer. bit=1→white, bit=0→black (SSD1681 native).
     * MSB-first: byte[0] bit7 = pixel at (col=0, row=0). */
    eink_send_cmd(0x24);
    eink_send_data(fb, EINK_FB_SIZE);
    ESP_LOGI(TAG, "eink_push: SPI write complete (~10 ms)");

    /* ── Step 9: Display Update Control 2 (cmd 0x22) ────────────
     * 0xF7 = full update using default OTP LUT (no custom waveform table). */
    eink_send_cmd(0x22);
    {
        static const uint8_t d22[] = {0xF7};
        eink_send_data(d22, sizeof(d22));
    }

    /* ── Step 10: Master Activation (cmd 0x20) ───────────────────
     * No data. Triggers the ~2 s full refresh. BUSY goes HIGH immediately.
     * DO NOT poll BUSY here — release the SPI lock first. */
    eink_send_cmd(0x20);
```

Then preserve the existing `half_spi_unlock()` line and the BUSY poll loop exactly as-is. Remove the now-replaced `(void)eink_send_data; (void)fb;` suppressions — the function uses both arguments now.

- [ ] **Step 3: Verify build — half_right (the board with the panel)**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 | tail -15
```

Expected: `Project build complete.` If any `[-Wunused]` warnings appear, check that `eink_send_data` and `fb` are fully used (Step 8 removes both suppressions).

- [ ] **Step 4: Verify dongle build still green**

```bash
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 | tail -5
```

Expected: `Project build complete.` (dongle has `KASE_HAS_EINK=n` → `eink.c` not compiled → no impact).

- [ ] **Step 5: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink.c
git commit -m "feat(eink): eink_push() full 10-step SSD1681 command sequence"
```

---

## Task 3: LVGL 1bpp sdkconfig + CMake wiring — build green

**Trap addressed:** Trap 1 (LV_COLOR_DEPTH=1 applies to half builds only; keyboard builds unaffected).

**Files:**
- Modify: `sdkconfig.defaults.half_left`
- Modify: `sdkconfig.defaults.half_right`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Add LVGL 1bpp block to `sdkconfig.defaults.half_left`**

Append to the end of `sdkconfig.defaults.half_left`:

```ini
# --- LVGL: 1bpp monochrome for SSD1681 e-ink ---
# NOTE: This is HALF-build-only. Keyboard builds use LV_COLOR_DEPTH_16.
# The half has independent build dirs (build_half_left, build_half_right)
# and independent sdkconfig. No conflict with keyboard or dongle builds.
CONFIG_LV_COLOR_DEPTH_1=y
# CONFIG_LV_COLOR_DEPTH_16 is not set  (they are a Kconfig choice group)
CONFIG_LV_USE_LABEL=y
CONFIG_LV_FONT_MONTSERRAT_14=y
# LVGL internal heap: ~8 KB at 1bpp (core + label objects). Within budget.
```

- [ ] **Step 2: Append the same LVGL block to `sdkconfig.defaults.half_right`**

`sdkconfig.defaults.half_right` inherits from `kase_half_right/board.h` (different NRF channel/address) and has identical sdkconfig content to `half_left`. Use the Edit tool to append the same LVGL block verbatim to `sdkconfig.defaults.half_right` — do not replace the whole file.

```ini
# --- LVGL: 1bpp monochrome for SSD1681 e-ink ---
CONFIG_LV_COLOR_DEPTH_1=y
CONFIG_LV_USE_LABEL=y
CONFIG_LV_FONT_MONTSERRAT_14=y
```

- [ ] **Step 3: Check `main/CMakeLists.txt` for `lvgl` in priv_requires**

```bash
grep -n "lvgl\|KASE_HAS_EINK" /home/mae/Documents/GitHub/KaSe_firmware/main/CMakeLists.txt
```

If `lvgl` is not listed under `KASE_HAS_EINK`, add it:

```cmake
if(CONFIG_KASE_HAS_EINK)
    list(APPEND srcs "periph/eink/eink.c")
    list(APPEND priv_requires lvgl)   # raw LVGL v8 — managed component
endif()
```

The `eink_lvgl.c` source (added in T4) will be appended to this same block.

- [ ] **Step 4: Verify LVGL 1bpp builds for half_right**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 | tail -20
```

Expected: `Project build complete.` Watch for:
- `CONFIG_LV_COLOR_DEPTH_1 not found` → Kconfig name mismatch; run `idf.py menuconfig` on the half build and navigate to LVGL to find the correct Kconfig symbol.
- `undefined reference to lv_init` → `lvgl` not in priv_requires (fix Step 3).
- `LV_COLOR_DEPTH=1 conflicts with LV_COLOR_DEPTH=16` → base `sdkconfig.defaults` sets 16; the half-specific override must explicitly set 1. In ESP-IDF Kconfig, later definitions override earlier ones in the merge order (half-specific file is merged last). This should work; if not, add `# CONFIG_LV_COLOR_DEPTH_16 is not set` as a comment line (Kconfig ignores it but documents intent — the choice group mechanism handles mutual exclusion).

- [ ] **Step 5: Verify keyboard build (V2) is unaffected**

```bash
rm -f sdkconfig
idf.py -B build_v2 -DBOARD=kase_v2 build 2>&1 | tail -5
```

Expected: `Project build complete.` with `LV_COLOR_DEPTH=16` still in effect (keyboard sdkconfig unchanged).

- [ ] **Step 6: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add sdkconfig.defaults.half_left sdkconfig.defaults.half_right main/CMakeLists.txt
git commit -m "feat(eink): LVGL 1bpp sdkconfig for half builds + CMake lvgl dep"
```

---

## Task 4: Raw LVGL init — `eink_lvgl.h`, `eink_lvgl.c`, flush callback, tick, handler task

**Traps addressed:** Trap 1 (stripe buffer sizing), Trap 3 (`full_refresh=1`), Trap 4 (stack check).

**Files:**
- Create: `main/periph/eink/eink_lvgl.h`
- Create: `main/periph/eink/eink_lvgl.c`
- Modify: `main/CMakeLists.txt` (add `eink_lvgl.c` to KASE_HAS_EINK block)

- [ ] **Step 1: Write `main/periph/eink/eink_lvgl.h`**

```c
#pragma once
/*
 * eink_lvgl.h — Raw LVGL v8 integration for SSD1681 e-ink (KaSe half).
 *
 * Uses raw LVGL (no esp_lvgl_port). Rationale: SSD1681 has no esp_lcd driver
 * in this project; a direct flush callback is simpler and self-contained.
 *
 * Call order in half_scan_task (after eink_init() returns true):
 *   eink_lvgl_init();    // lv_init, draw buf, disp drv, tick timer, screen content
 *   eink_lvgl_start();   // create eink_lvgl_task (prio 3, core 0)
 *
 * Thread safety: for v1 (static screen), only eink_lvgl_task touches LVGL.
 * Plan Bricks-4 (dynamic content) must add a mutex before calling lv_label_set_text
 * from an ESP-NOW callback.
 */

/* Initialize LVGL, register 1bpp display driver, create static screen content.
 * Must be called AFTER eink_init() returns true and BEFORE eink_lvgl_start(). */
void eink_lvgl_init(void);

/* Create the LVGL handler task (eink_lvgl_task, prio 3, stack 4096, core 0).
 * Replaces the old eink_task polling loop. */
void eink_lvgl_start(void);
```

- [ ] **Step 2: Write `main/periph/eink/eink_lvgl.c`**

```c
/*
 * eink_lvgl.c — Raw LVGL v8 integration for SSD1681 e-ink panel.
 *
 * Architecture:
 *   - lv_init() + lv_disp_drv_register() with flush_cb → eink_push().
 *   - disp_drv.full_refresh = 1 → always flushes [0,0]–[199,199].
 *   - Draw buffer: 5000-byte packed 1bpp (one byte per 8 pixels).
 *   - Tick: esp_timer at 5 ms period → lv_tick_inc(5).
 *   - Handler: eink_lvgl_task calls lv_timer_handler in a loop.
 *   - Static screen: "KaSe" label + firmware version from esp_app_get_description().
 *
 * No esp_lvgl_port used (no esp_lcd panel handle for SSD1681 in this project).
 */

#include "eink_lvgl.h"
#include "eink.h"           /* eink_push, eink_pack_lvgl_area, EINK_WIDTH/HEIGHT/FB_SIZE */
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"   /* esp_app_get_description() */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "eink_lvgl";

/* ── Framebuffer (5000 bytes, static BSS) ───────────────────────
 * s_fb is the SSD1681 B&W RAM image: 1 byte per 8 pixels, MSB-first.
 * Initialized to 0xFF (all white) before first flush.
 * Shared between flush_cb (writes) and eink_push (reads). Single-task
 * access guaranteed because full_refresh=1 serializes flushes. */
static uint8_t s_fb[EINK_FB_SIZE];

/* ── LVGL draw buffer ───────────────────────────────────────────
 * Trap 1: at LV_COLOR_DEPTH=1, sizeof(lv_color_t)==1 (1 byte per pixel,
 * not 1 bit). An array of 200*200=40000 elements = 40 KB — too large.
 * Solution: use a packed raw buffer cast to lv_color_t*.
 * 5000 bytes covers the full panel at 1bpp (8 pixels per byte).
 * With full_refresh=1, lv_timer_handler calls flush_cb exactly once
 * per render cycle with the complete frame — no stripe accumulation needed. */
static uint8_t       s_lvgl_buf_raw[EINK_FB_SIZE];   /* 5000 bytes, BSS */
static lv_color_t   *s_lvgl_buf = (lv_color_t *)s_lvgl_buf_raw;

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;

/* ── Tick timer ─────────────────────────────────────────────────
 * Fires every 5 ms. Advances LVGL's internal clock.
 * Does NOT trigger redraws — only lv_timer_handler does that. */
static esp_timer_handle_t s_tick_timer = NULL;

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(5);
}

/* ── Flush callback ─────────────────────────────────────────────
 * Called by lv_timer_handler when LVGL has rendered a frame.
 * With full_refresh=1, this is called exactly once per full render cycle,
 * area = [0,0]–[EINK_WIDTH-1, EINK_HEIGHT-1].
 *
 * Steps:
 *   1. Pack LVGL's 1bpp pixel array into s_fb (MSB-first, white=1).
 *   2. If this is the last fragment (always true with full_refresh=1),
 *      push s_fb to the SSD1681 panel.
 *   3. Call lv_disp_flush_ready() — MUST be called unconditionally,
 *      even if eink_push is not called, or LVGL will deadlock. */
static void eink_lvgl_flush_cb(lv_disp_drv_t *drv,
                                const lv_area_t *area,
                                lv_color_t *color_p)
{
    /* Pack LVGL pixels into SSD1681 B&W RAM format */
    eink_pack_lvgl_area(area, color_p, s_fb);

    /* Push to panel on last fragment. With full_refresh=1, every flush IS the last. */
    if (lv_disp_flush_is_last(drv)) {
        eink_push(s_fb);
    }

    /* MUST call unconditionally — LVGL blocks on this to issue next render */
    lv_disp_flush_ready(drv);
}

/* ── LVGL handler task ──────────────────────────────────────────
 * Calls lv_timer_handler() in a loop. Returns the delay until the next
 * LVGL timer fires (capped at 50 ms). At prio 3, lower than rf_rx_task
 * and half_scan_task — e-ink latency is irrelevant vs. key event latency.
 *
 * Trap 4: stack starts at 4096 bytes. Log uxTaskGetStackHighWaterMark
 * and bump to 6144 in CMakeLists if < 512 free. */
static void eink_lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "eink_lvgl_task started");

    for (;;) {
        uint32_t sleep_ms = lv_timer_handler();
        /* Cap at 50 ms: stay responsive to future invalidations */
        if (sleep_ms == 0 || sleep_ms > 50) sleep_ms = 50;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));

        /* Stack headroom check — log once after first render */
        static bool s_stack_checked = false;
        if (!s_stack_checked) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "eink_lvgl_task stack HWM: %u words free", (unsigned)hwm);
            if (hwm < 128) {  /* < 512 bytes */
                ESP_LOGW(TAG, "STACK LOW — bump eink_lvgl_task stack to 6144 in CMakeLists");
            }
            s_stack_checked = true;
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────*/

void eink_lvgl_init(void)
{
    /* Initialize LVGL core */
    lv_init();

    /* Initialize s_fb to all-white before first flush */
    memset(s_fb, 0xFF, EINK_FB_SIZE);

    /* Draw buffer: single buffer, full panel, 5000 bytes at 1bpp.
     * Second arg NULL = no double-buffer (not needed for e-ink).
     * Size arg = number of lv_color_t elements = EINK_FB_SIZE (one per pixel byte). */
    lv_disp_draw_buf_init(&s_draw_buf, s_lvgl_buf, NULL, EINK_FB_SIZE);

    /* Display driver */
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.flush_cb     = eink_lvgl_flush_cb;
    s_disp_drv.hor_res      = EINK_WIDTH;    /* 200 */
    s_disp_drv.ver_res      = EINK_HEIGHT;   /* 200 */
    /* Trap 3: full_refresh=1 ensures LVGL always sends the full 200×200 frame.
     * Required for SSD1681 — partial RAM writes are not used in v1. */
    s_disp_drv.full_refresh = 1;
    lv_disp_drv_register(&s_disp_drv);

    ESP_LOGI(TAG, "LVGL init OK, display registered (%dx%d, 1bpp)", EINK_WIDTH, EINK_HEIGHT);

    /* Tick timer: 5 ms period → lv_tick_inc(5) */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, 5 * 1000));   /* 5 ms in µs */

    /* ── Static screen content ───────────────────────────────────
     * Created here so LVGL renders on the first lv_timer_handler() call.
     * For a static screen, LVGL invalidates exactly once at creation.
     * After the first flush, no further invalidations occur (v1 scope). */
    lv_obj_t *scr = lv_scr_act();

    /* White background, black text (maps to SSD1681 white bg + black pixels) */
    lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* "KaSe" label */
    lv_obj_t *label_name = lv_label_create(scr);
    lv_label_set_text(label_name, "KaSe");
    lv_obj_set_style_text_color(label_name, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(label_name, 60, 80);   /* centered-ish on 200×200 */

    /* Firmware version label (from git describe via ESP-IDF) */
    const esp_app_desc_t *desc = esp_app_get_description();
    lv_obj_t *label_ver = lv_label_create(scr);
    lv_label_set_text(label_ver, desc->version);
    lv_obj_set_style_text_color(label_ver, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(label_ver, 60, 100);

    ESP_LOGI(TAG, "static screen created: 'KaSe' + version '%s'", desc->version);
}

void eink_lvgl_start(void)
{
    /* Trap 4: start at 4096, check HWM after first render, bump to 6144 if needed */
    BaseType_t ret = xTaskCreatePinnedToCore(
        eink_lvgl_task, "eink_lvgl",
        4096,   /* bytes — monitor HWM and bump to 6144 if < 512 free */
        NULL,
        3,      /* priority: lower than rf_rx_task, half_scan_task */
        NULL,
        0);     /* core 0 */
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed for eink_lvgl_task");
    }
}
```

- [ ] **Step 3: Add `eink_lvgl.c` to `main/CMakeLists.txt`**

In the `KASE_HAS_EINK` block, extend the sources list:

```cmake
if(CONFIG_KASE_HAS_EINK)
    list(APPEND srcs
        "periph/eink/eink.c"
        "periph/eink/eink_lvgl.c"
    )
    list(APPEND priv_requires lvgl)
endif()
```

- [ ] **Step 4: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink_lvgl.h main/periph/eink/eink_lvgl.c main/CMakeLists.txt
git commit -m "feat(eink): eink_lvgl.c — raw LVGL v8 init, flush_cb, tick, handler task"
```

---

## Task 5: Static screen + retire `eink_task` — build green all boards

**Files:**
- Modify: `main/periph/eink/eink.c` (retire `eink_task`; repurpose `eink_start()`)
- Modify: `main/periph/eink/eink.h` (update `eink_start()` doc comment)

The current `eink_start()` creates `eink_task` (a 1 Hz polling loop). This task is replaced by `eink_lvgl_task` (created by `eink_lvgl_start()`). The caller site in `half_scan_task.c` calls `eink_start()` — this call is preserved; `eink_start()` is repurposed to call `eink_lvgl_init()` + `eink_lvgl_start()`.

- [ ] **Step 1: Repurpose `eink_start()` in `eink.c`**

Remove the `eink_task` static function entirely. Replace `eink_start()` body with:

```c
#include "eink_lvgl.h"   /* add this include near the top of eink.c */

void eink_start(void)
{
    /* Retired: eink_task (1 Hz polling loop). Replaced by LVGL handler task.
     * eink_lvgl_init() sets up LVGL, draw buffer, flush callback, tick timer,
     * and creates the static screen. eink_lvgl_start() creates the handler task. */
    eink_lvgl_init();
    eink_lvgl_start();
}
```

Remove `s_layer_idx_stub` (the stub for Plan Bricks-3 g_half_state) — it was only used by `eink_task`. If it is still needed as a future placeholder, keep it with a `TODO` comment; otherwise, remove it to silence `-Wunused`.

- [ ] **Step 2: Update `eink_start()` doc comment in `eink.h`**

Replace the old doc:
```c
/* Start the e-ink refresh task (prio 3, stack 4096, core 0).
 * Only call if eink_init() returned true.
 * The task refreshes the panel at ~1 Hz (or on content change in future). */
```

With:
```c
/* Initialize LVGL and start the e-ink LVGL handler task.
 * Only call if eink_init() returned true.
 * Calls eink_lvgl_init() (LVGL setup + static screen) then eink_lvgl_start()
 * (handler task, prio 3, stack 4096, core 0). */
```

- [ ] **Step 3: Build half_right (target with panel)**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 | tail -20
```

Expected: `Project build complete.` Common failures:
- `undefined reference to lv_disp_drv_register` → `lvgl` not in priv_requires (fix T3 Step 3).
- `undefined reference to esp_app_get_description` → add `app_update` or `esp_app_format` to priv_requires (ESP-IDF 5.5: `esp_app_desc.h` is in `esp_app_format` component). Check with `idf.py reconfigure` if the include path resolves.
- `lv_color_white is not defined` → Kconfig symbol issue; ensure `CONFIG_LV_COLOR_DEPTH_1=y` is active in the half build.

- [ ] **Step 4: Build half_left**

```bash
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 5: Build dongle**

```bash
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 | tail -5
```

Expected: `Project build complete.` (KASE_HAS_EINK=n → eink*.c not compiled → no LVGL on dongle).

- [ ] **Step 6: Run host tests — verify existing tests still pass**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner
```

Expected: all tests pass (including `test_eink_pack` from T1).

- [ ] **Step 7: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink.c main/periph/eink/eink.h
git commit -m "feat(eink): retire eink_task polling loop — eink_start() delegates to eink_lvgl"
```

---

## Task 6: Bench validation — flash half-right, visual check, SPI coexistence

**This task is hardware-only. No code changes. Execute on the bench with the half-right DevKitC + WeAct 1.54" SSD1681 panel.**

- [ ] **Step 1: Flash half-right**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
# Ensure sdkconfig matches half_right (rm if last build was different board)
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build
idf.py -B build_half_right -p /dev/ttyUSB0 flash monitor
```

- [ ] **Step 2: Verify boot log sequence**

Expected console output (in order):

```
I eink: SSD1681 e-ink panel detected — init OK
I eink_lvgl: LVGL init OK, display registered (200x200, 1bpp)
I eink_lvgl: static screen created: 'KaSe' + version '<vX.Y.Z-...>'
I eink_lvgl: eink_lvgl_task started
I eink_lvgl: eink_lvgl_task stack HWM: <N> words free
I eink: eink_push: SPI write complete (~10 ms)
I eink: eink_push: BUSY cleared after <N> ms
```

If `eink_push: BUSY timeout after 3 s` appears instead, the panel is hung or not connected. Check SPI wiring and DC/RST/BUSY GPIO connections.

- [ ] **Step 3: Visual check — panel content**

After the BUSY timeout (~2 s from boot), the panel must display:
- White background
- "KaSe" label (Montserrat 14, black text) near center-left
- Firmware version string (e.g., `v0.4.2-3-gABCDEF`) below the "KaSe" label

If the panel shows all-white (blank): LVGL rendered but flush_cb produced 0xFF for all bytes — check `eink_pack_lvgl_area` polarity and the `lv_color_t.full` field access.

If the panel shows all-black: 0x00 bytes sent — check that `lv_color_white()` produces value 1 (not 0) at `LV_COLOR_DEPTH=1`.

If the panel shows garbled patterns: byte order or row stride issue in `eink_pack_lvgl_area`. Re-run the host test suite with the actual LVGL pixel output captured (add a debug dump).

- [ ] **Step 4: Stack headroom check — Trap 4**

Find the HWM log line:
```
I eink_lvgl: eink_lvgl_task stack HWM: <N> words free
```

If `N < 128` (< 512 bytes), increase the stack from 4096 to 6144 in `eink_lvgl_start()` and rebuild. This is the only code change permitted in T6.

- [ ] **Step 5: SPI coexistence regression test**

While the panel is refreshing (BUSY pin high, ~2 s window after activation):
1. Press several keys on the half-right matrix.
2. Verify key events reach the dongle: check dongle console for `rf_rx: PKT_KEY row=...` logs, or observe OS keystrokes in a text editor.
3. Key events must not stall during panel refresh. The BUSY wait is outside the SPI lock — NRF24 TX must proceed freely.

If key events stall: the `half_spi_unlock()` / BUSY-poll ordering in `eink_push` has been reversed. Inspect `eink.c` and restore the lock-release-before-poll sequence.

- [ ] **Step 6: Commit (stack bump only if needed)**

```bash
# Only if stack was bumped in Step 4:
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink_lvgl.c
git commit -m "fix(eink): bump eink_lvgl_task stack to 6144 (HWM < 512 bytes)"
# Otherwise no commit needed for T6.
```

---

## Self-review checklist

- [ ] `eink_pack_lvgl_area` host tests pass on all 9 cases before flush_cb is written (T1 Step 7).
- [ ] `half_spi_unlock()` is called BEFORE the BUSY poll in `eink_push()` — never after. Locking order unchanged from skeleton.
- [ ] SW reset BUSY wait in Step 1 of `eink_push` is INSIDE the lock (short, ~10 ms). Main BUSY wait after Step 10 is OUTSIDE the lock.
- [ ] `full_refresh = 1` set on `lv_disp_drv_t` before `lv_disp_drv_register()`.
- [ ] `lv_disp_flush_ready(drv)` called unconditionally in `flush_cb` (not only on last fragment).
- [ ] Static screen labels and styles set BEFORE `eink_lvgl_start()` (before first `lv_timer_handler()` call).
- [ ] `lv_color_white()` applied to screen background, `lv_color_black()` to label text — verify polarity on bench.
- [ ] LVGL handler task priority = 3 (lower than NRF24/scan tasks — e-ink latency is non-critical).
- [ ] `uxTaskGetStackHighWaterMark` logged in `eink_lvgl_task` after first render; bumped if < 512 bytes free.
- [ ] `sdkconfig.defaults.half_left` AND `sdkconfig.defaults.half_right` both updated with `CONFIG_LV_COLOR_DEPTH_1=y`.
- [ ] Dongle build green (KASE_HAS_EINK=n — no LVGL, no eink symbols).
- [ ] Keyboard V2 build green (`LV_COLOR_DEPTH=16` unchanged — independent sdkconfig).
- [ ] Host tests green (all existing tests + `test_eink_pack`).
- [ ] `esp_app_get_description()` used for version string — no hardcoded version.
- [ ] `eink_task` removed from `eink.c` — no dead code left.
- [ ] `(void)eink_send_data; (void)fb;` suppressions removed from `eink_push()` (both used in full sequence).

---

## Out of scope (Plan Bricks-4 and later)

| Feature | Notes |
|---------|-------|
| Dynamic content from ESP-NOW | `g_half_state` (layer name, modifiers, battery) — Plan Bricks-4. Requires LVGL mutex for multi-task access. |
| Partial refresh | SSD1681 supports partial update with custom LUT. Deferred — requires ghosting management. |
| Deep-sleep / power gating | Panel can be powered down between refreshes. Deferred to power-management brick. |
| Animations / multi-screen UI | Only static screen in v1. |
| LVGL mutex for concurrent updates | Not needed in v1 (single task). Required in Plan Bricks-4 before `lv_label_set_text` from ESP-NOW callback. Pattern: `xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY)` → LVGL call → `xSemaphoreGive`. |
| `esp_lvgl_port` fallback | If raw LVGL tick causes issues (esp_timer interaction on ESP-IDF 5.5), add a thin `esp_lcd` panel stub + use `esp_lvgl_port_add_disp()`. Try raw first. |
