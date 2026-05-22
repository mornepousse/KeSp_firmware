# Plan Bricks-1 — SPI Bus Lock + Trackpad Skeleton + Dongle PKT_TRACKPAD Handler

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the existing `s_tx_mutex` in `half_scan_task.c` into a shared SPI2 bus lock
(`half_spi.h/c`), add a trackpad skeleton (`periph/trackpad/`) with real I2C init + IRQ +
probe and stubbed register reads, wire the trackpad init/start into `half_scan_task.c`, and
implement the `PKT_TYPE_TRACKPAD` decode + stub mouse-HID hook on the dongle
(`rf_rx_task.c`). End state: half_left + half_right + dongle all build green.

**Architecture:**
- `half_spi.h/c` owns the single `SemaphoreHandle_t` that gates SPI2 bus access. It is
  initialized by `half_scan_task_start()` before `rf_driver_init_tx()`. NRF sends (already
  in `half_scan_task.c`) switch from `s_tx_mutex` → `half_spi_lock/unlock`. The e-ink brick
  (Plan Bricks-2) will reuse the same lock without changes.
- `trackpad.h/c` implements: I2C master init on SDA=GPIO40/SCL=GPIO38 at 400 kHz, RST
  pulse on GPIO13, RDY ISR on GPIO14 (NEGEDGE → semaphore), I2C ACK probe, and a FreeRTOS
  task that waits on the semaphore and calls `rf_encode_trackpad` + `half_spi_lock` +
  `rf_driver_send`. The IQS5xx register read is a clearly-marked stub.
- Kconfig: `KASE_HAS_TRACKPAD` (default y if HALF) + `KASE_HAS_EINK` (default y if HALF,
  needed for half_spi.c to be compiled) added; `KASE_HAS_ESPNOW` extended to HALF. All
  guarded such that the dongle and keyboard builds are unaffected.
- CMakeLists.txt: new conditional blocks for trackpad + half_spi sources; `esp_driver_i2c`
  added to priv_requires when trackpad enabled.
- `rf_rx_task.c` dongle: the `PKT_TYPE_TRACKPAD` TODO comment replaced by a real decode +
  `(void)tp` stub that does not crash.

**Tech Stack:** ESP-IDF 5.5, `esp_driver_i2c` (I2C master v2 API), `esp_driver_spi`,
`esp_driver_gpio`, FreeRTOS, existing `rf_packet.h` (rf_trackpad_t already defined).

**Spec reference:** `docs/superpowers/specs/2026-05-22-half-peripherals-espnow-design.md`
Sections 4 (half_spi), 5 (trackpad), Annex A/B.

**Depends on:** Plan Half-1 MVP (half_scan_task.c complete and build-green; NRF PTX
validated on bench). The dongle part is independent of the half build.

**Build targets:**
- `idf.py -B build_half_left  -DBOARD=kase_half_left  -DIDF_TARGET=esp32s3 build`
- `idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build`
- `idf.py -B build_dongle     -DBOARD=kase_dongle     -DIDF_TARGET=esp32s3 build`

Build command preamble (always `rm -f sdkconfig` when switching board):
```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_<target> -DBOARD=kase_<target> -DIDF_TARGET=esp32s3 build
```

**Hardware (from spec Annex C and `~/Documents/PCB-esp/CLAUDE.md`):**

| Signal     | GPIO     | Notes                                        |
|------------|----------|----------------------------------------------|
| SDA_TRACK  | GPIO40   | I2C data, 4.7kΩ pull-up to 3.3V             |
| SCL_TRACK  | GPIO38   | I2C clock, 4.7kΩ pull-up to 3.3V            |
| RST_TRACK  | GPIO13   | Reset active-low; 10 ms pulse at boot        |
| RDY_TRACK  | GPIO14   | Data-ready interrupt, active-low, NEGEDGE    |
| NRF MOSI   | GPIO48   | SPI2_HOST (shared bus)                       |
| NRF MISO   | GPIO47   |                                              |
| NRF SCK    | GPIO45   | Strapping pin VDD_SPI — safe: CPOL=0         |
| NRF CSN    | GPIO35   | NRF24L01+ chip select                        |
| NRF CE     | GPIO36   | NRF24L01+ transmit enable                    |

---

## Learned facts baked in

1. **`s_tx_mutex` init ordering is critical.** It is currently created inside `half_scan_task`
   (the FreeRTOS task body, not `half_scan_task_start`). After refactor, `half_spi_lock_init()`
   must be called before `rf_driver_init_tx()` AND before `trackpad_start()`. The safe location
   is at the top of `half_scan_task` (the task body), before any SPI or peripheral call. Do NOT
   call `half_spi_lock_init()` from `half_scan_task_start()` (called from `app_main`, before the
   task runs) — the mutex must be created in task context or before any task that uses it.
   Simplest: create it at the very top of the `half_scan_task` function body, before the NRF
   init call, exactly where `s_tx_mutex = xSemaphoreCreateMutex()` lives now. The move is
   purely mechanical — the call site moves from the task body to `half_spi.c` internals, but
   `half_spi_lock_init()` is still called from the same point in `half_scan_task`.

