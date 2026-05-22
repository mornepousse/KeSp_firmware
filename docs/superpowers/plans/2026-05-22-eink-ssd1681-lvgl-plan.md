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

**Trap 1 — LVGL v8 1bpp draw buffer sizing and packing approach.**
`sizeof(lv_color_t) == 1` at `LV_COLOR_DEPTH=1` (LVGL uses 1 byte per pixel in its draw buffer, not 1 bit). A full-frame draw buffer of `EINK_WIDTH * EINK_HEIGHT = 40000` elements allocates 40 KB — too large. A 5000-element buffer covers only 5000 pixels — undersized.

**The correct approach for this driver is `set_px_cb` + `rounder_cb` (canonical LVGL v8 monochrome recipe):**
- Use a small stripe draw buffer (e.g., `EINK_WIDTH * 10 = 2000` elements = 2000 bytes).
- Register `disp_drv.set_px_cb` to pack each pixel directly into the 5000-byte `s_fb` (MSB-first, bit=1→white) as LVGL renders.
- Register `disp_drv.rounder_cb` to byte-align the dirty area on the x-axis (required because SSD1681 RAM writes are byte-granular, not pixel-granular).
- `flush_cb` does NO packing — it only calls `eink_push(s_fb)` when `lv_disp_flush_is_last(drv)` is true, then `lv_disp_flush_ready(drv)`.

With `set_px_cb`, LVGL bypasses its normal draw buffer write path and calls the callback for every pixel. The draw buffer still needs to exist (LVGL needs it internally) but is never interpreted as a packed framebuffer.

> **Spec deviation (Section 5.3 / Section 5.5):** The spec proposes a `5000-byte s_lvgl_buf_raw` cast to `lv_color_t*` passed as the draw buffer, with `eink_pack_lvgl_area()` called in `flush_cb` to repack LVGL's output. This is incorrect: LVGL renders unpacked pixels (1 byte each at 1bpp depth) into the draw buffer, so passing 5000 elements covers only 5000 of the 40000 pixels. The cast does not change the draw buffer semantics. The `set_px_cb` approach below supersedes the spec's draw buffer and `eink_pack_lvgl_area` design. The `eink_fb_set_px()` helper tested in T1 replaces `eink_pack_lvgl_area()`.

**Trap 2 — Polarity alignment.**
SSD1681 convention: bit=1 → white, bit=0 → black. LVGL v8 at `LV_COLOR_DEPTH=1`: `lv_color_white()` has `.full == 1`, `lv_color_black()` has `.full == 0`. In `set_px_cb`, the mapping is direct: `color.full == 1` → set the bit in `s_fb`. The host test (T1) must confirm: all-white render produces `s_fb[i] == 0xFF` for all i. Do not proceed to T3 without this passing.

**Trap 3 — `full_refresh = 1` is mandatory.**
Without `drv.full_refresh = 1`, LVGL sends only the dirty region on re-renders. With `set_px_cb`, LVGL calls `flush_cb` once per rendered stripe with `is_last=false`, and once more when the whole frame is done with `is_last=true`. The SSD1681 does a full RAM push on every `eink_push()` call — calling `eink_push` on an incomplete `s_fb` would push a partial frame. Always set `full_refresh = 1` on the `lv_disp_drv_t` to guarantee a single pass of the full panel before the last `flush_cb`.

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

9. **`eink_fb_set_px` must be non-static and declared in `eink.h` under a `TEST_HOST` guard** so the host test can link it without any ESP-IDF headers. It has zero FreeRTOS/GPIO/SPI deps — pure bit manipulation. This function is called from `set_px_cb` in `eink_lvgl.c` and is the only packing logic in the whole pipeline.

10. **LVGL heap: ~8 KB for v8 core + label + screen at 1bpp.** The half has 512 KB SRAM. Static BSS adds `s_fb` (5000 bytes, already present in `eink.c`) + `s_lvgl_buf` stripe (2000 bytes, in `eink_lvgl.c`) + task stack (4–6 KB). All within budget.

---

## File Structure

### Created

| Path | Responsibility |
|------|----------------|
| `main/periph/eink/eink_lvgl.c` | Raw LVGL init, disp driver, flush_cb, tick timer, handler task, static screen |
| `main/periph/eink/eink_lvgl.h` | Public API: `eink_lvgl_init()`, `eink_lvgl_start()` |
| `test/test_eink_pack.c` | Host tests for `eink_fb_set_px()` pixel packing — must pass before T4 (flush/set_px_cb) is written |

### Modified

