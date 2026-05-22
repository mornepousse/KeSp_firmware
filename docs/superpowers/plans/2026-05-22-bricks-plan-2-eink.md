# Plan Bricks-2 — E-ink SSD1681 Skeleton (SPI Arbitration + Refresh Task)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the e-ink SSD1681 skeleton to the KaSe half firmware: SPI2 device
registration, GPIO config, reset pulse, BUSY-pin probe, a 5000-byte static framebuffer,
`eink_push()` with proper SPI lock/unlock ordering (unlock before BUSY wait), and a
low-rate refresh task. SSD1681 command sequences and 1bpp rendering are explicitly stubbed.
End state: half_left + half_right + dongle all build green after this plan.

**Architecture:**
- `eink.h/c` lives in `main/periph/eink/`. It adds a second `spi_device_t` on `SPI2_HOST`
  (CS=GPIO18), distinct from the NRF24 device (CS=GPIO35). Both share the same bus handle
  initialized by `rf_driver_init_tx`.
- `half_spi_lock/unlock` (from Plan Bricks-1) gates all SPI transactions. The lock is
  released BEFORE the BUSY wait (~1-2 s), so NRF24 can send key events uninterrupted during
  panel refresh. This decoupling is the key design property.
- `eink_init()` is called from `half_scan_task` after NRF init and trackpad init (SPI bus
  must already exist when we call `spi_bus_add_device`).
- The 5000-byte framebuffer (`s_fb[EINK_FB_SIZE]`) is a static BSS array — no malloc.
- `eink_task` runs at 1 Hz (configurable), renders half_state into `s_fb` (stub), calls
  `eink_push`. `half_state` is a shared struct initialized in `espnow_info.c` (Plan Bricks-3);
  for now `eink_task` reads zeroed global values directly without a mutex (safe: the mutex is
  added in Plan Bricks-3 when the writer (ESP-NOW) is wired).

**Tech Stack:** ESP-IDF 5.5, `esp_driver_spi` (already required by RF_TX), `esp_driver_gpio`,
FreeRTOS. No new external components. LVGL is NOT used (display is too simple; direct 1bpp
rendering deferred to stub completion).

**Spec reference:** `docs/superpowers/specs/2026-05-22-half-peripherals-espnow-design.md`
Sections 4 (half_spi), 6 (e-ink), Annex A/B/C.

**Depends on:** Plan Bricks-1 (half_spi.h/c exists and is included in CMake; board.h has
BOARD_EINK_* defines; Kconfig has KASE_HAS_EINK=y).

**Build targets:** Same as Plan Bricks-1. Always `rm -f sdkconfig` before switching board.

**Hardware (from spec Annex C and `~/Documents/PCB-esp/CLAUDE.md`):**

| Signal   | GPIO    | Notes                                           |
|----------|---------|-------------------------------------------------|
| CS_DISP  | GPIO18  | SPI chip select, active-low (BOARD_EINK_CS_GPIO)  |
| DC_DISP  | GPIO12  | Data/Command: H=data, L=command                 |
| RES_DISP | GPIO17  | Reset active-low; pulse sequence at boot        |
| BUSY     | GPIO1   | H=panel busy, L=ready for next command          |
| MOSI     | GPIO48  | SPI2 shared with NRF24                          |
| MISO     | GPIO47  | SPI2 shared (not used by SSD1681 write path)    |
| SCK      | GPIO45  | SPI2 shared                                     |

E-ink panel: WeAct 1.54" SSD1681, 200×200 px, 1bpp (1 bit per pixel, white=1, black=0 in
SSD1681 convention). Full refresh only in this brick; partial refresh is out of scope.

---

## Learned facts baked in

1. **`spi_bus_add_device` is safe to call on an already-initialized bus.** `SPI2_HOST` is
   initialized by `rf_driver_init_tx()` (which calls `spi_bus_initialize`). Adding a second
   device (the SSD1681) uses `spi_bus_add_device` on the same host — this is the standard
   ESP-IDF multi-device pattern. No re-initialization needed. If `spi_bus_initialize` is
   called again, it returns `ESP_ERR_INVALID_STATE` — always check for this.