2. **`half_spi.c` must be compiled whenever EINK or TRACKPAD is present** — not only when EINK
   is enabled. The spec gates it on `KASE_HAS_EINK`, but trackpad also calls
   `half_spi_lock/unlock`. The safest approach: compile `half_spi.c` whenever
   `KASE_HAS_RF_TX` is set (i.e., for every HALF build), because the original `s_tx_mutex`
   was always compiled. `KASE_HAS_EINK` and `KASE_HAS_TRACKPAD` both imply HALF, so tying
   half_spi.c to `KASE_HAS_RF_TX` is equivalent and simpler than a disjunction.

3. **Two existing send sites in `half_scan_task.c` must switch to `half_spi_lock/unlock`.**
   They are: `tx_key_event()` and `heartbeat_timer_cb()`. Both currently call
   `xSemaphoreTake(s_tx_mutex, portMAX_DELAY)` / `xSemaphoreGive(s_tx_mutex)`. Replace with
   `half_spi_lock()` / `half_spi_unlock()`. Remove `static SemaphoreHandle_t s_tx_mutex`.

4. **I2C master v2 API (ESP-IDF 5.x):** Use `i2c_master_bus_add_device`, `i2c_master_bus_rm_device`,
   `i2c_master_transmit_receive` — NOT the deprecated `i2c_master_write_to_device`. The bus handle
   is a `i2c_master_bus_handle_t`. For a probe, `i2c_master_probe()` returns `ESP_OK` on ACK.

5. **IQS5xx I2C address:** The TPS43-201A-S (Azoteq IQS550/572) default address is `0x74`
   (7-bit). This is the address used for the I2C probe. Mark it with a `/* TODO: verify address
   from ADDR pin config on PCB */` comment since the exact part may differ.

6. **`esp_driver_i2c` priv_requires:** The half build currently lists `esp_driver_i2c` in
   priv_requires unconditionally (it is already in the list). Verify this before adding a
   duplicate; only add a conditional block if it is not already present.

7. **Dongle build is NOT affected by trackpad.** The dongle compiles `rf_rx_task.c` which
   already has the `PKT_TYPE_TRACKPAD` comment. Adding the decode stub does not introduce
   new dependencies (rf_decode_trackpad is inline in rf_packet.h). No new CMake changes
   needed for the dongle side of this task.

8. **No host tests for trackpad in this plan.** `trackpad.c` has I2C and FreeRTOS deps that
   cannot easily be mocked. The encode/decode path (`rf_encode_trackpad` / `rf_decode_trackpad`)
   is already tested by the rf_packet host tests from Plan 2. No new host tests here.

9. **Build-then-verify sequence:** After each task, run the affected builds before committing.
   Do not batch multiple tasks and then try to fix everything at once — the linker error
   pattern from `cdc_half_stubs.c` (Plan Half-1, Fact 5 in the execution notes) shows that
   each new source file can introduce undefined symbols that must be caught incrementally.

---

## File Structure

### Created

| Path | Responsibility |
|---|---|
| `main/periph/half_spi.h` | Public API: `half_spi_lock_init()`, `half_spi_lock()`, `half_spi_unlock()` |
| `main/periph/half_spi.c` | Implementation: static mutex, trivial wrappers |
| `main/periph/trackpad/trackpad.h` | Public API: `trackpad_init()`, `trackpad_start()` |
| `main/periph/trackpad/trackpad.c` | I2C init, RST pulse, RDY ISR, I2C probe, TX task + stub register read |

### Modified

| Path | Change |
|---|---|
| `main/Kconfig.projbuild` | Add `KASE_HAS_TRACKPAD`, `KASE_HAS_EINK`; extend `KASE_HAS_ESPNOW` to HALF |
| `main/CMakeLists.txt` | Add half_spi.c under RF_TX; add trackpad.c + esp_driver_i2c under TRACKPAD |
| `main/comm/rf/half_scan_task.c` | Remove `s_tx_mutex`; add `half_spi_lock_init()` call; switch two send sites to `half_spi_lock/unlock`; add `trackpad_init/start` calls |
| `boards/kase_half_left/board.h` | Add trackpad GPIO defines (SDA40, SCL38, RST13, RDY14) and e-ink GPIO defines |
| `main/comm/rf/rf_rx_task.c` | Replace `PKT_TYPE_TRACKPAD` TODO with decode + `(void)tp` stub |

### Untouched

```
main/comm/rf/rf_packet.h         rf_trackpad_t, rf_encode_trackpad, rf_decode_trackpad — already defined
main/comm/rf/rf_driver.c         PTX and PRX paths unchanged
main/comm/rf/dongle_engine_state.c  unchanged (layer_changed hook is Plan Bricks-3)
boards/kase_half_right/board.h   inherits from left; GPIO defines flow through
sdkconfig.defaults.half_left     unchanged (WiFi stays off until Bricks-3)
sdkconfig.defaults.half_right    unchanged
```

---

## Task 1: Extend Kconfig — TRACKPAD, EINK, ESPNOW-on-HALF

**Files:**
- Modify: `main/Kconfig.projbuild`

This task adds three new Kconfig symbols needed by all three brick plans. Do it first so
subsequent tasks can gate on them cleanly.

- [ ] **Step 1: Read the current Kconfig**

```bash
cat /home/mae/Documents/GitHub/KaSe_firmware/main/Kconfig.projbuild
```

Verify: `KASE_HAS_ESPNOW` currently has `default y if KASE_DEVICE_ROLE_DONGLE` only.
No `KASE_HAS_TRACKPAD` or `KASE_HAS_EINK` exist yet.