| Path | Change |
|------|--------|
| `main/periph/eink/eink.c` | Fill `eink_push()` 10-step SSD1681 sequence; repurpose `eink_start()` to delegate to `eink_lvgl_start()` |
| `main/periph/eink/eink.h` | Expose `eink_fb_set_px()` declaration under `TEST_HOST` guard |
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

## Task 1: Host test — `eink_fb_set_px()` pure pixel packing function (TDD)

**Trap addressed:** Trap 1 (set_px_cb approach — pixel packing is per-pixel, not bulk), Trap 2 (polarity).

**Files:**
- Modify: `main/periph/eink/eink.h`
- Modify: `main/periph/eink/eink.c` (add `eink_fb_set_px` + `#ifndef TEST_HOST` guards)
- Create: `test/test_eink_pack.c`
- Modify: `test/CMakeLists.txt`
- Modify: `test/test_main.c`

**Rationale for TDD:** `eink_fb_set_px` is the only error-prone packing piece that can be exercised on the host before touching the SPI or LVGL stack. With the `set_px_cb` approach, this single function is the entire packing logic — `set_px_cb` calls it for each pixel. Write the test first, implement the function to make it pass, then wire `set_px_cb` around it. The function lives in `eink.c` (all ESP-IDF-dependent code guarded by `#ifndef TEST_HOST`); the test runner compiles `eink.c` with `-DTEST_HOST` (already set in `test/CMakeLists.txt` global flags).

- [ ] **Step 1: Add `eink_fb_set_px` declaration to `eink.h`**

Add after the existing `eink_start()` declaration:

```c
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
```

No LVGL types in this signature — pure C. No `#ifdef TEST_HOST` type stubs needed in the header.

- [ ] **Step 2: Write `test/test_eink_pack.c`**

Write the test BEFORE the implementation (TDD). The assertions define the contract.
The test exercises `eink_fb_set_px` by painting all pixels via the function, then checking the result byte-by-byte.