2. **SPI mode for SSD1681 is mode 0 (CPOL=0, CPHA=0)**, same as NRF24. No mode conflict on
   the shared bus. Speed: 4 MHz (BOARD_EINK_SPI_HZ) — conservative; SSD1681 supports up to
   20 MHz. Increase after validation.

3. **`spi_device_polling_transmit` is used for short commands; `spi_device_queue_trans` /
   `spi_device_get_trans_result` for the 5000-byte framebuffer write.** The polling form
   blocks and is simpler for command sequences. For the RAM write (5000 bytes), queued form
   allows DMA. In the stub, either form works — use polling for simplicity, note the TODO
   for DMA optimization.

4. **BUSY pin is INPUT with no pull.** GPIO1 reads H during refresh, L when ready. The
   polling loop `while (gpio_get_level(BOARD_EINK_BUSY_GPIO) == 1) vTaskDelay(10ms)` is
   performed OUTSIDE the SPI lock. This is the most critical design constraint — document
   it in `eink.c` comments.

5. **GPIO1 is strapping pin on ESP32-S3 (`SD_DATA0`, internal pull-down at boot).** After
   boot, `gpio_reset_pin(GPIO_NUM_1)` clears the bootloader function and returns it to a
   general-purpose input. Call `gpio_reset_pin` before `gpio_config` in `eink_init()`.

6. **The framebuffer is initialized to 0xFF (white in SSD1681 convention).** SSD1681 treats
   bit=1 as white, bit=0 as black. An all-white framebuffer means `memset(s_fb, 0xFF,
   EINK_FB_SIZE)` — do this in `eink_init()`.

7. **SPI transaction for framebuffer write:** The SSD1681 RAM write command (0x24) followed
   by 5000 bytes of data. The stub puts this in a `/* TODO STUB */` comment. The actual
   sequence is in Section 6.4 of the spec. Do not implement the command sequence — only
   the SPI infrastructure (device handle, GPIO, lock/unlock ordering).

8. **`eink_task` must NOT call `half_spi_lock` for the BUSY wait.** The sequence is:
   `half_spi_lock()` → SPI write → `half_spi_unlock()` → BUSY poll loop. The task body
   structure must enforce this order explicitly in code.

9. **`eink_start()` is called only if `eink_init()` returns true.** This mirrors the
   trackpad pattern. If the panel is not physically present, the task is not created and
   `s_fb` is never written to. No wasted heap.

10. **`half_state` is not yet defined in this plan.** `eink_task` reads it to render the
    display. In Plan Bricks-2, define a module-private stub:
    ```c
    /* TODO: replace with g_half_state from espnow_info.h (Plan Bricks-3) */
    static uint8_t s_layer_idx_stub = 0;
    ```
    The task reads `s_layer_idx_stub` instead of the real struct. Plan Bricks-3 wires the
    real struct. This avoids a forward dependency on Plan Bricks-3's headers.

---

## File Structure

### Created

| Path | Responsibility |
|---|---|
| `main/periph/eink/eink.h` | Public API: `eink_init()`, `eink_clear()`, `eink_push()`, `eink_start()` |
| `main/periph/eink/eink.c` | SPI device, GPIO, probe, framebuffer, refresh task skeleton |

### Modified

| Path | Change |
|---|---|
| `main/CMakeLists.txt` | Add `eink.c` under `KASE_HAS_EINK`; add `"periph/eink"` to INCLUDE_DIRS |
| `main/comm/rf/half_scan_task.c` | Add `eink_init()` / `eink_start()` calls in task body (after trackpad init) |

### Untouched

```
main/periph/half_spi.h/c        SPI lock — no changes needed
main/periph/trackpad/trackpad.c  unchanged
main/comm/rf/rf_rx_task.c       dongle — unchanged in this plan
boards/kase_half_left/board.h   BOARD_EINK_* defines already added in Plan Bricks-1
```

---

## Task 1: Write `eink.h`

**Files:**
- Create: `main/periph/eink/eink.h`

- [ ] **Step 1: Create directory**

```bash
mkdir -p /home/mae/Documents/GitHub/KaSe_firmware/main/periph/eink
```

- [ ] **Step 2: Write `main/periph/eink/eink.h`**