- [ ] **Step 2: Edit `main/Kconfig.projbuild`**

After the `KASE_HAS_ESPNOW` block (before `endmenu`), add:

```kconfig
config KASE_HAS_TRACKPAD
    bool
    default y if KASE_DEVICE_ROLE_HALF
    default n
    help
        Compiles trackpad.c (IQS5xx I2C driver + RF trackpad TX).
        Runtime probe: trackpad_init() returns false if not physically present.
        Compiled on both half variants — PCB is reversible, presence detected at boot.

config KASE_HAS_EINK
    bool
    default y if KASE_DEVICE_ROLE_HALF
    default n
    help
        Compiles eink.c (SSD1681 SPI skeleton + refresh task) and half_spi.c
        (shared SPI2 bus lock used by NRF, e-ink, and trackpad).
        Runtime probe: eink_init() returns false if panel not physically present.
        Compiled on both half variants.
```

Also modify the `KASE_HAS_ESPNOW` block to add the HALF default:

```kconfig
config KASE_HAS_ESPNOW
    bool
    default y if KASE_DEVICE_ROLE_DONGLE
    default y if KASE_DEVICE_ROLE_HALF
    default n
    help
        Compiles ESP-NOW stack (espnow_link.c + espnow_info.c).
        On the dongle: info-channel + Plan 5 OTA/config reserved range.
        On the half: info-channel only (layer/state RX, battery TX stub).
        Requires WiFi — see sdkconfig.defaults.half_left/right (Plan Bricks-3).
```

- [ ] **Step 3: Verify Kconfig parses (dongle build reconfigure, no errors)**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_dongle \
    -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware reconfigure 2>&1 | grep -E "ERROR|Kconfig|error:" | head -20
```

Expected: no Kconfig parse errors. The dongle build still has `KASE_HAS_TRACKPAD=n`,
`KASE_HAS_EINK=n`, `KASE_HAS_ESPNOW=y` (unchanged).

- [ ] **Step 4: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/Kconfig.projbuild
git commit -m "feat(bricks): Kconfig — TRACKPAD, EINK flags + ESPNOW extended to HALF"
```

---

## Task 2: Add peripheral GPIO defines to `board.h`

**Files:**
- Modify: `boards/kase_half_left/board.h`

The spec (Annex C) lists GPIO assignments for trackpad and e-ink. They are referenced by
`trackpad.c` and `eink.c` (Plan Bricks-2). Add them now so both bricks can use named
constants. `boards/kase_half_right/board.h` inherits via `#include` — no change needed there.

- [ ] **Step 1: Read the current `board.h`**

```bash
cat /home/mae/Documents/GitHub/KaSe_firmware/boards/kase_half_left/board.h
```

Confirm the "Unused peripheral GPIO" comment block exists. The defines below replace the
comment with real named constants.

- [ ] **Step 2: Edit `boards/kase_half_left/board.h`**

Replace the comment block:
```c
/* ── Unused peripheral GPIO — not initialized in MVP ───────── */
/* E-ink: CS=18, DC=12, RES=17, BUSY=1 */
/* Trackpad I2C: SDA=40, SCL=38, RST=13, RDY=14 */
/* Battery ADC: GPIO15 (ADC2_CH4), switchable GND=GPIO16 */
/* BMS status: GPIO46 (input only) */
/* LED backlight: GPIO11 (TPS61040DBV boost) */
/* These GPIOs are left in reset state (input, no pull). */
```

With:

```c
/* ── Trackpad IQS5xx (TPS43-201A-S) ───────────────────────── */
/* PCB connector: header J2, 6-pin 2.54mm. Free-wired to the carrying half. */
#define BOARD_TRACK_SDA_GPIO    GPIO_NUM_40   /* I2C data,  4.7kΩ pull-up to 3.3V */
#define BOARD_TRACK_SCL_GPIO    GPIO_NUM_38   /* I2C clock, 4.7kΩ pull-up to 3.3V */
#define BOARD_TRACK_RST_GPIO    GPIO_NUM_13   /* Reset active-low; 10 ms pulse at boot */
#define BOARD_TRACK_RDY_GPIO    GPIO_NUM_14   /* Data-ready IRQ, active-low, NEGEDGE */
#define BOARD_TRACK_I2C_PORT    I2C_NUM_0
#define BOARD_TRACK_I2C_HZ      400000        /* 400 kHz */
/* IQS5xx default 7-bit address. Verify ADDR pin config on assembled PCB. */
#define BOARD_TRACK_I2C_ADDR    0x74

/* ── E-ink SSD1681 (WeAct 1.54", 200x200, 1bpp) ───────────── */
/* Shares SPI2 bus (MOSI=GPIO48, MISO=GPIO47, SCK=GPIO45) with NRF24. */
#define BOARD_EINK_CS_GPIO      GPIO_NUM_18   /* SPI chip select, active-low */
#define BOARD_EINK_DC_GPIO      GPIO_NUM_12   /* Data/Command: H=data, L=command */
#define BOARD_EINK_RST_GPIO     GPIO_NUM_17   /* Reset active-low; pulse at boot */
#define BOARD_EINK_BUSY_GPIO    GPIO_NUM_1    /* Busy: H=panel busy, L=ready */
#define BOARD_EINK_SPI_HZ       4000000       /* 4 MHz (SSD1681 max 20 MHz; conservative) */

/* ── Other peripheral GPIO (hors scope de ces bricks) ──────── */
/* Battery ADC: GPIO15 (ADC2_CH4), switchable GND=GPIO16 */
/* BMS status: GPIO46 (input-only charge indicator) */
/* LED backlight: GPIO11 (TPS61040DBV boost enable) */
/* These GPIOs are left in reset state until their bricks are implemented. */
```