```c
/*
 * test_eink_pack.c — Host tests for eink_fb_set_px() 1bpp pixel packing.
 *
 * Run: cd test/build && cmake .. && make && ./test_runner
 *
 * Contract under test:
 *   eink_fb_set_px(fb, col, row, is_white)
 *     → fb[row*25 + col/8] bit (7 - col%8) = is_white
 *   White (is_white=1) → bit set to 1 (SSD1681 white).
 *   Black (is_white=0) → bit cleared to 0 (SSD1681 black).
 *   MSB-first: pixel at col=0 is bit 7, col=7 is bit 0, col=8 is next byte bit 7.
 *   Total framebuffer: EINK_FB_SIZE = 5000 bytes (200×200 / 8).
 */
#define TEST_HOST
#include "test_framework.h"
#include "../main/periph/eink/eink.h"
#include <string.h>

/* Helper: read back a single bit from the packed framebuffer */
static int fb_bit(const uint8_t *fb, int col, int row)
{
    int byte_idx = row * (EINK_WIDTH / 8) + col / 8;
    int bit_idx  = 7 - (col % 8);
    return (fb[byte_idx] >> bit_idx) & 1;
}

/* Helper: paint every pixel via eink_fb_set_px and check */
static void paint_all(uint8_t *fb, int is_white)
{
    memset(fb, is_white ? 0x00 : 0xFF, EINK_FB_SIZE);   /* pre-fill with opposite */
    for (int row = 0; row < EINK_HEIGHT; row++) {
        for (int col = 0; col < EINK_WIDTH; col++) {
            eink_fb_set_px(fb, col, row, is_white);
        }
    }
}

void test_eink_pack(void)
{
    TEST_SUITE("eink_fb_set_px");

    static uint8_t fb[EINK_FB_SIZE];

    /* ── Case 1: EINK_FB_SIZE sanity ─────────────────────────── */
    TEST_ASSERT(EINK_FB_SIZE == 5000, "EINK_FB_SIZE must be 5000 bytes");

    /* ── Case 2: all-white → s_fb all 0xFF ───────────────────── */
    paint_all(fb, 1);
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0xFF) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "all-white: fb must be 0xFF throughout");
    }

    /* ── Case 3: all-black → s_fb all 0x00 ───────────────────── */
    paint_all(fb, 0);
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0x00) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "all-black: fb must be 0x00 throughout");
    }

    /* ── Case 4: single black pixel at (0,0), all others white ── */
    /* Start all-white via paint_all, then set (0,0) black. */
    paint_all(fb, 1);
    eink_fb_set_px(fb, 0, 0, 0);
    /* Col 0 = bit 7 of byte 0. Black → bit 7 cleared.
     * Expected: fb[0] = 0b01111111 = 0x7F */
    TEST_ASSERT_EQ(fb[0], 0x7F, "single black (0,0): fb[0] must be 0x7F");
    TEST_ASSERT_EQ(fb[1], 0xFF, "single black (0,0): fb[1] must be 0xFF");
    TEST_ASSERT_EQ(fb[25], 0xFF, "single black (0,0): fb[25] (row1,byte0) must be 0xFF");

    /* ── Case 5: single black pixel at (7,0) — LSB of byte 0 ─── */
    paint_all(fb, 1);
    eink_fb_set_px(fb, 7, 0, 0);
    /* Col 7 = bit (7-7) = bit 0 of byte 0 → cleared. */
    TEST_ASSERT_EQ(fb[0], 0xFE, "single black (7,0): fb[0] must be 0xFE");

    /* ── Case 6: single black pixel at (8,0) — MSB of byte 1 ─── */
    paint_all(fb, 1);
    eink_fb_set_px(fb, 8, 0, 0);
    TEST_ASSERT_EQ(fb[0], 0xFF, "single black (8,0): fb[0] unaffected");
    TEST_ASSERT_EQ(fb[1], 0x7F, "single black (8,0): fb[1] must be 0x7F");

    /* ── Case 7: single black pixel at (0,1) — start of row 1 ── */
    paint_all(fb, 1);
    eink_fb_set_px(fb, 0, 1, 0);
    /* Row 1, byte 0 = fb[25]. Col 0 = bit 7. */
    TEST_ASSERT_EQ(fb[0],  0xFF, "single black (0,1): fb[0] (row0) unaffected");
    TEST_ASSERT_EQ(fb[25], 0x7F, "single black (0,1): fb[25] (row1,byte0) must be 0x7F");

    /* ── Case 8: single white pixel at (0,0) on black background ── */
    paint_all(fb, 0);
    eink_fb_set_px(fb, 0, 0, 1);
    TEST_ASSERT_EQ(fb[0], 0x80, "single white (0,0) on black: fb[0] must be 0x80");

    /* ── Case 9: checkerboard (even col = black) → all bytes 0x55 ── */
    /* bit7=0(black), bit6=1(white), bit5=0, bit4=1, ... = 0x55 */
    memset(fb, 0x00, EINK_FB_SIZE);
    for (int row = 0; row < EINK_HEIGHT; row++) {
        for (int col = 0; col < EINK_WIDTH; col++) {
            eink_fb_set_px(fb, col, row, (col % 2 != 0) ? 1 : 0);
        }
    }
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0x55) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "checkerboard (even=black): all bytes must be 0x55");
    }

    /* ── Case 10: inverse checkerboard (even col = white) → 0xAA ── */
    memset(fb, 0x00, EINK_FB_SIZE);
    for (int row = 0; row < EINK_HEIGHT; row++) {
        for (int col = 0; col < EINK_WIDTH; col++) {
            eink_fb_set_px(fb, col, row, (col % 2 == 0) ? 1 : 0);
        }
    }
    {
        int ok = 1;
        for (int i = 0; i < EINK_FB_SIZE; i++) {
            if (fb[i] != 0xAA) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "checkerboard (even=white): all bytes must be 0xAA");
    }

    /* ── Case 11: fb_bit helper self-check ─────────────────────── */
    paint_all(fb, 1);
    eink_fb_set_px(fb, 0, 0, 0);   /* (0,0) = black */
    TEST_ASSERT_EQ(fb_bit(fb, 0, 0), 0, "fb_bit(0,0) black must read 0");
    TEST_ASSERT_EQ(fb_bit(fb, 1, 0), 1, "fb_bit(1,0) white must read 1");
    TEST_ASSERT_EQ(fb_bit(fb, 0, 1), 1, "fb_bit(0,1) white must read 1");
}
```

- [ ] **Step 3: Add to `test/CMakeLists.txt`**

In the `add_executable(test_runner ...)` list, add `test_eink_pack.c` and the implementation file:

```cmake
add_executable(test_runner
    ... existing files ...
    test_eink_pack.c
    ../main/periph/eink/eink.c   # provides eink_fb_set_px via TEST_HOST guard
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

`eink.c` compiled with `-DTEST_HOST` exposes only `eink_fb_set_px`; all FreeRTOS/SPI/GPIO functions are behind `#ifndef TEST_HOST` guards (Step 6).

- [ ] **Step 4: Add to `test/test_main.c`**

```c
extern void test_eink_pack(void);
```

```c
test_eink_pack();
```

- [ ] **Step 5: Write `eink_fb_set_px` implementation in `eink.c`**