```c
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

/* Start the e-ink refresh task (prio 3, stack 4096, core 0).
 * Only call if eink_init() returned true.
 * The task refreshes the panel at ~1 Hz (or on content change in future). */
void eink_start(void);
```

- [ ] **Step 3: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink.h
git commit -m "feat(bricks): eink.h — SSD1681 skeleton API (lock ordering documented)"
```

---

## Task 2: Write `eink.c` (skeleton)

**Files:**
- Create: `main/periph/eink/eink.c`

- [ ] **Step 1: Write `main/periph/eink/eink.c`**

```c
/*
 * eink.c — SSD1681 e-ink skeleton for KaSe half firmware.
 * See eink.h for API contract and skeleton/stub boundary.
 *
 * Skeleton:  SPI device, GPIO, RST, probe, SPI lock ordering.
 * Stub:      SSD1681 command sequences, 1bpp rendering.
 *
 * CRITICAL SPI lock contract (do not change):
 *   eink_push():
 *     half_spi_lock()
 *       [SPI transactions — short, ~10 ms]
 *     half_spi_unlock()
 *     [BUSY wait — long, ~1-2 s — bus FREE, NRF can TX key events]
 *
 * This ensures keyboard latency is not affected by e-ink refresh cycles.
 */

#include "eink.h"
#include "board.h"         /* BOARD_EINK_* GPIO + SPI defines */
#include "half_spi.h"      /* half_spi_lock / half_spi_unlock */

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "eink";

/* ── SPI device handle for SSD1681 ─────────────────────────── */
static spi_device_handle_t s_eink_dev = NULL;

/* ── Framebuffer: 200×200 px, 1bpp (5000 bytes), static BSS ── */
/* White = 0xFF (SSD1681 convention: bit=1 → white, bit=0 → black) */
static uint8_t s_fb[EINK_FB_SIZE];

/* ── Probe result ───────────────────────────────────────────── */
static bool s_present = false;

/* ── Stub: current layer index for display (replaced by g_half_state in Plan Bricks-3) ── */
/* TODO: remove this stub and read from g_half_state.layer_idx once Plan Bricks-3 is done */
static uint8_t s_layer_idx_stub = 0;

/* ── Helper: send one SPI command byte (DC=low) ─────────────── */
/* Caller must hold half_spi_lock(). */
static void eink_send_cmd(uint8_t cmd)
{
    gpio_set_level(BOARD_EINK_DC_GPIO, 0);   /* DC=low → command */
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_eink_dev, &t);
}

/* ── Helper: send data bytes (DC=high) ──────────────────────── */
/* Caller must hold half_spi_lock(). */
static void eink_send_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(BOARD_EINK_DC_GPIO, 1);   /* DC=high → data */
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_eink_dev, &t);
}

bool eink_init(void)
{
    /* ── GPIO config: DC, RST (output); BUSY (input) ─────────── */
    /* GPIO1 is a strapping pin (SD_DATA0) — reset bootloader function first */
    gpio_reset_pin(BOARD_EINK_BUSY_GPIO);

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << BOARD_EINK_DC_GPIO) | (1ULL << BOARD_EINK_RST_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);

    gpio_config_t busy_cfg = {
        .pin_bit_mask = (1ULL << BOARD_EINK_BUSY_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&busy_cfg);

    /* ── Reset pulse: RST high 10 ms → low 10 ms → high 10 ms ─ */
    gpio_set_level(BOARD_EINK_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_EINK_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_EINK_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* ── Probe: BUSY should go low within 200 ms after reset ─── */
    /* SSD1681 pulls BUSY high internally during power-on / reset init. */
    int probe_ms = 0;
    while (gpio_get_level(BOARD_EINK_BUSY_GPIO) == 1 && probe_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        probe_ms += 10;
    }
    if (gpio_get_level(BOARD_EINK_BUSY_GPIO) != 0) {
        ESP_LOGI(TAG, "BUSY did not go low within 200 ms — panel not present on this half");
        return false;
    }

    /* ── Register SSD1681 as second device on SPI2_HOST ────────── */
    /* SPI2_HOST bus was initialized by rf_driver_init_tx(). */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = BOARD_EINK_SPI_HZ,   /* 4 MHz */
        .mode           = 0,                    /* CPOL=0, CPHA=0 — same as NRF24 */
        .spics_io_num   = BOARD_EINK_CS_GPIO,   /* GPIO18, active-low */
        .queue_size     = 1,
        .pre_cb         = NULL,
        .post_cb        = NULL,
    };
    esp_err_t err = spi_bus_add_device(BOARD_NRF_SPI_HOST, &dev_cfg, &s_eink_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %d — cannot register SSD1681", err);
        return false;
    }

    /* ── Initialize framebuffer to white ───────────────────────── */
    memset(s_fb, 0xFF, EINK_FB_SIZE);

    s_present = true;
    ESP_LOGI(TAG, "SSD1681 e-ink panel detected — init OK");
    return true;
}

