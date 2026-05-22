# SSD1681 E-Ink Driver + LVGL Integration — KaSe Half

**Date:** 2026-05-22
**Branch:** `dongle-firmware`
**Status:** Design spec — approved, ready to implement (Plan Bricks-2 fill)
**Target boards:** `kase_half_left`, `kase_half_right` (reversible PCB, runtime probe)
**ESP-IDF:** 5.5
**Panel:** WeAct 1.54" SSD1681, 200×200 px, 1bpp, full refresh only

---

## 1. Overview and Goal

The skeleton in `main/periph/eink/eink.c` was hardware-validated: `eink_init()`
correctly probes the SSD1681 panel on the half-right (BUSY goes low within 200 ms).
The SPI device is registered, GPIOs are configured, and the lock ordering
(`half_spi_lock` → SPI transactions → `half_spi_unlock` → BUSY wait) is in place.

**This spec fills the two remaining stubs:**

1. The SSD1681 command sequence inside `eink_push()` — currently a no-op SW reset.
2. The rendering path — currently an empty `eink_task` that never calls `eink_push`.

**Goal for this brick:** Display a static screen — "KaSe" label + firmware version
string from `esp_app_get_description()` — rendered through LVGL at 1bpp, pushed
to the SSD1681 panel, producing visible output on the e-ink display. This validates
the full path: LVGL renders → flush_cb packs → `eink_push` commands SSD1681.

**Out of scope (Plan Bricks-3 and later):**

- Dynamic content from ESP-NOW (`g_half_state`: layer name, modifiers, battery)
- Partial refresh (ghosting management, LUT waveform tuning)
- Deep-sleep / power gating of the e-ink panel
- Multi-screen UI or animations

---

## 2. Architecture — Three Layers

```
┌─────────────────────────────────────────────────────────┐
│  LVGL v8 (LV_COLOR_DEPTH=1, monochrome)                 │
│    lv_label "KaSe"     lv_label firmware version        │
│    flush_cb: packs LVGL 1bpp area → s_fb (MSB-first)   │
│    Tick: esp_timer 5 ms → lv_tick_inc(5)                │
│    Handler: eink_lvgl_task loop (lv_timer_handler)      │
└──────────────────┬──────────────────────────────────────┘
                   │ eink_push(s_fb) — on flush "last fragment"
┌──────────────────▼──────────────────────────────────────┐
│  SSD1681 driver (eink.c)                                 │
│    half_spi_lock()                                       │
│    10-step command sequence (cmds below)                 │
│    half_spi_unlock()                                     │
│    BUSY poll outside lock (~1–2 s)                       │
└──────────────────┬──────────────────────────────────────┘
                   │ SPI2_HOST, 4 MHz, CS=GPIO18
┌──────────────────▼──────────────────────────────────────┐
│  SSD1681 panel — 200×200 px B&W RAM                     │
│    Default OTP LUT (full refresh waveform)               │
│    BUSY pin: H = refreshing, L = ready                   │
└─────────────────────────────────────────────────────────┘
```

### Layer 1 — LVGL

Raw LVGL v8 (no `esp_lvgl_port`). Rationale: `esp_lvgl_port` is built around
`esp_lcd` panel handles; the SSD1681 has no `esp_lcd` driver in this project.
Using raw LVGL avoids the abstraction mismatch and keeps the integration minimal.

### Layer 2 — SSD1681 driver (`eink.c`)

Fills the `eink_push()` stub with the full 10-step command sequence. No other
changes to `eink_init()`, `eink_clear()`, `eink_start()` (signatures preserved).

### Layer 3 — SSD1681 panel

200×200 px, 1bpp RAM (B&W RAM only — no RED RAM). Default OTP LUT. Full refresh
every push (~2 s). No partial refresh in v1.

---

## 3. Hardware Reference

All GPIO assignments come from `boards/kase_half_left/board.h` (half_right
board.h mirrors these; the same PCB is reversible):