Add before the `eink_init` function (outside any `#ifndef TEST_HOST` guard — this function must be reachable by the test runner):

```c
/* ── 1bpp pixel packing: set one pixel in the SSD1681 B&W RAM image ── */
/*
 * Packs a single pixel into the packed 1bpp framebuffer.
 *
 * Layout (row-major, MSB-first):
 *   fb[row * (EINK_WIDTH/8) + col/8]  bit (7 - col%8) = is_white
 *
 * SSD1681 convention: bit=1 → white, bit=0 → black.
 * LVGL v8 at LV_COLOR_DEPTH=1: color.full==1 = white, color.full==0 = black.
 * Polarity is direct — no inversion needed.
 *
 * No FreeRTOS / GPIO / SPI deps. Called from eink_lvgl_set_px_cb (firmware)
 * and directly from host tests. Must remain outside TEST_HOST guards.
 */
void eink_fb_set_px(uint8_t *fb, int col, int row, int is_white)
{
    int byte_idx = row * (EINK_WIDTH / 8) + col / 8;
    int bit_idx  = 7 - (col % 8);   /* MSB = leftmost pixel in each byte */

    if (is_white) {
        fb[byte_idx] |=  (uint8_t)(1 << bit_idx);   /* white: set bit */
    } else {
        fb[byte_idx] &= ~(uint8_t)(1 << bit_idx);   /* black: clear bit */
    }
}
```

No `#ifdef TEST_HOST` needed inside this function — it has no ESP-IDF dependencies.

- [ ] **Step 6: Guard ESP-IDF-dependent code in `eink.c` for host compilation**

The test runner compiles `eink.c` with `-DTEST_HOST`. All functions that use FreeRTOS, GPIO, or SPI must be excluded. Wrap all existing includes and functions (except `eink_fb_set_px`) with `#ifndef TEST_HOST`:

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

/* eink_fb_set_px — no guard, host-safe, pure byte manipulation */
void eink_fb_set_px(uint8_t *fb, int col, int row, int is_white) { ... }

#ifndef TEST_HOST
static const char *TAG = "eink";
static spi_device_handle_t s_eink_dev = NULL;
static uint8_t s_fb[EINK_FB_SIZE];
static bool s_present = false;
static uint8_t s_layer_idx_stub = 0;

static void eink_send_cmd(uint8_t cmd) { ... }
static void eink_send_data(const uint8_t *data, size_t len) { ... }

bool eink_init(void) { ... }
void eink_push(const uint8_t *fb) { ... }
void eink_clear(void) { ... }
static void eink_task(void *arg) { ... }
void eink_start(void) { ... }
#endif /* TEST_HOST */
```

The `eink.h` include and the `#include <string.h>` (used by `eink_fb_set_px`'s callers and by `eink_init`'s `memset`) must remain outside the `#ifndef TEST_HOST` block — or add a bare `#include <string.h>` before the guard.