void eink_push(const uint8_t *fb)
{
    if (!s_present || s_eink_dev == NULL) return;

    /* STEP 1: Acquire SPI bus (NRF24 must not be transmitting) */
    half_spi_lock();

    /* STEP 2: SSD1681 command sequence — write RAM + trigger full refresh.
     *
     * TODO STUB: Implement the following SSD1681 sequence:
     *   1. SW reset (cmd 0x12), then wait BUSY (inside the lock, short wait ~10 ms).
     *   2. Driver output control (cmd 0x01): set gate/source driver output counts
     *        for 200×200: data = {0xC7, 0x00, 0x00}  (200-1=0xC7 gate lines)
     *   3. Border waveform control (cmd 0x3C): data = {0x05}
     *   4. Set RAM-X address start/end (cmd 0x44): data = {0x00, 0x18}
     *        (0x00=col 0, 0x18=col 24 → 25 bytes × 8 bits = 200 px)
     *   5. Set RAM-Y address start/end (cmd 0x45): data = {0xC7, 0x00, 0x00, 0x00}
     *        (0x00C7 = 199 start, 0x0000 end — Y counts down)
     *   6. Set RAM-X address counter (cmd 0x4E): data = {0x00}
     *   7. Set RAM-Y address counter (cmd 0x4F): data = {0xC7, 0x00}
     *   8. Write B&W RAM (cmd 0x24): send fb[EINK_FB_SIZE] bytes
     *   9. Display update control 2 (cmd 0x22): data = {0xF7} (full update sequence)
     *   10. Master activation (cmd 0x20): no data — triggers the refresh
     *
     * After step 10, BUSY goes HIGH (panel is refreshing). DO NOT poll BUSY inside
     * the lock — release the lock first (step below), then poll.
     *
     * For now, send a no-op SW reset only (demonstrates SPI path works): */
    eink_send_cmd(0x12);   /* SW Reset — BUSY goes high briefly */

    /* STEP 3: Release SPI bus — BEFORE BUSY wait.
     * Panel is now refreshing internally. NRF24 can transmit freely. */
    half_spi_unlock();

    /* STEP 4: Wait for BUSY to go low (panel refresh complete).
     * Lock is NOT held during this wait. */
    int wait_ms = 0;
    while (gpio_get_level(BOARD_EINK_BUSY_GPIO) == 1 && wait_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (wait_ms >= 3000) {
        ESP_LOGW(TAG, "eink_push: BUSY timeout after 3 s (panel hung?)");
    }
}

void eink_clear(void)
{
    memset(s_fb, 0xFF, EINK_FB_SIZE);   /* all white */
    eink_push(s_fb);
    /* eink_push already polls BUSY to completion — panel is white when this returns */
}

/* ── Refresh task ───────────────────────────────────────────── */
static void eink_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "eink_task started (1 Hz refresh)");

    /* Initial clear on first run */
    eink_clear();

    for (;;) {
        /* 1 Hz refresh rate — e-ink content changes slowly (layer name, modifiers).
         * Future optimization: wake on ESP-NOW layer-change notification instead. */
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* TODO STUB: Render half_state into s_fb (1bpp, MSB-first).
         *
         * Plan Bricks-3 will wire g_half_state (espnow_info.h). Until then,
         * s_layer_idx_stub is always 0 and rendering draws "Layer 0".
         *
         * Rendering decision (deferred — see spec Section 9.3):
         *   Option A: Direct 1bpp font (bitmap font, no dependencies, minimal RAM).
         *   Option B: LVGL with a custom 1bpp display driver (more flexible, ~150 KB flash).
         *
         * Suggested minimal direct approach:
         *   1. memset(s_fb, 0xFF, EINK_FB_SIZE);  // white background
         *   2. Draw layer index as ASCII digits using a 1bpp bitmap font.
         *   3. Draw modifier state icons (Shift, Ctrl, etc.) as 16×16 bitmaps.
         *   4. Draw BT/USB status indicators.
         *
         * For now: do nothing (panel stays white from eink_clear() above). */
        (void)s_layer_idx_stub;   /* suppress unused warning until rendering is implemented */

        /* Only push if content has changed (future: compare previous state) */
        /* eink_push(s_fb); */   /* commented out — no rendering yet, avoid refresh noise */
    }
}