| Signal        | GPIO      | Direction | Note                                     |
|---------------|-----------|-----------|------------------------------------------|
| CS            | GPIO18    | Output    | Active-low, SPI chip select              |
| DC            | GPIO12    | Output    | H = data, L = command                    |
| RST           | GPIO17    | Output    | Active-low, pulse at boot                |
| BUSY          | GPIO1     | Input     | H = panel busy, L = ready                |
| SPI MOSI      | GPIO48    | Output    | Shared SPI2 bus with NRF24               |
| SPI MISO      | GPIO47    | Input     | Shared SPI2 bus (e-ink is write-only)    |
| SPI SCK       | GPIO45    | Output    | Shared SPI2 bus                          |

SPI clock: `BOARD_EINK_SPI_HZ = 4 000 000` (4 MHz — conservative; SSD1681 max 20 MHz).
SPI host: `BOARD_NRF_SPI_HOST = SPI2_HOST` (bus already initialized by NRF24 init).

---

## 4. SSD1681 Command Sequence — `eink_push()`

The existing stub in `eink.c` already documents all 10 steps in a TODO comment.
This section confirms and specifies them precisely.

**Lock ordering (CRITICAL — must not change):**

```
half_spi_lock()
  [steps 1–10: SPI commands + RAM write, ~10 ms total]
half_spi_unlock()
[BUSY poll: up to 3 s, lock NOT held — NRF24 can TX key events freely]
```

**Steps:**

### Step 1 — Software Reset (cmd 0x12)

```c
eink_send_cmd(0x12);
/* BUSY goes HIGH briefly (~10 ms) during internal reset.
 * Poll inside the lock with a short timeout (50 ms max). */
int wait = 0;
while (gpio_get_level(BOARD_EINK_BUSY_GPIO) == 1 && wait < 50) {
    vTaskDelay(pdMS_TO_TICKS(1));
    wait++;
}
```

SW reset restores all registers to OTP defaults. Required before each full-update
sequence to ensure register state is clean.

### Step 2 — Driver Output Control (cmd 0x01)

```c
eink_send_cmd(0x01);
static const uint8_t d01[] = {0xC7, 0x00, 0x00};
eink_send_data(d01, sizeof(d01));
```

- Byte 0–1: `0x00C7` = 199 — number of gate lines minus 1 (200 rows).
- Byte 2: scanning direction = 0 (default).

### Step 3 — Border Waveform Control (cmd 0x3C)

```c
eink_send_cmd(0x3C);
static const uint8_t d3c[] = {0x05};
eink_send_data(d3c, sizeof(d3c));
```

`0x05` = fix border at VSS (black border). Prevents border flicker during full refresh.

### Step 4 — Set RAM-X Address Start/End (cmd 0x44)

```c
eink_send_cmd(0x44);
static const uint8_t d44[] = {0x00, 0x18};
eink_send_data(d44, sizeof(d44));
```

- Start col: 0x00 (column 0)
- End col: 0x18 = 24 (column 24 → 25 byte columns × 8 bits = 200 pixels)

### Step 5 — Set RAM-Y Address Start/End (cmd 0x45)

```c
eink_send_cmd(0x45);
static const uint8_t d45[] = {0xC7, 0x00, 0x00, 0x00};
eink_send_data(d45, sizeof(d45));
```

- Start Y: `0x00C7` = 199 (Y counts downward from 199 to 0)
- End Y: `0x0000` = 0

### Step 6 — Set RAM-X Address Counter (cmd 0x4E)

```c
eink_send_cmd(0x4E);
static const uint8_t d4e[] = {0x00};
eink_send_data(d4e, sizeof(d4e));
```

Reset X pointer to column 0 before RAM write.

### Step 7 — Set RAM-Y Address Counter (cmd 0x4F)

```c
eink_send_cmd(0x4F);
static const uint8_t d4f[] = {0xC7, 0x00};
eink_send_data(d4f, sizeof(d4f));
```

Reset Y pointer to row 199 (top of display). RAM is written top-to-bottom.

### Step 8 — Write B&W RAM (cmd 0x24)

```c
eink_send_cmd(0x24);
eink_send_data(fb, EINK_FB_SIZE);  /* 5000 bytes */
```

Sends the entire 1bpp framebuffer. Each byte covers 8 horizontal pixels.
Bit convention (SSD1681 native): **bit = 1 → white, bit = 0 → black**.
`s_fb` is initialized to `0xFF` (all white) by `eink_init()`.