- [ ] **Step 7: Run host tests — must pass before proceeding to T2**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -15
./test_runner 2>&1 | grep -E "eink_pack|FAIL|passed|failed"
```

Expected: all `test_eink_pack` assertions pass (Cases 1–11). **Do not proceed to Task 2 until this is green.** If a case fails, fix `eink_fb_set_px` (not the test).

- [ ] **Step 8: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink.h main/periph/eink/eink.c \
        test/test_eink_pack.c test/CMakeLists.txt test/test_main.c
git commit -m "test(eink): eink_fb_set_px 1bpp pixel packing + host tests (TDD)"
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

**Trap addressed:** Trap 1 (LV_COLOR_DEPTH=1 is half-build-only; keyboard builds use LV_COLOR_DEPTH=16 and must remain unaffected). With `LV_COLOR_DEPTH=1` set, `set_px_cb` receives `lv_color_t` with a valid 1-bit `.full` field — enabling the direct polarity mapping in T4.

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

## Task 4: Raw LVGL init — `eink_lvgl.h`, `eink_lvgl.c`, set_px_cb, rounder_cb, flush_cb, tick, handler task

**Traps addressed:** Trap 1 (set_px_cb + small stripe draw buffer — avoids 40 KB full-frame buffer and broken cast), Trap 2 (direct polarity in set_px_cb), Trap 3 (`full_refresh=1`), Trap 4 (stack check).

**Files:**
- Create: `main/periph/eink/eink_lvgl.h`
- Create: `main/periph/eink/eink_lvgl.c`
- Modify: `main/CMakeLists.txt` (add `eink_lvgl.c` to KASE_HAS_EINK block)

**Architecture summary:** LVGL v8's canonical monochrome driver pattern uses `set_px_cb` + `rounder_cb`. With these set:
- LVGL allocates a small draw buffer (scratch space only — not a packed framebuffer).
- For each rendered pixel, LVGL calls `set_px_cb(drv, buf, buf_w, x, y, color, opa)` instead of writing to the draw buffer.
- `set_px_cb` calls `eink_fb_set_px(s_fb, x, y, color.full)` to pack the pixel directly into the 5000-byte `s_fb`.
- `rounder_cb` byte-aligns the dirty area so x1 is always a multiple of 8 and x2 is always `multiple_of_8 + 7` — required because SSD1681 RAM is byte-addressed and `eink_fb_set_px` handles individual bits within bytes.
- `flush_cb` does NO pixel packing — it simply calls `eink_push(s_fb)` when `lv_disp_flush_is_last(drv)` is true, then `lv_disp_flush_ready(drv)`.

Draw buffer size: `EINK_WIDTH * 10 = 2000` elements (2000 bytes at `LV_COLOR_DEPTH=1`, since `sizeof(lv_color_t)==1`). This covers 10 stripes of 200 pixels. LVGL renders in stripes and calls `set_px_cb` for each pixel. The draw buffer is only scratch space; it is never read by our code.

- [ ] **Step 1: Write `main/periph/eink/eink_lvgl.h`**

```c
#pragma once
/*
 * eink_lvgl.h — Raw LVGL v8 integration for SSD1681 e-ink (KaSe half).
 *
 * Monochrome driver recipe: set_px_cb + rounder_cb + flush_cb.
 * - set_px_cb packs each pixel directly into the 5000-byte s_fb (MSB-first, bit=1→white).
 * - rounder_cb byte-aligns the dirty area (x to multiples of 8).
 * - flush_cb calls eink_push(s_fb) on the last flush fragment, then lv_disp_flush_ready().
 * - Draw buffer: 2000-byte stripe (EINK_WIDTH × 10 rows) — scratch only, never interpreted.
 *
 * Uses raw LVGL (no esp_lvgl_port). Rationale: SSD1681 has no esp_lcd driver
 * in this project; a direct set_px_cb is simpler and self-contained.
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
 * eink_lvgl.c — Raw LVGL v8 monochrome integration for SSD1681 e-ink panel.
 *
 * Architecture (set_px_cb + rounder_cb recipe):
 *   - disp_drv.set_px_cb: packs each LVGL-rendered pixel into s_fb via
 *       eink_fb_set_px(s_fb, x, y, color.full). No bulk packing in flush_cb.
 *   - disp_drv.rounder_cb: byte-aligns the dirty area (x1 rounds down to
 *       multiple of 8, x2 rounds up to multiple_of_8 + 7).
 *   - disp_drv.flush_cb: calls eink_push(s_fb) on last fragment; always
 *       calls lv_disp_flush_ready(drv) — LVGL deadlocks without this.
 *   - disp_drv.full_refresh = 1: LVGL sends the full 200×200 frame each cycle,
 *       guaranteeing s_fb is fully populated before eink_push is called.
 *   - Draw buffer: EINK_WIDTH × 10 = 2000 lv_color_t elements (2000 bytes
 *       at LV_COLOR_DEPTH=1). Scratch space only — never interpreted by our code.
 *   - Tick: esp_timer 5 ms → lv_tick_inc(5).
 *   - Handler: eink_lvgl_task (lv_timer_handler loop, prio 3, core 0).
 *   - Static screen: "KaSe" + firmware version from esp_app_get_description().
 *
 * Why set_px_cb, not direct draw buffer packing:
 *   At LV_COLOR_DEPTH=1, sizeof(lv_color_t)==1 (1 byte per pixel in the draw
 *   buffer, not 1 bit). A full-frame draw buffer would need 40000 bytes (40 KB).
 *   A 5000-element draw buffer covers only 5000 of the 40000 pixels — undersized.
 *   set_px_cb bypasses the draw buffer layout entirely: LVGL calls it per pixel,
 *   and we pack directly into the 5000-byte s_fb without any intermediate buffer.
 *
 * No esp_lvgl_port used (no esp_lcd panel handle for SSD1681 in this project).
 */

#include "eink_lvgl.h"
#include "eink.h"           /* eink_push, eink_fb_set_px, EINK_WIDTH/HEIGHT/FB_SIZE */
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"   /* esp_app_get_description() */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "eink_lvgl";

/* ── Packed framebuffer — the only pixel storage (5000 bytes, BSS) ─
 * Layout: row-major, MSB-first, 25 bytes per row.
 * s_fb[row * 25 + col/8]  bit (7 - col%8) = 1→white, 0→black.
 * Written pixel-by-pixel via set_px_cb → eink_fb_set_px().
 * Pushed to SSD1681 RAM by eink_push() in flush_cb on last fragment.
 * Initialized to 0xFF (all white) in eink_lvgl_init(). */