void eink_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        eink_task, "eink_task",
        4096,   /* 4 KB stack: rendering may need temporary local buffers */
        NULL, 3, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed: %d", (int)ret);
    }
}
```

- [ ] **Step 2: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink.c
git commit -m "feat(bricks): eink.c skeleton — SPI device, probe, push (SSD1681 cmds TODO)"
```

---

## Task 3: Wire `eink_init` / `eink_start` into `half_scan_task.c`

**Files:**
- Modify: `main/comm/rf/half_scan_task.c`

- [ ] **Step 1: Add `eink.h` include and init/start calls**

In the `#ifndef TEST_HOST` include block, add:

```c
#if CONFIG_KASE_HAS_EINK
#include "eink.h"
#endif
```

In `half_scan_task` (task body), after the trackpad init block, add:

```c
#if CONFIG_KASE_HAS_EINK
    /* E-ink skeleton: add SSD1681 SPI device, GPIO config, probe.
     * Returns true if the panel is physically present on this half. */
    bool eink_present = eink_init();
    if (eink_present) {
        ESP_LOGI(TAG, "e-ink detected — starting refresh task");
        eink_start();
    } else {
        ESP_LOGI(TAG, "e-ink not detected on this half (BUSY timeout) — skipping");
    }
#endif /* CONFIG_KASE_HAS_EINK */
```

- [ ] **Step 2: Verify init ordering in `half_scan_task` task body**

The order must be:
1. `half_spi_lock_init()`  — mutex creation
2. GPIO matrix reset (`gpio_reset_pin` for COL/ROW pins)
3. `rf_driver_init_tx()`  — initializes SPI2 bus + NRF24 device
4. `trackpad_init()` / `trackpad_start()` (if KASE_HAS_TRACKPAD)
5. `eink_init()` / `eink_start()` (if KASE_HAS_EINK)  ← NEW
6. `keyboard_button_create` + register callback
7. Heartbeat timer start

Step 5 must come AFTER step 3 because `spi_bus_add_device` (inside `eink_init`) requires
the SPI2 bus to be initialized. Read the current task body to verify the ordering.

- [ ] **Step 3: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/half_scan_task.c
git commit -m "feat(bricks): wire eink_init/eink_start into half_scan_task (after NRF init)"
```

---

## Task 4: CMake — add eink sources + periph/eink to INCLUDE_DIRS

**Files:**
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Add `eink.c` under `KASE_HAS_EINK`**

In `main/CMakeLists.txt`, the `KASE_HAS_ESPNOW` block currently has a comment
`# Plan 5 will add: comm/espnow/*.c`. Add before that block:

```cmake
# E-ink SSD1681 skeleton — half role only
if(CONFIG_KASE_HAS_EINK)
    list(APPEND srcs "periph/eink/eink.c")
endif()
```

Note: `half_spi.c` was added in Plan Bricks-1 under `KASE_HAS_RF_TX` — no change needed.
`esp_driver_spi` is already a priv_require when `KASE_HAS_RF_TX` is set.

- [ ] **Step 2: Add `"periph/eink"` to INCLUDE_DIRS**

In `idf_component_register(SRCS ... INCLUDE_DIRS ...)`, add `"periph/eink"` to the list
(alongside `"periph"` and `"periph/trackpad"` added in Plan Bricks-1).