The `fb` parameter was previously suppressed with `(void)fb` — this step
removes that suppression.

### Step 9 — Display Update Control 2 (cmd 0x22)

```c
eink_send_cmd(0x22);
static const uint8_t d22[] = {0xF7};
eink_send_data(d22, sizeof(d22));
```

`0xF7` selects the full update sequence using the default OTP LUT. No custom
waveform table is loaded.

### Step 10 — Master Activation (cmd 0x20)

```c
eink_send_cmd(0x20);
/* No data. This triggers the refresh. BUSY goes HIGH immediately after.
 * Do NOT poll BUSY here — release the SPI lock first (step below). */
```

After `0x20`, BUSY goes HIGH and the panel begins its ~2 s refresh cycle.
The SPI bus is no longer needed.

**Post-lock BUSY poll (outside lock):**

```c
half_spi_unlock();

int wait_ms = 0;
while (gpio_get_level(BOARD_EINK_BUSY_GPIO) == 1 && wait_ms < 3000) {
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_ms += 10;
}
if (wait_ms >= 3000) {
    ESP_LOGW(TAG, "eink_push: BUSY timeout after 3 s (panel hung?)");
}
```

This pattern is already in the skeleton and must be preserved verbatim.

---

## 5. LVGL Integration — Raw LVGL v8

### 5.1 Why Raw LVGL, Not `esp_lvgl_port`

`esp_lvgl_port` wraps around `esp_lcd_panel_handle_t` and drives flush via
`esp_lcd_panel_draw_bitmap()`. SSD1681 has no `esp_lcd` driver in this project —
adding one would require implementing the full panel IO abstraction, which is
more complex than a direct flush callback. Raw LVGL is simpler here and keeps
the e-ink path self-contained in `eink.c` + a new `eink_lvgl.c`.

If raw LVGL tick/lock proves fiddly during integration (e.g., thread-safety
edge cases with the handler task), the fallback is to create a thin
`esp_lcd`-compatible wrapper and use `esp_lvgl_port` — but try raw first.

### 5.2 Color Depth

The half build uses `LV_COLOR_DEPTH=1`. LVGL v8 monochrome mode renders
pixels as 1-bit values. The draw buffer contains packed 1bpp data after
rendering.

**Important:** `LV_COLOR_DEPTH=1` is a **build-time constant** baked into
the LVGL library at compilation. It cannot coexist with the keyboards'
`LV_COLOR_DEPTH=16` in the same build. The half build is completely separate
(`sdkconfig.defaults.half_left` / `.half_right`), so there is no conflict.
The keyboards' LVGL config is untouched.

### 5.3 Draw Buffer

```c
/* Full-panel draw buffer: 200×200 / 8 = 5000 bytes, static BSS */
static lv_color_t s_lvgl_buf[EINK_WIDTH * EINK_HEIGHT];
/* At LV_COLOR_DEPTH=1, lv_color_t is 1 bit; the array holds 1 color per pixel.
 * LVGL packs them internally. Total BSS cost: ~200 bytes (LVGL internal packing). */
```

Note on LVGL 1bpp buffer sizing: with `LV_COLOR_DEPTH=1`, `sizeof(lv_color_t)` = 1
byte (LVGL uses 8-bit units even for 1bpp). An array of `EINK_WIDTH * EINK_HEIGHT`
= 40 000 elements actually allocates 40 000 bytes — that is too large. Use a single
draw buffer sized to one horizontal stripe instead, e.g.:

```c
/* Single stripe buffer: 200 px wide × 10 rows = 250 bytes at 1bpp */
#define LVGL_BUF_ROWS  10
static lv_color_t s_lvgl_buf[EINK_WIDTH * LVGL_BUF_ROWS];
```

LVGL will call `flush_cb` multiple times (one call per stripe) with `is_last`
set to `true` on the final call. The flush callback accumulates stripes into
`s_fb`, and `eink_push` is called only on the last fragment. This keeps
stack/BSS usage bounded regardless of panel size.