static uint8_t s_fb[EINK_FB_SIZE];

/* ── LVGL draw buffer — scratch space only (2000 bytes, BSS) ────────
 * At LV_COLOR_DEPTH=1, sizeof(lv_color_t)==1 (1 byte per pixel).
 * 2000 elements = 2000 bytes = scratch for 10 rows of 200 pixels.
 * With set_px_cb registered, LVGL does NOT use this buffer to output
 * final pixel data — it calls set_px_cb per pixel instead.
 * The buffer is required by lv_disp_draw_buf_init but never read by us.
 *
 * LVGL v8 requires buf_size_px >= 2 × hor_res (minimum). 2000 >> 400.  */
#define EINK_LVGL_BUF_ROWS   10
static lv_color_t         s_lvgl_buf[EINK_WIDTH * EINK_LVGL_BUF_ROWS];   /* 2000 bytes */
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;

/* ── Tick timer ─────────────────────────────────────────────────────
 * Fires every 5 ms → lv_tick_inc(5). Does NOT trigger redraws. */
static esp_timer_handle_t s_tick_timer = NULL;

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(5);
}

/* ── set_px_cb — called by LVGL for every rendered pixel ────────────
 * Signature (LVGL v8 monochrome convention):
 *   void set_px_cb(lv_disp_drv_t *drv, uint8_t *buf, lv_coord_t buf_w,
 *                  lv_coord_t x, lv_coord_t y, lv_color_t color, lv_opa_t opa)
 *
 * We ignore buf and buf_w — s_fb is the authoritative framebuffer.
 * Polarity: color.full == 1 = white (lv_color_white), color.full == 0 = black.
 * SSD1681 native: bit=1→white, bit=0→black. Mapping is direct, no inversion. */
static void eink_lvgl_set_px_cb(lv_disp_drv_t *drv,
                                 uint8_t *buf,
                                 lv_coord_t buf_w,
                                 lv_coord_t x,
                                 lv_coord_t y,
                                 lv_color_t color,
                                 lv_opa_t opa)
{
    (void)drv;
    (void)buf;
    (void)buf_w;
    (void)opa;   /* no alpha blending on 1bpp — treat any non-zero alpha as opaque */
    eink_fb_set_px(s_fb, (int)x, (int)y, (int)color.full);
}

/* ── rounder_cb — align dirty area to byte boundaries ───────────────
 * SSD1681 RAM is byte-granular. eink_fb_set_px writes individual bits
 * within bytes. LVGL's dirty area must be byte-aligned so that no partial
 * byte is left stale when LVGL renders a stripe and calls flush_cb.
 *
 * With full_refresh=1, LVGL always sends [0,0]–[199,199], which is already
 * aligned. The rounder_cb is still registered as defensive measure for
 * future partial-refresh mode (Plan Bricks-4+). */
static void eink_lvgl_rounder_cb(lv_disp_drv_t *drv, lv_area_t *area)
{
    (void)drv;
    /* Round x1 DOWN to the nearest multiple of 8 */
    area->x1 = (area->x1 / 8) * 8;
    /* Round x2 UP to the last pixel of the byte containing x2 */
    area->x2 = (area->x2 / 8) * 8 + 7;
    if (area->x2 >= EINK_WIDTH) area->x2 = EINK_WIDTH - 1;
}

/* ── flush_cb — called by lv_timer_handler after each rendered stripe ─
 * With set_px_cb registered, pixels are already packed into s_fb.
 * This callback only needs to trigger eink_push on the last fragment
 * and acknowledge the flush to LVGL.
 *
 * full_refresh=1 guarantees is_last is true only after all 200 rows are
 * rendered — so eink_push always receives a complete 5000-byte s_fb.
 *
 * CRITICAL: lv_disp_flush_ready(drv) MUST be called unconditionally —
 * even if eink_push is skipped — or LVGL will deadlock waiting for the
 * ready signal before issuing the next render. */
static void eink_lvgl_flush_cb(lv_disp_drv_t *drv,
                                const lv_area_t *area,
                                lv_color_t *color_p)
{
    (void)area;
    (void)color_p;   /* pixels already in s_fb via set_px_cb — nothing to pack here */

    if (lv_disp_flush_is_last(drv)) {
        /* Full frame is in s_fb — push to SSD1681 panel */
        eink_push(s_fb);
    }

    /* Acknowledge to LVGL — MUST be unconditional */
    lv_disp_flush_ready(drv);
}