- [ ] **Step 3: Commit CMake change**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/CMakeLists.txt
git commit -m "chore(bricks): CMake — eink.c source + periph/eink include dir"
```

---

## Task 5: Build verification — all three targets

- [ ] **Step 1: Build half_left (clean)**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_half_left \
    -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -10
```

Expected: `Project build complete.`

Common failure modes:
- `spi_bus_add_device: invalid arg` or wrong SPI host name → check `BOARD_NRF_SPI_HOST`
  is `SPI2_HOST` in board.h (it is — confirmed from existing code).
- `gpio_reset_pin` on GPIO1 causes issues → GPIO1 is available post-boot; the reset call
  is standard practice.
- `eink_send_cmd` / `eink_send_data` internal static functions trigger a `[-Wunused]` warning
  if the stub body doesn't call them → add `(void)eink_send_data;` or enable the calls in the
  SW Reset stub (the `eink_send_cmd(0x12)` call already uses `eink_send_cmd`; `eink_send_data`
  is unused in the stub — suppress with `(void)eink_send_data;` at file scope or just call
  it with zero bytes in the stub).

- [ ] **Step 2: Build half_right (clean)**

```bash
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_half_right \
    -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 3: Build dongle (clean)**

```bash
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_dongle \
    -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -5
```

Expected: `Project build complete.` (dongle has KASE_HAS_EINK=n, KASE_HAS_TRACKPAD=n).

---

## Self-review checklist

- [ ] `half_spi_unlock()` is called BEFORE the BUSY poll in `eink_push()`. If the order
  is reversed, NRF24 cannot send key events for up to 2 seconds during refresh.
- [ ] `eink_push()` is never called with `s_eink_dev == NULL` (guarded by `s_present`).
- [ ] `eink_init()` calls `gpio_reset_pin(BOARD_EINK_BUSY_GPIO)` before `gpio_config` on
  GPIO1 (strapping pin cleanup).
- [ ] `spi_bus_add_device` uses `BOARD_NRF_SPI_HOST` (not a hardcoded `SPI2_HOST` literal)
  to avoid divergence if the board ever changes.
- [ ] Framebuffer `s_fb[5000]` is in BSS (static array, zero-initialized then set to 0xFF in
  `eink_init`). No heap allocation.
- [ ] `eink_task` does not call `eink_push` in the stub (commented out) — avoids constant
  SW-reset noise on the bus with no rendering.
- [ ] Init ordering in `half_scan_task`: `eink_init` after `rf_driver_init_tx` (bus must
  exist before `spi_bus_add_device`).
- [ ] `KASE_HAS_EINK=n` on dongle — dongle CMake block for eink is empty; no eink symbols
  referenced from dongle code.
- [ ] `eink_send_data` is either called or suppressed with `(void)` to avoid -Wunused.
- [ ] All three boards build green.

---

## Out of scope / stubs left for the implementer

| Stub | Location | What remains |
|------|----------|--------------|
| SSD1681 init sequence | `eink.c::eink_push` TODO block | Steps 1-10 from spec Section 6.4 (driver output, border, RAM addr, write, activation) |
| SSD1681 BUSY wait inside lock | `eink.c::eink_push` | SW Reset (0x12) triggers brief BUSY; wait for it inside the lock (short) before writing RAM |
| 1bpp rendering — font + layout | `eink.c::eink_task` TODO block | Choose: direct 1bpp bitmap font, or LVGL with e-ink driver; implement after Plan Bricks-3 wires g_half_state |
| `g_half_state` read | `eink.c::s_layer_idx_stub` | Replace stub read with `half_state_lock(); copy = g_half_state; half_state_unlock();` after Plan Bricks-3 |
| Partial refresh | not planned | SSD1681 supports partial; out of scope for this brick |
| eink_task notify on layer change | `eink.c::eink_task` | Replace 1 Hz poll with `xTaskNotify` from `espnow_on_layer()` (Plan Bricks-3) |
| DMA for 5000-byte write | `eink_push` | Use `spi_device_queue_trans` + `spi_device_get_trans_result` for efficiency once command stubs are complete |