**Alternative:** use a full-panel buffer (5000 bytes at 1bpp — one byte per 8
pixels) if RAM allows. The half has 512 KB SRAM; 5000 bytes is negligible.
Single-buffer mode calls `flush_cb` exactly once with `is_last = true`,
simplifying the flush callback. Prefer this for v1 simplicity:

```c
/* Full-panel, 1bpp packed: 5000 bytes */
static uint8_t s_lvgl_buf_raw[EINK_FB_SIZE];  /* 5000 bytes */
static lv_color_t *s_lvgl_buf = (lv_color_t *)s_lvgl_buf_raw;
```

Verify LVGL's 1bpp buffer layout matches the direct byte interpretation
before relying on this cast (see Section 7 — Testing).

### 5.4 Display Driver Registration

```c
lv_disp_drv_t drv;
lv_disp_drv_init(&drv);
drv.draw_buf  = &s_draw_buf;          /* lv_disp_draw_buf_t */
drv.flush_cb  = eink_lvgl_flush_cb;
drv.hor_res   = EINK_WIDTH;           /* 200 */
drv.ver_res   = EINK_HEIGHT;          /* 200 */
drv.full_refresh = 1;                 /* always flush full frame */
lv_disp_t *disp = lv_disp_drv_register(&drv);
```

`full_refresh = 1` ensures LVGL always sends the complete 200×200 framebuffer
in a single flush cycle, even for small invalidations. This matches the SSD1681
requirement (partial RAM writes are not used in v1).

### 5.5 Flush Callback

```c
static void eink_lvgl_flush_cb(lv_disp_drv_t *drv,
                                const lv_area_t *area,
                                lv_color_t *color_p)
{
    /* Pack LVGL's 1bpp area into s_fb (MSB-first, white=1).
     * With full_refresh=1, area always covers [0,0]–[199,199].
     * This is the error-prone step — see Section 7 for the host test. */
    eink_pack_lvgl_area(area, color_p, s_fb);

    if (lv_disp_flush_is_last(drv)) {
        /* All LVGL fragments received — push to panel */
        eink_push(s_fb);
    }

    lv_disp_flush_ready(drv);  /* MUST be called to unblock LVGL */
}
```

`eink_pack_lvgl_area()` is a new function, unit-tested separately (Section 7).
It converts from LVGL's internal 1bpp pixel representation to the SSD1681
RAM layout (MSB-first, bit=1 → white). Orientation and bit-order must be
verified at implementation time — they are the most error-prone aspect
of the 1bpp integration.

### 5.6 LVGL Tick Source

```c
static esp_timer_handle_t s_lvgl_tick_timer;

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(5);   /* 5 ms per tick */
}

/* In eink_lvgl_init(): */
esp_timer_create_args_t tick_args = {
    .callback = lvgl_tick_cb,
    .name     = "lvgl_tick",
};
esp_timer_create(&tick_args, &s_lvgl_tick_timer);
esp_timer_start_periodic(s_lvgl_tick_timer, 5 * 1000);   /* 5 ms period */
```

The tick timer runs independently of the display refresh rate. It only advances
LVGL's internal time; it does not trigger redraws.

### 5.7 LVGL Handler Task

```c
static void eink_lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "eink_lvgl_task started");
    for (;;) {
        uint32_t sleep_ms = lv_timer_handler();
        /* lv_timer_handler returns the time in ms until the next timer fires.
         * Cap at 50 ms to remain responsive to invalidations. */
        if (sleep_ms > 50) sleep_ms = 50;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}
```

**Priority:** 3 (same as current `eink_task`). This is intentionally lower than
`rf_rx_task` and `half_scan_task` so the LVGL handler never starves the NRF24
hot path. E-ink refresh is slow by nature; a few extra milliseconds of handler
latency are irrelevant.

**Core:** 0 (same as current `eink_task`).

**Stack:** 4096 bytes minimum. LVGL rendering + label layout may use more;
increase to 6144 if stack overflow is observed (use `uxTaskGetStackHighWaterMark`
during validation).

### 5.8 Refresh Throttling

E-ink full refresh takes ~2 s. The existing `eink_task` runs `vTaskDelay(1000)`
at 1 Hz — this causes continuous redraws regardless of content change. This is
replaced by event-driven refresh:

- `lv_timer_handler` calls `flush_cb` only when LVGL has pending invalidations.
- For a static screen (v1), LVGL invalidates exactly once at startup (when the
  label is created). After the first flush, no further invalidations occur unless
  the screen content is explicitly changed.
- Result: one panel refresh at startup, then silence. No polling loop needed.

**Guard against re-entry:** `eink_push` already returns early if `!s_present`.
Additionally, `flush_cb` should not call `eink_push` re-entrantly. Since
`eink_lvgl_task` runs serially and `lv_timer_handler` is not reentrant, this is
safe by default.

The `vTaskDelay(pdMS_TO_TICKS(1000))` loop in the current `eink_task` is removed.
The task is replaced by `eink_lvgl_task`.

---

## 6. Static Screen — v1 Content

The v1 display shows two labels, rendered once at startup:

```c
/* Inside eink_lvgl_init(), after lv_disp_drv_register(): */
lv_obj_t *scr = lv_scr_act();

lv_obj_t *label_name = lv_label_create(scr);
lv_label_set_text(label_name, "KaSe");
lv_obj_set_pos(label_name, 60, 80);   /* centered-ish on 200×200 */

const esp_app_desc_t *desc = esp_app_get_description();
lv_obj_t *label_ver = lv_label_create(scr);
lv_label_set_text(label_ver, desc->version);
lv_obj_set_pos(label_ver, 60, 100);
```

`esp_app_get_description()` is available via `esp_app_desc.h` (ESP-IDF). The
`version` field is populated by ESP-IDF from `git describe --tags` at build
time when `CONFIG_APP_PROJECT_VER_FROM_CONFIG=n` (the default).

Font: `LV_FONT_MONTSERRAT_14` (enabled in sdkconfig — see Section 8). Default
LVGL font is adequate for the static proof-of-concept; sizing/positioning can
be refined after the pipeline is validated.

Background: LVGL default is black on black for 1bpp. Set the screen background
to white explicitly:

```c
lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
lv_obj_set_style_text_color(label_name, lv_color_black(), LV_PART_MAIN);
lv_obj_set_style_text_color(label_ver,  lv_color_black(), LV_PART_MAIN);
```

At `LV_COLOR_DEPTH=1`, `lv_color_white()` = all bits set = 1, and `lv_color_black()`
= 0. Verify that LVGL's white maps to SSD1681 white (bit=1) in the flush callback.

---

## 7. Testing

### 7.1 Host Test — 1bpp Packing Function

The most error-prone part of the integration is `eink_pack_lvgl_area()`:
the mapping from LVGL's internal 1bpp pixel layout to the SSD1681 RAM bit
order (MSB-first, white=1).

A host-side C test (in `test/`) must verify this function with known patterns:

- Input: LVGL buffer with a single black pixel at (0, 0) — all other pixels white.
  Expected: `s_fb[0]` = 0b01111111 = 0x7F (MSB=pixel[0,0]=black=0; others=white=1).
- Input: LVGL buffer all-white. Expected: `s_fb` = all 0xFF.
- Input: LVGL buffer all-black. Expected: `s_fb` = all 0x00.
- Input: checkerboard pattern. Expected: alternating 0xAA / 0x55 bytes.
- Input: a single row of pixels (horizontal stripe). Verifies row stride is correct.

The test also verifies total output size = `EINK_FB_SIZE` = 5000 bytes,
regardless of the `lv_area_t` region passed in (with `full_refresh=1` it is
always [0,0]–[199,199]).

This test should be written before the flush callback, not after.

### 7.2 Integration Bench

Flash half-right (the half with the e-ink panel):

```bash
idf.py -B build_half_right -DBOARD=kase_half_right build
idf.py -B build_half_right -p /dev/ttyUSB0 flash monitor
```

Expected log sequence on boot:

```
I eink: SSD1681 e-ink panel detected — init OK
I eink_lvgl: LVGL init OK, display registered (200×200, 1bpp)
I eink_lvgl: eink_lvgl_task started
I eink: eink_push: SPI write complete (~10 ms)
I eink: eink_push: BUSY cleared after <N> ms
```