- [ ] **Step 3: Verify kase_half_left still builds (quick reconfigure)**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_half_left \
    -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware reconfigure 2>&1 | tail -5
```

Expected: reconfigure completes without error (build not triggered yet; that happens in Task 5).

- [ ] **Step 4: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add boards/kase_half_left/board.h
git commit -m "feat(bricks): board.h — trackpad + eink GPIO defines (named constants)"
```

---

## Task 3: Create `half_spi.h` and `half_spi.c`

**Files:**
- Create: `main/periph/half_spi.h`
- Create: `main/periph/half_spi.c`

This module is the single shared SPI2 bus lock. It replaces `s_tx_mutex` in
`half_scan_task.c`. Implementation is trivial but the placement matters for init ordering.

- [ ] **Step 1: Create directory**

```bash
mkdir -p /home/mae/Documents/GitHub/KaSe_firmware/main/periph
```

- [ ] **Step 2: Write `main/periph/half_spi.h`**

```c
#pragma once

/*
 * half_spi.h — Shared SPI2 bus lock for the KaSe half firmware.
 *
 * The SPI2 bus (MOSI=GPIO48, MISO=GPIO47, SCK=GPIO45) is shared between:
 *   - NRF24L01+ (CSN=GPIO35) — accessed by tx_key_event + heartbeat_timer_cb
 *   - SSD1681 e-ink (CS=GPIO18) — accessed by eink_push() in eink_task
 *
 * All SPI transactions must hold this lock. The BUSY wait of the SSD1681
 * (~1-2 s) is performed OUTSIDE the lock (bus is free while panel refreshes).
 *
 * I2C transactions (trackpad IQS5xx) use a separate bus and do NOT need this lock.
 *
 * Init: half_spi_lock_init() must be called once before any half_spi_lock() call,
 *       i.e. before rf_driver_init_tx(), trackpad_init(), and eink_init().
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Initialize the shared SPI2 bus mutex.
 * Must be called exactly once, at the start of half_scan_task (before NRF init). */
void half_spi_lock_init(void);

/* Acquire the SPI2 bus for exclusive use. Blocks until available (portMAX_DELAY). */
void half_spi_lock(void);

/* Release the SPI2 bus. */
void half_spi_unlock(void);
```

- [ ] **Step 3: Write `main/periph/half_spi.c`**

```c
/*
 * half_spi.c — Shared SPI2 bus lock implementation.
 * See half_spi.h for rationale and usage contract.
 */

#include "half_spi.h"
#include "esp_log.h"

static const char *TAG = "half_spi";
static SemaphoreHandle_t s_spi_mutex = NULL;

void half_spi_lock_init(void)
{
    s_spi_mutex = xSemaphoreCreateMutex();
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed — out of heap");
    }
}

void half_spi_lock(void)
{
    xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
}

void half_spi_unlock(void)
{
    xSemaphoreGive(s_spi_mutex);
}
```

- [ ] **Step 4: Add `periph` to CMakeLists.txt INCLUDE_DIRS and add half_spi.c source**

Edit `main/CMakeLists.txt`:

a) In the `if(CONFIG_KASE_HAS_RF_TX)` block, append `half_spi.c` to the source list (it is
   compiled whenever HALF is built — the SPI2 lock is always needed for the NRF sends):

```cmake
# RF TX (NRF24 PTX) + half scan task + shared SPI bus lock — half role only
if(CONFIG_KASE_HAS_RF_TX)
    list(APPEND srcs
        "comm/rf/half_scan_task.c"
        "periph/half_spi.c"
    )
    if(NOT CONFIG_KASE_HAS_RF_RX)
        list(APPEND srcs "comm/rf/rf_driver.c")
    endif()
endif()
```

b) Add `"periph"` to the `INCLUDE_DIRS` list in `idf_component_register(...)`:

```cmake
INCLUDE_DIRS
    "."
    ...existing entries...
    "periph"
```

- [ ] **Step 5: Commit (no build yet — half_scan_task still uses old mutex)**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/half_spi.h main/periph/half_spi.c main/CMakeLists.txt
git commit -m "feat(bricks): half_spi — shared SPI2 bus lock module"
```

---

## Task 4: Refactor `half_scan_task.c` — replace `s_tx_mutex` with `half_spi_lock`

**Files:**
- Modify: `main/comm/rf/half_scan_task.c`

This is a pure mechanical refactor. No behavioral change. Three edits:
1. Remove the `static SemaphoreHandle_t s_tx_mutex` declaration.
2. Add `#include "half_spi.h"` (inside the `#ifndef TEST_HOST` block).
3. Replace `half_spi_lock_init()` call at the top of the task body (where
   `s_tx_mutex = xSemaphoreCreateMutex()` was).
4. Replace `xSemaphoreTake(s_tx_mutex, portMAX_DELAY)` → `half_spi_lock()` (two sites).
5. Replace `xSemaphoreGive(s_tx_mutex)` → `half_spi_unlock()` (two sites).