/* ── LVGL handler task ──────────────────────────────────────────────
 * lv_timer_handler() drives rendering + flush. Returns ms until next
 * LVGL timer fires (capped at 50 ms to remain responsive).
 * Priority 3 — lower than rf_rx_task and half_scan_task; e-ink latency
 * is irrelevant compared to key event latency.
 *
 * Trap 4: stack starts at 4096. Log HWM after first render; bump to 6144
 * in eink_lvgl_start() if < 512 bytes free (128 FreeRTOS words). */
static void eink_lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "eink_lvgl_task started");

    for (;;) {
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms == 0 || sleep_ms > 50) sleep_ms = 50;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));

        /* Stack headroom check — log once after first render completes */
        static bool s_stack_checked = false;
        if (!s_stack_checked) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "eink_lvgl_task stack HWM: %u words free", (unsigned)hwm);
            if (hwm < 128) {   /* < 512 bytes */
                ESP_LOGW(TAG, "STACK LOW — bump eink_lvgl_task stack to 6144");
            }
            s_stack_checked = true;
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────*/

void eink_lvgl_init(void)
{
    /* Initialize LVGL core */
    lv_init();

    /* Pre-fill s_fb with all-white (0xFF) before first render.
     * set_px_cb will overwrite individual bits as LVGL renders.
     * Without this, pixels LVGL does not explicitly paint are undefined. */
    memset(s_fb, 0xFF, EINK_FB_SIZE);

    /* Draw buffer: 2000-element stripe, single buffer (no double-buffer for e-ink).
     * Size = EINK_WIDTH × EINK_LVGL_BUF_ROWS = 200 × 10 = 2000 lv_color_t elements
     * = 2000 bytes at LV_COLOR_DEPTH=1. This is scratch space only;
     * set_px_cb bypasses it and writes directly to s_fb. */
    lv_disp_draw_buf_init(&s_draw_buf, s_lvgl_buf, NULL,
                          EINK_WIDTH * EINK_LVGL_BUF_ROWS);

    /* Display driver */
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.flush_cb     = eink_lvgl_flush_cb;
    s_disp_drv.set_px_cb    = eink_lvgl_set_px_cb;   /* per-pixel packing into s_fb */
    s_disp_drv.rounder_cb   = eink_lvgl_rounder_cb;  /* byte-align dirty area */
    s_disp_drv.hor_res      = EINK_WIDTH;    /* 200 */
    s_disp_drv.ver_res      = EINK_HEIGHT;   /* 200 */
    /* Trap 3: full_refresh=1 — LVGL sends the full 200×200 frame each cycle.
     * Guarantees s_fb is complete when flush_cb fires with is_last=true.
     * Required for SSD1681 (no partial RAM writes in v1). */
    s_disp_drv.full_refresh = 1;
    lv_disp_drv_register(&s_disp_drv);

    ESP_LOGI(TAG, "LVGL init OK, display registered (%dx%d, 1bpp, set_px_cb)",
             EINK_WIDTH, EINK_HEIGHT);

    /* Tick timer: 5 ms period → lv_tick_inc(5) */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, 5 * 1000));   /* µs */

    /* ── Static screen content ──────────────────────────────────────
     * Created here so LVGL renders on the first lv_timer_handler() call.
     * Static screen invalidates once at creation. After the first flush,
     * no further invalidations occur until screen content is explicitly changed. */
    lv_obj_t *scr = lv_scr_act();

    /* White background → s_fb byte = 0xFF (all bits set) via set_px_cb.
     * lv_color_white() has .full == 1 at LV_COLOR_DEPTH=1 → bit set = white. */
    lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* "KaSe" label — black text on white background */
    lv_obj_t *label_name = lv_label_create(scr);
    lv_label_set_text(label_name, "KaSe");
    lv_obj_set_style_text_color(label_name, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(label_name, 60, 80);

    /* Firmware version from git describe (set by ESP-IDF at build time) */
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
        4096,   /* bytes — monitor HWM and bump to 6144 if < 512 bytes free */
        NULL,
        3,      /* priority: lower than rf_rx_task and half_scan_task */
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

- [ ] **Step 4: Verify build — half_right**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 | tail -20
```

Expected: `Project build complete.` Common failures at this step:
- `'lv_disp_drv_t' has no member named 'set_px_cb'` → verify LVGL version is v8.x (`lvgl/lvgl: ^8` in idf_component.yml). In LVGL v9, `set_px_cb` was removed; the equivalent is `render_start_cb` + direct buffer manipulation.
- `undefined reference to eink_fb_set_px` → check `eink.h` declaration is outside `TEST_HOST` guards and `eink.c` function definition is outside `#ifndef TEST_HOST` block.
- `undefined reference to eink_push` → `eink.h` not included, or `lvgl` not in priv_requires (fix T3 Step 3).

- [ ] **Step 5: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink_lvgl.h main/periph/eink/eink_lvgl.c main/CMakeLists.txt
git commit -m "feat(eink): eink_lvgl.c — set_px_cb monochrome driver, flush_cb, tick, handler task"
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
- `undefined reference to eink_fb_set_px` → check `eink.h` declaration is NOT inside a `TEST_HOST` guard and `eink.c` function is NOT inside `#ifndef TEST_HOST`.
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

If the panel shows all-white (blank): `set_px_cb` was called but `s_fb` stayed 0xFF — check `eink_fb_set_px` black-pixel path (the `&= ~(1 << bit)` branch). Alternatively, `set_px_cb` was not registered on `lv_disp_drv_t` and LVGL fell back to writing the draw buffer (which is never read by our flush_cb).

If the panel shows all-black: 0x00 bytes in `s_fb` — check that `lv_color_white()` has `.full == 1` at `LV_COLOR_DEPTH=1`. Also verify `s_fb` is pre-filled with 0xFF before first render; if `memset(s_fb, 0xFF, EINK_FB_SIZE)` is missing, unpainted pixels default to 0x00 (black).

If the panel shows garbled patterns: byte order or row-stride issue in `eink_fb_set_px`. Re-run the host test suite (T1) with the failing pixel coordinate logged from `set_px_cb` to pinpoint the mismatch. Check `rounder_cb` is byte-aligning x1 correctly.

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

- [ ] `eink_fb_set_px` host tests pass on all 11 cases before `set_px_cb` is wired (T1 Step 7).
- [ ] `half_spi_unlock()` is called BEFORE the BUSY poll in `eink_push()` — never after. Locking order unchanged from skeleton.
- [ ] SW reset BUSY wait in Step 1 of `eink_push` is INSIDE the lock (short, ~10 ms). Main BUSY wait after Step 10 is OUTSIDE the lock.
- [ ] `full_refresh = 1` set on `lv_disp_drv_t` before `lv_disp_drv_register()`.
- [ ] `set_px_cb` registered on `lv_disp_drv_t` — calls `eink_fb_set_px(s_fb, x, y, color.full)`.
- [ ] `rounder_cb` registered on `lv_disp_drv_t` — byte-aligns x1 down and x2 up to 8-pixel boundaries.
- [ ] `flush_cb` does NO pixel packing — only calls `eink_push(s_fb)` on `lv_disp_flush_is_last()`, then `lv_disp_flush_ready()` unconditionally.
- [ ] `lv_disp_flush_ready(drv)` called unconditionally in `flush_cb` (deadlock if omitted).
- [ ] Draw buffer sized to `EINK_WIDTH * EINK_LVGL_BUF_ROWS` (2000 elements = 2000 bytes) — not 5000, not 40000.
- [ ] `s_fb` pre-filled with 0xFF (all white) before first render so unpainted pixels default to white.
- [ ] Static screen labels and styles set BEFORE `eink_lvgl_start()` (before first `lv_timer_handler()` call).
- [ ] `lv_color_white()` (.full==1) applied to screen background, `lv_color_black()` (.full==0) to label text — polarity confirmed by T1 host tests.
- [ ] LVGL handler task priority = 3 (lower than NRF24/scan tasks — e-ink latency is non-critical).
- [ ] `uxTaskGetStackHighWaterMark` logged in `eink_lvgl_task` after first render; bumped to 6144 if < 512 bytes free.
- [ ] `sdkconfig.defaults.half_left` AND `sdkconfig.defaults.half_right` both updated with `CONFIG_LV_COLOR_DEPTH_1=y`.
- [ ] Dongle build green (KASE_HAS_EINK=n — no LVGL, no eink symbols).
- [ ] Keyboard V2 build green (`LV_COLOR_DEPTH=16` unchanged — independent sdkconfig).
- [ ] Host tests green (all existing tests + `test_eink_pack` 11 cases).
- [ ] `esp_app_get_description()` used for version string — no hardcoded version.
- [ ] `eink_task` removed from `eink.c` — no dead code left.
- [ ] `(void)eink_send_data; (void)fb;` suppressions removed from `eink_push()` (both used in full sequence).
- [ ] `eink_fb_set_px` function body is OUTSIDE any `#ifndef TEST_HOST` guard in `eink.c` — it must compile in both firmware and test builds.

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