Visual check: panel must show "KaSe" and the version string (e.g., `v0.4.2-3-gABCDEF`).

### 7.3 SPI Coexistence Validation

While the panel is refreshing (~2 s BUSY high):

- Press keys on the half matrix.
- Verify key events reach the dongle (log on dongle or OS keystrokes visible).
- BUSY wait is outside the SPI lock — NRF24 TX must not be blocked.

This is a regression test for the `half_spi_lock/unlock` contract.

---

## 8. Build and sdkconfig Changes

### 8.1 `sdkconfig.defaults.half_left` and `sdkconfig.defaults.half_right`

Add the following block to **both** files (they share the same LVGL requirements):

```ini
# --- LVGL: 1bpp monochrome for SSD1681 e-ink ---
# NOTE: This overrides the keyboards' LV_COLOR_DEPTH_16. The half build
# is independent — no conflict. Each board has its own build directory and sdkconfig.
CONFIG_LV_COLOR_DEPTH_1=y
# Disable 16-bit (conflicts with 1bpp):
# CONFIG_LV_COLOR_DEPTH_16 is not set
CONFIG_LV_USE_LABEL=y
CONFIG_LV_FONT_MONTSERRAT_14=y

# LVGL memory: use internal allocator (default)
# At 1bpp, LVGL internal heap usage is minimal (~8 KB for v8 core + label objects)
```

Note: `CONFIG_LV_COLOR_DEPTH_16=y` is currently present in the base
`sdkconfig.defaults`. The half-specific files are merged on top; the half file
must set `CONFIG_LV_COLOR_DEPTH_1=y` and explicitly unset 16. In Kconfig,
`CONFIG_LV_COLOR_DEPTH_1=y` implies `CONFIG_LV_COLOR_DEPTH_16` is not set
(they are a choice group). Verify this with `idf.py menuconfig` on the half build.

### 8.2 Flash and RAM Budget

| Item                      | Size     | Notes                                     |
|---------------------------|----------|-------------------------------------------|
| LVGL v8 core (1bpp)       | ~100 KB  | Flash; less than 16bpp build (~150 KB)    |
| LVGL font (Montserrat 14) | ~10 KB   | Flash                                     |
| `s_fb` framebuffer        | 5 000 B  | Static BSS (already in skeleton)          |
| LVGL stripe draw buffer   | ~250 B   | Static BSS (10-row stripe, 200×10/8)      |
| LVGL heap                 | ~8 KB    | Dynamic, LVGL internal; label + screen    |
| `eink_lvgl_task` stack    | 4–6 KB   | FreeRTOS stack                            |
| `esp_timer` tick          | ~200 B   | Negligible                                |

Total flash impact from LVGL on the half build: ~110 KB. The half's factory
partition is 2 MB — this is well within budget.

The half build does not include USB HID, BLE, OLED/round display, LED strip,
or LittleFS. The LVGL addition is affordable.

### 8.3 `idf_component.yml` — No Change Required

`lvgl/lvgl: ^8` is already a dependency. `esp_lvgl_port` is also listed but
not used in the half build; it is compiled conditionally and its component is
only linked if referenced. Raw LVGL requires no new component entries.

---

## 9. Files to Create / Modify

### New files

| File                                  | Description                                      |
|---------------------------------------|--------------------------------------------------|
| `main/periph/eink/eink_lvgl.c`        | Raw LVGL init, flush callback, tick, handler task|
| `main/periph/eink/eink_lvgl.h`        | Public API: `eink_lvgl_init()`, `eink_lvgl_start()` |
| `test/test_eink_pack.c`               | Host test for `eink_pack_lvgl_area()` bit packing|

### Modified files

| File                                  | Change                                            |
|---------------------------------------|---------------------------------------------------|
| `main/periph/eink/eink.c`             | Fill `eink_push()` stub (10-step command sequence); remove `eink_task` polling loop |
| `main/periph/eink/eink.h`             | Expose `eink_pack_lvgl_area()` for test linkage   |
| `main/periph/eink/CMakeLists.txt`     | Add `eink_lvgl.c` to sources                      |
| `sdkconfig.defaults.half_left`        | Add LVGL 1bpp config block                        |
| `sdkconfig.defaults.half_right`       | Add LVGL 1bpp config block                        |