Also add the `trackpad_init()` / `trackpad_start()` calls (guarded by `KASE_HAS_TRACKPAD`)
in the task body, after NRF init (peripheral init after bus lock is established).

- [ ] **Step 1: Read the full `half_scan_task.c` to locate all four mutex call sites**

```bash
grep -n "s_tx_mutex\|SemaphoreCreate\|SemaphoreTake\|SemaphoreGive" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/half_scan_task.c
```

Expected output (from current code):
- Declaration: `static SemaphoreHandle_t s_tx_mutex;`
- Init: `s_tx_mutex = xSemaphoreCreateMutex();`
- Take ×2: in `tx_key_event` and `heartbeat_timer_cb`
- Give ×2: in `tx_key_event` and `heartbeat_timer_cb`

- [ ] **Step 2: Edit `half_scan_task.c` — remove mutex declaration and add half_spi.h include**

In the `#ifndef TEST_HOST` include block, add `#include "half_spi.h"`.

Remove the line:
```c
static SemaphoreHandle_t s_tx_mutex;
```

- [ ] **Step 3: Edit `half_scan_task.c` — replace mutex init and add peripheral inits**

In `half_scan_task` (task body), replace:
```c
    /* Mutex to serialize NRF SPI sends (key-event task vs heartbeat timer) */
    s_tx_mutex = xSemaphoreCreateMutex();
```
With:
```c
    /* Initialize shared SPI2 bus lock (used by NRF, and by e-ink/trackpad bricks). */
    half_spi_lock_init();
```

After the NRF init block (after the `rf_driver_init_tx` check and its error branch), add:

```c
#if CONFIG_KASE_HAS_TRACKPAD
    /* Trackpad skeleton: init I2C, RST pulse, RDY IRQ, I2C probe.
     * Returns true if the trackpad is physically present on this half. */
    bool trackpad_present = trackpad_init();
    if (trackpad_present) {
        ESP_LOGI(TAG, "trackpad detected — starting trackpad task");
        trackpad_start();
    } else {
        ESP_LOGI(TAG, "trackpad not detected on this half (no I2C ACK) — skipping");
    }
#endif /* CONFIG_KASE_HAS_TRACKPAD */
```

- [ ] **Step 4: Edit `half_scan_task.c` — replace `xSemaphoreTake`/`xSemaphoreGive` calls**

In `tx_key_event()`:
```c
    // BEFORE:
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    bool ok = rf_driver_send(&s_radio, buf, 3);
    xSemaphoreGive(s_tx_mutex);

    // AFTER:
    half_spi_lock();
    bool ok = rf_driver_send(&s_radio, buf, 3);
    half_spi_unlock();
```

In `heartbeat_timer_cb()` — retry block:
```c
    // BEFORE:
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    rf_driver_send(&s_radio, buf, 3);   /* no second retry on failure */
    xSemaphoreGive(s_tx_mutex);

    // AFTER:
    half_spi_lock();
    rf_driver_send(&s_radio, buf, 3);   /* no second retry on failure */
    half_spi_unlock();
```

In `heartbeat_timer_cb()` — heartbeat TX block:
```c
    // BEFORE:
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    bool ok = rf_driver_send(&s_radio, buf, 9);
    xSemaphoreGive(s_tx_mutex);

    // AFTER:
    half_spi_lock();
    bool ok = rf_driver_send(&s_radio, buf, 9);
    half_spi_unlock();
```

- [ ] **Step 5: Add `#if CONFIG_KASE_HAS_TRACKPAD` include for trackpad.h**

At the top of the `#ifndef TEST_HOST` include block, add:

```c
#if CONFIG_KASE_HAS_TRACKPAD
#include "trackpad.h"
#endif
```

- [ ] **Step 6: Verify no remaining references to `s_tx_mutex`**

```bash
grep "s_tx_mutex" /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/half_scan_task.c
```

Expected: empty output.

- [ ] **Step 7: Build half_left (trackpad.c not yet written — expect compile error on include)**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_half_left \
    -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -20
```

If `trackpad.h` is missing, the compile will fail with "No such file or directory: trackpad.h".
That is expected at this stage — proceed to Task 5 to create it, then re-build.

If the build fails for another reason (e.g., undefined `half_spi_lock`), check that:
- `half_spi.c` is in the RF_TX CMake block (Task 3).
- `periph` is in INCLUDE_DIRS.

- [ ] **Step 8: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/half_scan_task.c
git commit -m "refactor(half): replace s_tx_mutex with half_spi_lock/unlock"
```

---

## Task 5: Create `trackpad.h` and `trackpad.c` (skeleton)

**Files:**
- Create: `main/periph/trackpad/trackpad.h`
- Create: `main/periph/trackpad/trackpad.c`

All real wiring (I2C init, RST pulse, RDY ISR, probe) is implemented. The IQS5xx register
read is a clearly-marked stub that sends zeros and returns immediately.

- [ ] **Step 1: Create directory**

```bash
mkdir -p /home/mae/Documents/GitHub/KaSe_firmware/main/periph/trackpad
```

- [ ] **Step 2: Write `main/periph/trackpad/trackpad.h`**

```c
#pragma once

/*
 * trackpad.h — IQS5xx trackpad skeleton API.
 *
 * Hardware: TPS43-201A-S (Azoteq IQS5xx-compatible) on I2C.
 * GPIO assignments from board.h:
 *   SDA=BOARD_TRACK_SDA_GPIO (GPIO40), SCL=BOARD_TRACK_SCL_GPIO (GPIO38)
 *   RST=BOARD_TRACK_RST_GPIO (GPIO13), RDY=BOARD_TRACK_RDY_GPIO (GPIO14)
 *
 * Both halves compile this module (PCB is reversible — presence detected at runtime
 * by I2C ACK probe). Only call trackpad_start() if trackpad_init() returns true.
 *
 * Register reads (IQS5xx protocol) are stubs — see TODO in trackpad.c.
 */

#include <stdbool.h>

/* Initialize trackpad hardware:
 *   - I2C master init on SDA=GPIO40, SCL=GPIO38 at 400 kHz
 *   - Pulse RST=GPIO13 low 10 ms then high; settle 50 ms
 *   - Configure RDY=GPIO14 as input with ISR on NEGEDGE (→ binary semaphore)
 *   - I2C ACK probe at BOARD_TRACK_I2C_ADDR
 * Returns true if trackpad is physically present (ACK received).
 * Returns false if no ACK (trackpad not mounted on this half). */
bool trackpad_init(void);

/* Start the trackpad FreeRTOS task (prio 6, stack 3072, core 0).
 * Only call if trackpad_init() returned true.
 * The task waits on the RDY semaphore, reads (stubs) touch data, and
 * transmits PKT_TRACKPAD over NRF24 via half_spi_lock + rf_driver_send. */
void trackpad_start(void);
```

- [ ] **Step 3: Write `main/periph/trackpad/trackpad.c`**

```c
/*
 * trackpad.c — IQS5xx trackpad skeleton for KaSe half firmware.
 *
 * Skeleton status:
 *   REAL:  I2C init, RST pulse, RDY ISR + semaphore, I2C probe.
 *   STUB:  IQS5xx register read (protocol proprietary — see TODO block).
 *   REAL:  rf_encode_trackpad + half_spi_lock + rf_driver_send (send path).
 *
 * The PCB is reversible: both halves compile this module. Runtime probe
 * (I2C ACK check) determines whether the trackpad is mounted.
 */

#include "trackpad.h"
#include "board.h"           /* BOARD_TRACK_* GPIO + I2C defines */
#include "half_spi.h"        /* half_spi_lock / half_spi_unlock */
#include "rf_packet.h"       /* rf_trackpad_t, rf_encode_trackpad, PKT_TYPE_TRACKPAD */
#include "rf_driver.h"       /* rf_driver_send, rf_radio_t — extern from half_scan_task */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "trackpad";

/* ── I2C bus + device handles ──────────────────────────────── */
static i2c_master_bus_handle_t s_i2c_bus    = NULL;
static i2c_master_dev_handle_t s_i2c_device = NULL;

/* ── RDY interrupt semaphore ───────────────────────────────── */
static SemaphoreHandle_t s_rdy_sem = NULL;

/* ── Packet sequence counter (shared trackpad seq space) ───── */
static volatile uint8_t s_seq = 0;

/* ── Reference to the half's NRF radio (owned by half_scan_task) ── */
/* Declared extern — half_scan_task.c defines it as static s_radio.
 * DESIGN NOTE: If the linker rejects this (static symbol not visible),
 * expose a getter half_scan_get_radio() in half_scan_task.h instead. */
extern rf_radio_t s_radio;   /* half_scan_task.c, non-static (add extern linkage there) */

/* ── RDY GPIO ISR handler — signals the trackpad task ──────── */
static void IRAM_ATTR rdy_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_rdy_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

bool trackpad_init(void)
{
    /* ── I2C master bus init ──────────────────────────────── */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = BOARD_TRACK_I2C_PORT,
        .scl_io_num        = BOARD_TRACK_SCL_GPIO,
        .sda_io_num        = BOARD_TRACK_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   /* external 4.7kΩ pull-ups on PCB */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %d", err);
        return false;
    }

    /* ── RST pulse: 10 ms low, then high, settle 50 ms ─────── */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << BOARD_TRACK_RST_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(BOARD_TRACK_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TRACK_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));   /* IQS5xx startup time after reset */

    /* ── RDY interrupt semaphore + ISR ─────────────────────── */
    s_rdy_sem = xSemaphoreCreateBinary();
    if (s_rdy_sem == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed");
        return false;
    }

    gpio_config_t rdy_cfg = {
        .pin_bit_mask = (1ULL << BOARD_TRACK_RDY_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   /* external pull-up on PCB */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,     /* RDY goes low when data ready */
    };
    gpio_config(&rdy_cfg);
    gpio_install_isr_service(0);   /* no-op if already installed by another driver */
    gpio_isr_handler_add(BOARD_TRACK_RDY_GPIO, rdy_isr_handler, NULL);

    /* ── I2C probe: check device ACKs at BOARD_TRACK_I2C_ADDR ── */
    err = i2c_master_probe(s_i2c_bus, BOARD_TRACK_I2C_ADDR, 50 /* timeout ms */);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "I2C probe: no ACK at 0x%02X — trackpad not present on this half",
                 BOARD_TRACK_I2C_ADDR);
        /* Clean up ISR and bus — not strictly necessary but tidy */
        gpio_isr_handler_remove(BOARD_TRACK_RDY_GPIO);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return false;
    }

    /* ── Add I2C device handle for subsequent transactions ───── */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_TRACK_I2C_ADDR,
        .scl_speed_hz    = BOARD_TRACK_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %d", err);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return false;
    }

    ESP_LOGI(TAG, "trackpad present at I2C 0x%02X — init OK", BOARD_TRACK_I2C_ADDR);
    return true;
}

/* ── Trackpad FreeRTOS task ─────────────────────────────────── */
static void trackpad_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "trackpad_task started");

    for (;;) {
        /* Wait for RDY pin to go low (data-ready from IQS5xx) */
        xSemaphoreTake(s_rdy_sem, portMAX_DELAY);

        /* ----------------------------------------------------------------
         * TODO STUB: Read IQS5xx touch report over I2C.
         *
         * The IQS5xx uses a Window Transfer protocol:
         *   1. Wait for RDY low (done above).
         *   2. I2C read starting at register 0x0000 (InfoFlags, 2 bytes):
         *        - InfoFlags[1:0] = finger count, InfoFlags[7] = ShowReset
         *   3. Continue reading XY data (register 0x0004..0x0014 for 5 fingers):
         *        - Each finger: AbsX (16-bit), AbsY (16-bit), TouchStrength, Area
         *   4. For relative movement: use the relative XY fields (register 0x0012..0x0013).
         *   5. Read button state from InfoFlags.
         *   6. End read with a STOP or an end-of-communication byte if required by IQS5xx.
         *
         * Suggested implementation sequence:
         *   uint8_t reg_addr[2] = {0x00, 0x00};
         *   uint8_t data[10];
         *   i2c_master_transmit_receive(s_i2c_device, reg_addr, 2, data, 10, 50);
         *   int8_t dx = (int8_t)data[...];  // extract from relative XY
         *   int8_t dy = (int8_t)data[...];
         *   uint8_t buttons = data[...] & 0x03;  // button bits
         *
         * Reference: Azoteq IQS550 / IQS572 Application Note AN171.
         * ---------------------------------------------------------------- */

        /* Stub: send zero movement (trackpad present, no actual data yet) */
        rf_trackpad_t tp = {
            .dx       = 0,    /* TODO STUB: replace with actual dx from IQS5xx */
            .dy       = 0,    /* TODO STUB: replace with actual dy */
            .buttons  = 0,    /* TODO STUB: replace with button state */
            .scroll_v = 0,    /* TODO STUB: replace with vertical scroll delta */
            .scroll_h = 0,    /* TODO STUB: replace with horizontal scroll delta */
            .seq      = s_seq++,
        };

        /* Encode and transmit over NRF24 (PKT_TYPE_TRACKPAD = 0x3) */
        uint8_t buf[7];
        rf_encode_trackpad(buf, &tp);
        half_spi_lock();
        rf_driver_send(&s_radio, buf, 7);
        half_spi_unlock();
    }
}

void trackpad_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        trackpad_task, "trackpad",
        3072,   /* stack: 3 KB (I2C + encoding, no large buffers) */
        NULL, 6, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed: %d", (int)ret);
    }
}
```