The main application entry point (`main/main.c` or the half's equivalent task
orchestrator) must call `eink_lvgl_init()` followed by `eink_lvgl_start()`
after `eink_init()` returns `true`.

---

## 10. Open Points

### 10.1 LVGL 1bpp Buffer Format vs SSD1681 Bit Order

LVGL v8 with `LV_COLOR_DEPTH=1` stores pixels packed MSB-first within each byte,
with the leftmost pixel in the MSB. The SSD1681 B&W RAM also uses MSB-first
(leftmost pixel = MSB of each byte, bit=1 → white). These should align, but the
exact row stride, Y-axis direction, and whether LVGL treats 0 or 1 as "white" must
be confirmed at implementation time.

The host test (Section 7.1) is the primary verification mechanism. Do not assume
alignment without running the test.

### 10.2 LVGL Handler Task and Thread Safety

Raw LVGL (without `esp_lvgl_port`) has no built-in mutex around
`lv_timer_handler`. If any other task calls LVGL APIs (e.g., to update label
text in Plan Bricks-3), a mutex must be taken before all LVGL calls:

```c
/* Pattern for future dynamic content updates: */
xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
lv_label_set_text(label_name, new_text);
lv_obj_invalidate(label_name);
xSemaphoreGive(s_lvgl_mutex);
```

For v1 (static screen, single task touching LVGL), no mutex is needed.
Add the mutex when Plan Bricks-3 (ESP-NOW → label update) is implemented.

### 10.3 `esp_lvgl_port` Fallback

If raw LVGL tick/lock proves fiddly (e.g., `lv_timer_handler` interacts badly
with `esp_timer` callbacks on ESP-IDF 5.5), the fallback is:

1. Add a thin `esp_lcd` panel driver stub for SSD1681 that wraps `eink_push`.
2. Use `esp_lvgl_port_add_disp()` with that panel handle.
3. `esp_lvgl_port` then manages its own tick and handler task internally.

This adds ~50 lines of boilerplate but eliminates tick/threading concerns.
Try raw LVGL first.

### 10.4 `eink_task` Replacement Strategy

The current `eink_start()` creates `eink_task`. After this brick, `eink_task`
is retired and replaced by `eink_lvgl_task` (created by `eink_lvgl_start()`).
The `eink_start()` function can be repurposed to call `eink_lvgl_start()`, or
kept as-is and a new `eink_lvgl_start()` added alongside it. The caller site
in the half main task must be updated.

---

## 11. Self-Review Checklist

- [x] SSD1681 command bytes match the datasheet (SW reset 0x12; driver output
      0x01 = 200 gate lines; RAM window 0x44/0x45 = 25 cols × 200 rows; write
      B&W RAM 0x24; display update 0x22 = 0xF7; activation 0x20).
- [x] Lock ordering preserved: `half_spi_lock` before all SPI ops;
      `half_spi_unlock` before BUSY poll.
- [x] No `eink_push` call inside the SPI lock.
- [x] `lv_disp_flush_ready()` called unconditionally in `flush_cb`
      (not only on last fragment — LVGL will deadlock otherwise).
- [x] `full_refresh = 1` set on the display driver to avoid partial-area flush.
- [x] LVGL handler task priority = 3 (does not starve NRF24 / scan tasks).
- [x] Static screen content (labels) created on the active screen before
      `lv_timer_handler` is called the first time — LVGL renders on first tick.
- [x] `sdkconfig.defaults.half_left` and `.half_right` both updated.
- [x] Host test for `eink_pack_lvgl_area()` listed as required before flush_cb
      implementation, not as an afterthought.
- [x] Dynamic content (`g_half_state`, ESP-NOW layer updates) explicitly
      deferred to Plan Bricks-3 — not referenced here.
- [x] Flash budget verified: ~110 KB LVGL addition fits in 2 MB factory partition.
- [x] `esp_app_get_description()` used for version string — no hardcoded version.
- [x] GPIO1 (BUSY) strapping-pin reset already handled in `eink_init()`
      via `gpio_reset_pin(BOARD_EINK_BUSY_GPIO)` — no change needed.