**Design note on `s_radio` extern:** `s_radio` is currently `static rf_radio_t s_radio` in
`half_scan_task.c`. There are two options:
a) Make it non-static and add `extern rf_radio_t s_radio;` in `trackpad.c` (as shown above).
b) Add `half_scan_get_radio(void)` accessor in `half_scan_task.h`.

Option (a) is simpler but breaks the encapsulation. Option (b) is cleaner. The implementer
should choose (b) if symbol visibility is problematic at link time. The plan shows (a) for
brevity; the commit message should note which was used.

- [ ] **Step 4: Add `trackpad` to CMakeLists.txt**

In `main/CMakeLists.txt`, after the `if(CONFIG_KASE_HAS_RF_TX)` block, add:

```cmake
# Trackpad IQS5xx skeleton — half role only (compiled on both halves; runtime probe)
if(CONFIG_KASE_HAS_TRACKPAD)
    list(APPEND srcs "periph/trackpad/trackpad.c")
endif()
```

In priv_requires, add (only if not already unconditionally present — check first):

```cmake
if(CONFIG_KASE_HAS_TRACKPAD)
    list(APPEND priv_requires esp_driver_i2c)
endif()
```

Also add `"periph/trackpad"` to INCLUDE_DIRS.

- [ ] **Step 5: Build half_left — must compile green**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_half_left \
    -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -30
```

Expected: `Project build complete.`

Common failure modes:
- `undefined reference to 's_radio'` → make `s_radio` non-static in `half_scan_task.c`, or
  add a `half_scan_get_radio()` accessor and update `trackpad.c` accordingly.
- `i2c_master_probe not found` → verify ESP-IDF 5.x API; may be `i2c_master_probe_device`
  in some minor versions. Check `esp_driver_i2c` header.
- `gpio_install_isr_service` already called → this is safe (ESP-IDF returns ESP_ERR_INVALID_STATE,
  not a panic). Wrap with `if (err != ESP_ERR_INVALID_STATE)` check.

- [ ] **Step 6: Build dongle — must not be broken**

```bash
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_dongle \
    -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -10
```

Expected: `Project build complete.` (dongle has KASE_HAS_TRACKPAD=n).

- [ ] **Step 7: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/trackpad/trackpad.h main/periph/trackpad/trackpad.c \
        main/CMakeLists.txt
git commit -m "feat(bricks): trackpad skeleton — I2C init, RST, RDY ISR, probe, TX stub"
```

---

## Task 6: Dongle — implement `PKT_TYPE_TRACKPAD` decode + stub hook in `rf_rx_task.c`

**Files:**
- Modify: `main/comm/rf/rf_rx_task.c`

The dongle's `drain_radio()` function currently has a comment for trackpad. Replace it with
a real decode + stub that does not crash and is ready for Plan Bricks-3 (mouse HID).

- [ ] **Step 1: Read `rf_rx_task.c` around the trackpad comment**

```bash
grep -n "PKT_TYPE_TRACKPAD\|trackpad\|drain_radio" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/rf_rx_task.c
```

Locate the line: `/* PKT_TYPE_TRACKPAD handled in Plan 3 (mouse HID) */`

- [ ] **Step 2: Edit `rf_rx_task.c` — add decode + stub**

Replace:
```c
        /* PKT_TYPE_TRACKPAD handled in Plan 3 (mouse HID) */
```

With:

```c
        } else if (type == PKT_TYPE_TRACKPAD) {
            rf_trackpad_t tp;
            if (rf_decode_trackpad(buf, n, &tp)) {
                /* TODO STUB: forward to mouse HID report.
                 *   Plan 3 will implement:
                 *     hid_send_mouse(tp.dx, tp.dy, tp.buttons, tp.scroll_v, tp.scroll_h)
                 *   This requires TinyUSB HID descriptor update (composite mouse Report ID 2)
                 *   and a mouse report builder in hid_report.c / usb_hid.c.
                 *   For now, drop the packet silently — no crash, no action. */
                (void)tp;
                ESP_LOGD(TAG, "PKT_TRACKPAD dx=%d dy=%d btn=0x%02x (stub — dropped)",
                         tp.dx, tp.dy, tp.buttons);
            }
```

Note the alignment with the surrounding `if (type == PKT_TYPE_KEY)` / `else if (type ==
PKT_TYPE_HEARTBEAT)` structure. The final brace sequence in `drain_radio` should still close
correctly. Read the full function before editing to ensure brace matching.

- [ ] **Step 3: Build dongle — must compile green**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_dongle \
    -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -15
```

Expected: `Project build complete.` No new dependencies; `rf_decode_trackpad` is inline in
`rf_packet.h` (already included by `rf_rx_task.c`).

- [ ] **Step 4: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/rf_rx_task.c
git commit -m "feat(bricks): dongle — PKT_TRACKPAD decode + stub hook (mouse HID TODO)"
```

---

## Task 7: Final build verification — all three targets

- [ ] **Step 1: Build half_left (clean)**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_half_left \
    -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -5
```

Expected: `Project build complete.`

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

Expected: `Project build complete.`

---

## Self-review checklist

- [ ] `half_spi_lock_init()` is called before any `half_spi_lock()` call (before
  `rf_driver_init_tx` in task body). No lock taken before init.
- [ ] `s_tx_mutex` is gone from `half_scan_task.c`. `grep -n s_tx_mutex` returns empty.
- [ ] Both send sites in `half_scan_task.c` (tx_key_event + heartbeat retry + heartbeat send
  = 3 lock/unlock pairs) use `half_spi_lock/unlock`.
- [ ] `trackpad_init()` is called only after NRF init (SPI bus init is prerequisite for
  `half_spi_lock_init`, which happens just before NRF init).
- [ ] `trackpad_start()` is called only if `trackpad_init()` returned true.
- [ ] The dongle `PKT_TYPE_TRACKPAD` branch does not crash on malformed input — guarded by
  `rf_decode_trackpad` return value check.
- [ ] `KASE_HAS_TRACKPAD=n` on dongle and keyboard — dongle build not affected.
- [ ] `KASE_HAS_EINK=y` and `KASE_HAS_TRACKPAD=y` on HALF but no `eink.c` yet — CMake
  block for EINK sources is empty (added in Plan Bricks-2). Kconfig symbols exist, no code yet.
- [ ] `KASE_HAS_ESPNOW=y` on HALF but no espnow sources yet — CMake ESPNOW block is empty
  (added in Plan Bricks-3). Kconfig symbol exists, no code yet.
- [ ] Board.h defines added for trackpad AND e-ink GPIOs (both bricks need them).
- [ ] All three boards (half_left, half_right, dongle) build green.

---

## Out of scope / stubs left for the implementer

| Stub | Location | What remains |
|------|----------|--------------|
| IQS5xx register read | `trackpad.c::trackpad_task` | Read InfoFlags + XY data per IQS5xx AN171; replace zero `rf_trackpad_t` fields |
| IQS5xx end-of-communication | `trackpad.c::trackpad_task` | IQS5xx requires specific end-of-window signalling — consult app note |
| I2C address verification | `board.h BOARD_TRACK_I2C_ADDR` | Verify 0x74 matches ADDR pin config on assembled PCB |
| Mouse HID report on dongle | `rf_rx_task.c::drain_radio` | Plan 3: TinyUSB descriptor + hid_send_mouse() |
| `s_radio` linkage | `trackpad.c` | If static linkage fails, add `half_scan_get_radio()` accessor |
| Battery TX | Not in this plan | Plan Bricks-3 (ESP-NOW) adds battery periodic TX in heartbeat_timer_cb |
