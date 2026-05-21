# Plan 1 — Half Firmware MVP (Matrix Scan + NRF TX)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `kase_half_left` and `kase_half_right` board variant to KaSe_firmware. Each half scans its local 7×5 matrix and transmits PKT_KEY press/release events + PKT_HEARTBEAT (100 ms) to the dongle over NRF24L01+ Enhanced ShockBurst in PTX mode. No keymap engine on the half — the dongle remains the brain.

**Architecture:** Adds a third device role `KASE_DEVICE_ROLE_HALF` + a new capability flag `KASE_HAS_RF_TX`. The half compiles exactly one applicative task: `half_scan_task.c`, which owns the `keyboard_button` component directly (bypassing `matrix_scan.c` entirely) and routes every press/release to `rf_driver_send()`. A periodic `esp_timer` sends PKT_HEARTBEAT with the current pressed-key bitmap. The RF TX path (`rf_driver_init_tx` + `rf_driver_send`) is added to the existing `rf_driver.c` behind `#if CONFIG_KASE_HAS_RF_TX`, making the extension fully retro-compatible with the dongle's PRX path.

**Tech Stack:** ESP-IDF 5.5, `esp_driver_spi`, `esp_timer`, `espressif/keyboard_button` (local component). No new external components.

**Spec reference:** `docs/superpowers/specs/2026-05-21-half-firmware-design.md` (all sections).

**Depends on:** Plans 1 + 2 dongle firmware (NRF PRX + engine already working on the dongle). The half plan is self-contained on the half side, but end-to-end bench validation requires a live dongle.

**Hardware (from spec Section 2 and `~/Documents/PCB-esp/CLAUDE.md`):**

| Signal | GPIO | Notes |
|---|---|---|
| COL0 | GPIO 8 | Matrix output (driven high to select column) |
| COL1 | GPIO 7 | |
| COL2 | GPIO 39 | JTAG_TDI — safe, USB-JTAG used on DevKitC |
| COL3 | GPIO 5 | |
| COL4 | GPIO 2 | |
| COL5 | GPIO 42 | JTAG_TMS — safe, same reason |
| COL6 | GPIO 4 | |
| ROW0 | GPIO 6 | Matrix input (read after column drive) |
| ROW1 | GPIO 37 | |
| ROW2 | GPIO 3 | JTAG_TDI — safe, USB-JTAG used on DevKitC |
| ROW3 | GPIO 9 | |
| ROW4 | GPIO 10 | |
| NRF MOSI | GPIO 48 | SPI2_HOST |
| NRF MISO | GPIO 47 | |
| NRF SCK | GPIO 45 | Strapping pin VDD_SPI — safe: CPOL=0, pull-down at boot |
| NRF CSN | GPIO 35 | Manual GPIO chip select |
| NRF CE | GPIO 36 | Transmit enable pulse |
| NRF IRQ | GPIO 21 | Not used in MVP (polling mode), wired for Phase 2 |

---

## Learned facts baked in (from Plan 1 + Plan 2 execution)

These are non-obvious truths discovered during prior plan execution. Read before touching any file.

1. **`keyboard_button` callback delivers both press AND release in a single event type.** The component has only one event type (`KBD_EVENT_PRESSED`) that fires on any matrix change. The report struct carries `key_pressed_num + key_data[]` (currently pressed) AND `key_release_num + key_release_data[]` (just released this cycle). The half must iterate `key_release_data[]` for releases and `key_data[]` for presses, NOT diff a previous bitmap. It still maintains a local bitmap (`s_pressed_bitmap`) to include in heartbeats.

2. **`TEST_ASSERT` takes 2 args, `TEST_ASSERT_EQ` takes 3 args.** Signature: `TEST_ASSERT(cond, "msg")`, `TEST_ASSERT_EQ(actual, expected, "msg")`. Tests are `void test_xxx(void)` called directly in `test_main.c`'s `main()` — no `RUN_TEST()` macro, just a direct call after an `extern void test_xxx(void)` declaration. Add new `.c` files to `test/CMakeLists.txt` in `add_executable(test_runner ...)`.

3. **Build command requires `-DIDF_TARGET=esp32s3` on first config after `rm sdkconfig`.** The shared `sdkconfig` file must be deleted before switching roles (KEYBOARD ↔ DONGLE ↔ HALF) or the previous board's settings leak. Full command: `rm -f sdkconfig && idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build`.

4. **sdkconfig.defaults naming: the root CMakeLists strips `kase_` prefix.** For `BOARD=kase_half_left` the short name is `half_left`, so the file must be `sdkconfig.defaults.half_left`. Likewise `sdkconfig.defaults.half_right` for the right variant. The spec mentions `sdkconfig.defaults.half` — that name will NOT be picked up. **Decision: create two files (`sdkconfig.defaults.half_left` and `sdkconfig.defaults.half_right`) with identical content.** This is 2 small files vs. a CMake fallback mechanism that would complicate the loader.

5. **`matrix_scan.c` uses a `COLS13`-wide map array** (`const int cols_map[MATRIX_COLS] = { COLS0, …, COLS12 }`). On a half `MATRIX_COLS=7`, so only COLS0–COLS6 are referenced. This file is NOT compiled on the half (`KASE_HAS_LOCAL_MATRIX=n`), but `board.h` must still define the constants cleanly. No COLS7–COLS13 defines needed on half boards.

6. **`half_scan_task.c` placement: `main/comm/rf/half_scan_task.c`** — this directory already contains all RF code and is in the `INCLUDE_DIRS` list. No new include path needed.

7. **`CONFIG_KASE_HAS_RF_TX` priv_requires: add `esp_driver_spi`** — same as `KASE_HAS_RF_RX`. When only `KASE_HAS_RF_TX` is set (no RF_RX), the SPI block is still needed. The CMake block for `RF_RX` already adds it; the `RF_TX` block must add it too in case someone builds half firmware without dongle in the same sdkconfig.

8. **`main.c` HALF branch uses `#elif CONFIG_KASE_DEVICE_ROLE_HALF`** after the existing `#else /* CONFIG_KASE_DEVICE_ROLE_DONGLE */` block. The current structure is `#if !DONGLE … #else /* DONGLE */ … #endif`. To add HALF cleanly, restructure to `#if KEYBOARD … #elif DONGLE … #elif HALF … #endif`. The HALF branch calls `half_scan_task_start()` — nothing else.

9. **`cdc_dongle_stubs.c` must NOT be compiled for HALF.** It is currently gated `if(CONFIG_KASE_DEVICE_ROLE_DONGLE)` in CMakeLists.txt — correct. HALF needs its own stubs for the CDC symbols referenced from `cdc_binary_cmds.c` (tama, bt, display, layout_json). Create `main/comm/cdc/cdc_half_stubs.c` gated on `CONFIG_KASE_DEVICE_ROLE_HALF`.

10. **`board.h` must define `MAX_MATRIX_KEYS`, `MATRIX_ROWS`, `MATRIX_COLS`, `BOARD_DEBOUNCE_TICKS`, `BOARD_MATRIX_SCAN_INTERVAL_US`, `BOARD_MATRIX_COL2ROW`.** These are all consumed by engine code that links on every role. On the half the engine is compiled but never called — the defines still need to exist.

---

## File Structure

### Created

| Path | Responsibility |
|---|---|
| `boards/kase_half_left/board.h` | Pinout matrix + NRF left: GPIO values, channel 0x4C, addr suffix 0x01, all required defines |
| `boards/kase_half_left/board_keymap.c` | Keymap placeholder (required by build; half never calls engine) |
| `boards/kase_half_right/board.h` | `#include "../kase_half_left/board.h"` + override SIDE/channel/addr_suffix |
| `boards/kase_half_right/board_keymap.c` | Identical placeholder |
| `sdkconfig.defaults.half_left` | UART console on, BT off, WiFi off, TinyUSB CDC only, PM off |
| `sdkconfig.defaults.half_right` | Identical content to `sdkconfig.defaults.half_left` |
| `main/comm/rf/half_scan_task.h` | `half_scan_task_start()` declaration |
| `main/comm/rf/half_scan_task.c` | keyboard_button init + on_key_event TX + heartbeat timer |
| `main/comm/cdc/cdc_half_stubs.c` | No-op stubs for tama/bt/display/layout symbols on half |
| `test/test_half_matrix_diff.c` | Host tests for bitmap diff helper (`half_diff_emit`) |

### Modified

| Path | Change |
|---|---|
| `main/Kconfig.projbuild` | Add `KASE_DEVICE_ROLE_HALF` choice + `KASE_HAS_RF_TX` flag |
| `main/CMakeLists.txt` | Add `KASE_HAS_RF_TX` block (half_scan_task.c + rf_driver.c when not already in RF_RX); add `cdc_half_stubs.c` under `KASE_DEVICE_ROLE_HALF`; add `esp_driver_spi` under `KASE_HAS_RF_TX` |
| `main/comm/rf/rf_driver.h` | Add `rf_driver_init_tx()` + `rf_driver_send()` declarations under `#if CONFIG_KASE_HAS_RF_TX` |
| `main/comm/rf/rf_driver.c` | Add PTX register constants + `rf_driver_init_tx()` + `rf_driver_send()` + `s_max_rt_count` extern under `#if CONFIG_KASE_HAS_RF_TX` |
| `main/main.c` | Restructure `#if !DONGLE … #else … #endif` → `#if KEYBOARD … #elif DONGLE … #elif HALF … #endif`; add HALF include of `half_scan_task.h`; add HALF boot sequence |
| `test/CMakeLists.txt` | Add `test_half_matrix_diff.c` to `add_executable` |
| `test/test_main.c` | Add `extern void test_half_matrix_diff(void)` + direct call in `main()` |

### Untouched (validated post-merge)

```
main/comm/rf/rf_packet.h          codec partagé (half uses rf_encode_key/rf_encode_heartbeat/rf_bitmap_set)
main/comm/rf/rf_driver.c          PRX path (rf_driver_init / rf_driver_read_rx) — unchanged
main/comm/rf/heartbeat.h/c        dongle-only reconciliation — not compiled on half
main/comm/rf/rf_rx_task.c         dongle-only — not compiled on half
boards/kase_dongle/board_rf.h     dongle RF cfg — unchanged
partitions.csv                    shared, unchanged
```

---

## Task 1: Add `KASE_DEVICE_ROLE_HALF` and `KASE_HAS_RF_TX` to Kconfig

**Files:**
- Modify: `main/Kconfig.projbuild`

- [ ] **Step 1: Read current Kconfig**

```bash
cat /home/mae/Documents/GitHub/KaSe_firmware/main/Kconfig.projbuild
```

Confirm: two choices (`KEYBOARD`, `DONGLE`), six flags (`HAS_LOCAL_MATRIX`, `HAS_DISPLAY`, `HAS_BLE`, `HAS_TAMA`, `HAS_RF_RX`, `HAS_ESPNOW`).

- [ ] **Step 2: Edit `main/Kconfig.projbuild`**

Replace the entire file content with:

```kconfig
menu "KaSe firmware"

choice KASE_DEVICE_ROLE
    prompt "Device role"
    default KASE_DEVICE_ROLE_KEYBOARD
    help
        Selects the type of device this firmware runs on.
        KEYBOARD = standalone keyboard with local matrix (V1, V2, V2D).
        DONGLE   = USB receiver dongle for split wireless setup.
        HALF     = split keyboard half, transmits matrix events to dongle.

config KASE_DEVICE_ROLE_KEYBOARD
    bool "Keyboard (V1/V2/V2D)"

config KASE_DEVICE_ROLE_DONGLE
    bool "Dongle (RX from halves)"

config KASE_DEVICE_ROLE_HALF
    bool "Half (TX to dongle via NRF24)"

endchoice

config KASE_HAS_LOCAL_MATRIX
    bool
    default y if KASE_DEVICE_ROLE_KEYBOARD
    default n
    help
        Compiles matrix_scan.c and keyboard_task.c local-scan path.
        NOTE: the half has a physical matrix but manages it directly in
        half_scan_task.c — it does NOT set this flag.

config KASE_HAS_DISPLAY
    bool
    default y if KASE_DEVICE_ROLE_KEYBOARD
    default n
    help
        Compiles display backends and status display task.

config KASE_HAS_BLE
    bool
    default y if KASE_DEVICE_ROLE_KEYBOARD
    default n
    help
        Compiles BLE HID stack (ESP-IDF Bluedroid).

config KASE_HAS_TAMA
    bool
    default y if KASE_DEVICE_ROLE_KEYBOARD
    default n
    help
        Compiles Tamagotchi virtual pet.

config KASE_HAS_RF_RX
    bool
    default y if KASE_DEVICE_ROLE_DONGLE
    default n
    help
        Compiles NRF24L01+ RX driver and rf_rx_task (dongle role).

config KASE_HAS_RF_TX
    bool
    default y if KASE_DEVICE_ROLE_HALF
    default n
    help
        Compiles NRF24L01+ TX path (rf_driver_init_tx, rf_driver_send)
        and half_scan_task.c. Half role only.

config KASE_HAS_ESPNOW
    bool
    default y if KASE_DEVICE_ROLE_DONGLE
    default n
    help
        Compiles ESP-NOW cold path stack (Plan 5).

endmenu
```

- [ ] **Step 3: Sanity check — Kconfig parses without error**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 reconfigure 2>&1 | grep -E "ERROR|Kconfig|error:" | head -20
```

Expected: no Kconfig parse errors (build will fail at "board not found" — that is correct at this stage).

- [ ] **Step 4: Commit**

```bash
git add main/Kconfig.projbuild
git commit -m "feat(half): add KASE_DEVICE_ROLE_HALF + KASE_HAS_RF_TX to Kconfig"
```

---

## Task 2: Create `boards/kase_half_left/` board files

**Files:**
- Create: `boards/kase_half_left/board.h`
- Create: `boards/kase_half_left/board_keymap.c`

- [ ] **Step 1: Create directory and write `board.h`**

```bash
mkdir -p /home/mae/Documents/GitHub/KaSe_firmware/boards/kase_half_left
```

Write `boards/kase_half_left/board.h`:

```c
#ifndef BOARD_H
#define BOARD_H

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#endif

/* ── Product info ──────────────────────────────────────────── */
#define GATTS_TAG           "KaSe_Half"
#define MANUFACTURER_NAME   "KaSe"
#define PRODUCT_NAME        "KaSe Half Left"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0x01   /* left half */

/* ── Half side ─────────────────────────────────────────────── */
#define HALF_SIDE_LEFT  1
#define HALF_SIDE       HALF_SIDE_LEFT

/* ── Matrix 7 cols × 5 rows — COL2ROW, DevKitC ESP32-S3 ────── */
/* Note: the half is a physical matrix but NOT managed by matrix_scan.c.
 * MATRIX_ROWS/COLS/MAX_MATRIX_KEYS are still required because engine code
 * (key_processor.c, keymap.c, etc.) references these defines at compile time. */
#define MATRIX_ROWS         5
#define MATRIX_COLS         7
#define MAX_MATRIX_KEYS     (MATRIX_ROWS * MATRIX_COLS)   /* 35 */

/* COL = output (driven), ROW = input (read). COL2ROW topology. */
#define COLS0   GPIO_NUM_8
#define COLS1   GPIO_NUM_7
#define COLS2   GPIO_NUM_39   /* JTAG_TDI — safe: DevKitC uses USB-JTAG, gpio_reset_pin clears */
#define COLS3   GPIO_NUM_5
#define COLS4   GPIO_NUM_2
#define COLS5   GPIO_NUM_42   /* JTAG_TMS — safe: same reason */
#define COLS6   GPIO_NUM_4

#define ROWS0   GPIO_NUM_6
#define ROWS1   GPIO_NUM_37
#define ROWS2   GPIO_NUM_3    /* JTAG_TDI — safe: same reason as COL2 */
#define ROWS3   GPIO_NUM_9
#define ROWS4   GPIO_NUM_10

/* ── Matrix scan timing (consumed by half_scan_task which creates kbd_cfg) ─ */
#define BOARD_MATRIX_COL2ROW
#define BOARD_MATRIX_SCAN_INTERVAL_US   1000   /* 1 ms between scans */
#define BOARD_MATRIX_SETTLING_US        0
#define BOARD_MATRIX_RECOVERY_US        0
#define BOARD_DEBOUNCE_TICKS            3      /* ~3 ms debounce */

/* ── NRF24L01+ PTX ─────────────────────────────────────────── */
#define BOARD_NRF_SPI_HOST        SPI2_HOST
#define BOARD_NRF_SPI_MOSI        GPIO_NUM_48
#define BOARD_NRF_SPI_MISO        GPIO_NUM_47
#define BOARD_NRF_SPI_SCK         GPIO_NUM_45   /* strapping pin VDD_SPI — safe: CPOL=0 */
#define BOARD_NRF_SPI_CLOCK_HZ    (10 * 1000 * 1000)   /* 10 MHz NRF24 max */

#define BOARD_NRF_CSN_GPIO        GPIO_NUM_35
#define BOARD_NRF_CE_GPIO         GPIO_NUM_36
#define BOARD_NRF_IRQ_GPIO        GPIO_NUM_21   /* wired; not used in MVP (polled TX) */

/* RF addressing — left half. Must match dongle board_rf.h defaults. */
#define BOARD_NRF_ADDR_SUFFIX     0x01
#define BOARD_NRF_CHANNEL         0x4C   /* 2476 MHz */

/* ── Unused peripheral GPIO — not initialized in MVP ───────── */
/* E-ink: CS=18, DC=12, RES=17, BUSY=1 */
/* Trackpad I2C: SDA=40, SCL=38, RST=13, RDY=14 */
/* Battery ADC: GPIO15 (ADC2_CH4), switchable GND=GPIO16 */
/* BMS status: GPIO46 (input only) */
/* LED backlight: GPIO11 (TPS61040DBV boost) */
/* These GPIOs are left in reset state (input, no pull). */

/* ── No display, no BLE, no LED strip on half ──────────────── */
#define BOARD_DISPLAY_SLEEP_MS    0
#define BOARD_SLEEP_MINS          0
#define BOARD_HAS_LED_STRIP       0

/* ── USB: flash/console only (no HID on half) ──────────────── */
/* GPIO43/44 = UART console TX/RX — available on half (not used by matrix) */
#define BOARD_USB_VID             0x303A
#define BOARD_USB_PID             0x4002   /* half, distinct from dongle 0x4001 */

#endif /* BOARD_H */
```

- [ ] **Step 2: Check what keymap symbols the engine requires**

```bash
grep -E "^extern.*keymaps|extern.*default_layout_names|extern.*macros_list" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/input/keymap.h | head -15
grep -E "NUM_LAYOUTS|LAYERS" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/input/keymap.h | head -10
```

Note the actual `NUM_LAYOUTS` value and `keymaps` array signature for the placeholder below.

- [ ] **Step 3: Write `boards/kase_half_left/board_keymap.c`**

The half never calls the engine. This file exists only so the linker finds `keymaps` and `default_layout_names` symbols, which `keymap.c` and `cdc_binary_cmds.c` reference unconditionally.

```c
/*
 * Keymap placeholder for kase_half_left.
 * The half firmware does NOT use the keymap engine — key events are
 * transmitted raw to the dongle. This file satisfies linker symbol
 * requirements from engine code compiled unconditionally.
 */
#include "key_definitions.h"
#include "keymap.h"

/* Minimal 1-layer keymap (35 positions, all transparent / KC_TRNS = 0). */
uint16_t keymaps[NUM_LAYOUTS][MATRIX_ROWS][MATRIX_COLS] = {
    /* Layer 0 — placeholder (half never processes keycodes locally) */
    {
        { 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0 },
    },
    /* Remaining layers: zero-init by C (NUM_LAYOUTS-1 more layers). */
};

/* Layer name placeholder */
const char *default_layout_names[NUM_LAYOUTS] = {
    "Base",
    /* Remaining entries zero-init by C (NULL pointers). */
};
```

- [ ] **Step 4: Verify directory**

```bash
ls -la /home/mae/Documents/GitHub/KaSe_firmware/boards/kase_half_left/
```

Expected: `board.h`, `board_keymap.c`.

- [ ] **Step 5: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add boards/kase_half_left/
git commit -m "feat(half): kase_half_left board — matrix + NRF PTX pinout"
```

---

## Task 3: Create `boards/kase_half_right/` board files

**Files:**
- Create: `boards/kase_half_right/board.h`
- Create: `boards/kase_half_right/board_keymap.c`

The right variant inherits everything from left and overrides only the three RF-related values.

- [ ] **Step 1: Create directory**

```bash
mkdir -p /home/mae/Documents/GitHub/KaSe_firmware/boards/kase_half_right
```

- [ ] **Step 2: Write `boards/kase_half_right/board.h`**

```c
#ifndef BOARD_H
#define BOARD_H

/* Inherit full left board definition — all matrix GPIO, NRF SPI bus,
 * timing constants, USB IDs, etc. are identical (reversible PCB). */
#include "../kase_half_left/board.h"

/* ── Override product name ─────────────────────────────────── */
#undef  PRODUCT_NAME
#define PRODUCT_NAME        "KaSe Half Right"
#undef  MODULE_ID
#define MODULE_ID           0x02   /* right half */

/* ── Override side identifier ──────────────────────────────── */
#undef  HALF_SIDE_LEFT
#undef  HALF_SIDE
#define HALF_SIDE_RIGHT     1
#define HALF_SIDE           HALF_SIDE_RIGHT

/* ── Override RF addressing — right half ───────────────────── */
/* Must match dongle board_rf.h: RF_CH_RIGHT_DEFAULT=0x52, suffix=0x02 */
#undef  BOARD_NRF_ADDR_SUFFIX
#undef  BOARD_NRF_CHANNEL
#define BOARD_NRF_ADDR_SUFFIX   0x02
#define BOARD_NRF_CHANNEL       0x52   /* 2482 MHz */

#endif /* BOARD_H */
```

- [ ] **Step 3: Write `boards/kase_half_right/board_keymap.c`**

Identical to the left placeholder — copy the file:

```bash
cp /home/mae/Documents/GitHub/KaSe_firmware/boards/kase_half_left/board_keymap.c \
   /home/mae/Documents/GitHub/KaSe_firmware/boards/kase_half_right/board_keymap.c
```

- [ ] **Step 4: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add boards/kase_half_right/
git commit -m "feat(half): kase_half_right board — inherit left, override side/RF addr"
```

---

## Task 4: Create `sdkconfig.defaults.half_left` and `sdkconfig.defaults.half_right`

**Files:**
- Create: `sdkconfig.defaults.half_left`
- Create: `sdkconfig.defaults.half_right`

The root `CMakeLists.txt` strips `kase_` prefix and loads `sdkconfig.defaults.<short>`. For `BOARD=kase_half_left` the short name is `half_left`. The spec mentions `sdkconfig.defaults.half` — that name is NOT loaded by the current build system. We create two files with identical content.

**Key differences from dongle defaults:**
- Console UART is ON (GPIO43/44 free on half, useful for bring-up). The dongle has `CONSOLE_NONE` to free GPIO43/44 for scanning — the half does not scan those GPIOs.
- TinyUSB HID is OFF (half is not a USB HID device).
- TinyUSB CDC is ON (needed for `idf.py flash monitor` and runtime UART console).

- [ ] **Step 1: Write `sdkconfig.defaults.half_left`**

```ini
# KaSe half_left board overrides — merged on top of sdkconfig.defaults

# --- Device role ---
CONFIG_KASE_DEVICE_ROLE_HALF=y

# --- Console UART active (GPIO43/44 not used by matrix on half) ---
# This differs from the dongle which uses CONFIG_ESP_CONSOLE_NONE=y.
CONFIG_ESP_CONSOLE_UART_DEFAULT=y

# --- Bluetooth disabled (not used in MVP; saves ~80KB heap) ---
CONFIG_BT_ENABLED=n
CONFIG_BT_CONTROLLER_ENABLED=n
CONFIG_BT_BLUEDROID_ENABLED=n

# --- WiFi disabled (ESP-NOW deferred to Phase 2) ---
CONFIG_ESP_WIFI_ENABLED=n

# --- Power management off (simpler for MVP; light-sleep deferred) ---
CONFIG_PM_ENABLE=n

# --- FreeRTOS tick = 1 ms (matches keyboard latency targets) ---
CONFIG_FREERTOS_HZ=1000

# --- TinyUSB: CDC only for flash/console — NO HID on half ---
CONFIG_TINYUSB_CDC_ENABLED=y
CONFIG_TINYUSB_HID_ENABLED=n

# --- CPU 240 MHz fixed (power management off) ---
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
```

- [ ] **Step 2: Create `sdkconfig.defaults.half_right` (identical content)**

```bash
cp /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig.defaults.half_left \
   /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig.defaults.half_right
```

- [ ] **Step 3: Verify loader picks up the file**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 reconfigure 2>&1 | grep -E "per-board|sdkconfig"
```

Expected: `Using per-board sdkconfig defaults: .../sdkconfig.defaults.half_left`

- [ ] **Step 4: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add sdkconfig.defaults.half_left sdkconfig.defaults.half_right
git commit -m "feat(half): sdkconfig.defaults for half_left and half_right boards"
```

---

## Task 5: Update CMake + `main.c` stubs — compile the half role (build green, stub task)

**Goal:** Get a half build that compiles with a minimal stub `half_scan_task_start()` so every downstream task can validate its own changes in isolation.

### 5a — `main/CMakeLists.txt`

- [ ] **Step 1: Read current `main/CMakeLists.txt`**

```bash
cat /home/mae/Documents/GitHub/KaSe_firmware/main/CMakeLists.txt
```

Verify the existing structure: `CONFIG_KASE_HAS_RF_RX` block, `cdc_dongle_stubs.c` block, `INCLUDE_DIRS` list with `"comm/rf"`.

- [ ] **Step 2: Add RF_TX block after the RF_RX block**

After the `if(CONFIG_KASE_HAS_RF_RX) … endif()` block, add:

```cmake
# RF TX (NRF24 PTX) + half scan task — half role only
if(CONFIG_KASE_HAS_RF_TX)
    list(APPEND srcs
        "comm/rf/half_scan_task.c"
    )
    # rf_driver.c is already in the list when KASE_HAS_RF_RX is also set.
    # When building half standalone (no RF_RX), add it here.
    if(NOT CONFIG_KASE_HAS_RF_RX)
        list(APPEND srcs "comm/rf/rf_driver.c")
    endif()
endif()
```

- [ ] **Step 3: Add `cdc_half_stubs.c` for HALF role**

After the existing dongle stubs block (`if(CONFIG_KASE_DEVICE_ROLE_DONGLE) … endif()`), add:

```cmake
# CDC stubs for half role (no-op symbols for tama/bt/display/layout)
if(CONFIG_KASE_DEVICE_ROLE_HALF)
    list(APPEND srcs "comm/cdc/cdc_half_stubs.c")
endif()
```

- [ ] **Step 4: Add `esp_driver_spi` to priv_requires for RF_TX**

After the `if(CONFIG_KASE_HAS_RF_RX) list(APPEND priv_requires esp_driver_spi) endif()` block, add:

```cmake
if(CONFIG_KASE_HAS_RF_TX AND NOT CONFIG_KASE_HAS_RF_RX)
    list(APPEND priv_requires esp_driver_spi)
endif()
```

### 5b — `main/comm/cdc/cdc_half_stubs.c`

- [ ] **Step 5: Read `cdc_dongle_stubs.c` to understand what symbols to stub**

```bash
cat /home/mae/Documents/GitHub/KaSe_firmware/main/comm/cdc/cdc_dongle_stubs.c
```

Note every function/symbol defined there. The half needs the same stubs because `cdc_binary_cmds.c` references the same symbols regardless of role.

- [ ] **Step 6: Write `main/comm/cdc/cdc_half_stubs.c`**

The simplest correct approach: copy the dongle stubs file and update the header comment. The half needs the exact same symbols because `cdc_binary_cmds.c` references tama, BT, status_display, layout_json, `current_layout`, and matrix test mode globals unconditionally.

```bash
cp /home/mae/Documents/GitHub/KaSe_firmware/main/comm/cdc/cdc_dongle_stubs.c \
   /home/mae/Documents/GitHub/KaSe_firmware/main/comm/cdc/cdc_half_stubs.c
```

Then edit the file header comment from "dongle" to "half" using Edit tool. The function bodies are unchanged — they are already correct no-op stubs.

After copying, verify no duplicate symbol errors will occur. The half role must NOT compile `cdc_dongle_stubs.c` (already gated on `CONFIG_KASE_DEVICE_ROLE_DONGLE` in CMake — correct). The half role will compile `cdc_half_stubs.c` (gated on `CONFIG_KASE_DEVICE_ROLE_HALF`). Confirm with:

```bash
grep "cdc_dongle_stubs\|cdc_half_stubs" /home/mae/Documents/GitHub/KaSe_firmware/main/CMakeLists.txt
```

Expected: dongle stubs gated on `DEVICE_ROLE_DONGLE`, half stubs gated on `DEVICE_ROLE_HALF` (added in Step 3 above).

### 5c — Stub `half_scan_task.h` and `half_scan_task.c`

- [ ] **Step 7: Write `main/comm/rf/half_scan_task.h`**

```c
#ifndef HALF_SCAN_TASK_H
#define HALF_SCAN_TASK_H

/* Start the half scan task (keyboard_button init + NRF PTX init + heartbeat timer).
 * Called from app_main() in the HALF role branch. */
void half_scan_task_start(void);

#endif /* HALF_SCAN_TASK_H */
```

- [ ] **Step 8: Write stub `main/comm/rf/half_scan_task.c`** (minimal, just boots)

```c
#include "half_scan_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "half_scan";

static void half_scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "half_scan_task: STUB — awaiting full implementation");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void half_scan_task_start(void)
{
    ESP_LOGI(TAG, "half_scan_task_start (stub)");
    xTaskCreatePinnedToCore(half_scan_task, "half_scan", 4096, NULL, 10, NULL, 0);
}
```

### 5d — Update `main/main.c` HALF branch

- [ ] **Step 9: Read current `main.c` branching structure**

```bash
grep -n "CONFIG_KASE_DEVICE_ROLE" /home/mae/Documents/GitHub/KaSe_firmware/main/main.c
```

Note line numbers of `#if !CONFIG_KASE_DEVICE_ROLE_DONGLE`, `#else /* CONFIG_KASE_DEVICE_ROLE_DONGLE */`, and `#endif`.

- [ ] **Step 10: Add HALF include at the top of `main.c`**

The include block near the top already has:

```c
#if !CONFIG_KASE_DEVICE_ROLE_DONGLE
#include "display_backend.h"
...
#endif
```

Add a HALF-specific include section just before that block:

```c
#if CONFIG_KASE_DEVICE_ROLE_HALF
#include "half_scan_task.h"
#endif
```

- [ ] **Step 11: Restructure the `#else` block to add a `#elif HALF` branch**

The current structure (simplified):

```c
#if !CONFIG_KASE_DEVICE_ROLE_DONGLE
  /* keyboard-only init */
#else  /* CONFIG_KASE_DEVICE_ROLE_DONGLE */
  /* dongle init */
#endif /* CONFIG_KASE_DEVICE_ROLE_DONGLE */
```

Change to:

```c
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
  /* keyboard-only init — unchanged from before */
  if (!safe_mode) {
    /* display init ... */
  }
  rtc_matrix_deinit();
  matrix_setup();
  keyboard_manager_init();
  /* ... rest of keyboard block ... */
#elif CONFIG_KASE_DEVICE_ROLE_DONGLE
  /* dongle init — unchanged from before */
  ESP_LOGI(TAG, "Dongle role: init engine + RF RX");
  {
    extern void tap_hold_init(void);
    /* ... existing dongle block ... */
  }
#elif CONFIG_KASE_DEVICE_ROLE_HALF
  /* Half role: reset matrix GPIOs, init NRF PTX, start scan task. */
  ESP_LOGI(TAG, "Half role: starting NRF PTX + matrix scan");
  if (!safe_mode) {
    half_scan_task_start();
  } else {
    ESP_LOGW(TAG, "Safe mode: skipping half scan task (USB console accessible)");
  }
#endif /* device role */
```

**Important**: the `#if !CONFIG_KASE_DEVICE_ROLE_DONGLE` guard on the keyboard-only includes and `status_display_task` function (lines ~19-27 and ~55-123) must also be narrowed to `#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD` so HALF doesn't try to include display/matrix headers. Update both occurrences.

- [ ] **Step 12: Verify `main.c` compiles for all roles**

```bash
grep -n "CONFIG_KASE_DEVICE_ROLE" /home/mae/Documents/GitHub/KaSe_firmware/main/main.c
```

Expected: every `#if !DONGLE` replaced by `#if KEYBOARD`, every `#else DONGLE` followed by a `#elif HALF` block.

### 5e — First half build

- [ ] **Step 13: Build half_left**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 | tail -30
```

Expected: `Project build complete.` If linker errors occur, read them carefully:
- Undefined reference to a display/BLE symbol → that symbol is referenced from always-compiled code; find where and add a stub to `cdc_half_stubs.c` or narrow the include guard.
- Undefined reference to `current_layout` → `dongle_engine_state.c` is not compiled for HALF; add the missing global to `cdc_half_stubs.c`.
- Duplicate symbol → check that `cdc_dongle_stubs.c` is not leaking into HALF build.

Iterate until build is green.

- [ ] **Step 14: Verify dongle build still works**

```bash
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 | tail -10
```

Expected: `Project build complete.` — the dongle build must not be broken.

- [ ] **Step 15: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/CMakeLists.txt main/comm/cdc/cdc_half_stubs.c \
        main/comm/rf/half_scan_task.h main/comm/rf/half_scan_task.c \
        main/main.c
git commit -m "feat(half): CMake + main.c HALF branch + stub half_scan_task (build green)"
```

---

## Task 6: Host test — `half_diff_emit` pure bitmap diff logic

**Background on keyboard_button callback behavior (critical):**
The `keyboard_button` component fires `KBD_EVENT_PRESSED` on every matrix change (press or release). The callback receives a `keyboard_btn_report_t` with:
- `key_pressed_num + key_data[]`: all keys currently pressed (output_index=col, input_index=row)
- `key_release_num + key_release_data[]`: keys released in this scan cycle

The half does NOT need to diff two bitmaps to find releases — the component delivers `key_release_data[]` directly. However, maintaining a local `s_pressed_bitmap` is still needed for the PKT_HEARTBEAT.

**What to test:** The `half_diff_emit` function converts the `key_release_data[]` and `key_data[]` arrays from a callback report into PKT_KEY encode calls, updating the local bitmap. This pure logic can be extracted and host-tested.

Define in `half_scan_task.c`:

```c
/* Pure function: given arrays of pressed and released positions,
 * call emit_cb for each change and update bitmap.
 * Exposed as non-static for host testing under TEST_HOST. */
void half_diff_emit(
    uint8_t *bitmap,                           /* in/out: local pressed bitmap */
    const keyboard_btn_data_t *pressed,        /* keys pressed this cycle */
    uint32_t press_cnt,
    const keyboard_btn_data_t *released,       /* keys released this cycle */
    uint32_t release_cnt,
    void (*emit_cb)(uint8_t row, uint8_t col, bool pressed, void *ctx),
    void *ctx
);
```

**Files:**
- Modify: `main/comm/rf/half_scan_task.c` (add `half_diff_emit` as a non-static function)
- Modify: `main/comm/rf/half_scan_task.h` (declare `half_diff_emit` under `TEST_HOST` guard)
- Create: `test/test_half_matrix_diff.c`
- Modify: `test/CMakeLists.txt`
- Modify: `test/test_main.c`

- [ ] **Step 1: Add `half_diff_emit` to `half_scan_task.h`**

Add below `half_scan_task_start()`:

```c
#ifdef TEST_HOST
/* Exposed for host-side unit tests only. */
#include <stdint.h>
#include <stdbool.h>

/* Minimal stub for keyboard_btn_data_t in host test context. */
typedef struct {
    uint8_t output_index;   /* col */
    uint8_t input_index;    /* row */
} keyboard_btn_data_t;

void half_diff_emit(
    uint8_t *bitmap,
    const keyboard_btn_data_t *pressed,  uint32_t press_cnt,
    const keyboard_btn_data_t *released, uint32_t release_cnt,
    void (*emit_cb)(uint8_t row, uint8_t col, bool pressed, void *ctx),
    void *ctx
);
#endif /* TEST_HOST */
```

- [ ] **Step 2: Add `half_diff_emit` implementation to `half_scan_task.c`**

Add after the existing stub task (not inside the task function):

```c
#include "rf_packet.h"   /* rf_bitmap_set, RF_HALF_ROWS, RF_HALF_COLS */

void half_diff_emit(
    uint8_t *bitmap,
    const keyboard_btn_data_t *pressed,  uint32_t press_cnt,
    const keyboard_btn_data_t *released, uint32_t release_cnt,
    void (*emit_cb)(uint8_t row, uint8_t col, bool pressed, void *ctx),
    void *ctx)
{
    /* Process releases first (matches keyboard convention) */
    for (uint32_t i = 0; i < release_cnt; i++) {
        uint8_t row = released[i].input_index;
        uint8_t col = released[i].output_index;
        if (row < RF_HALF_ROWS && col < RF_HALF_COLS) {
            rf_bitmap_set(bitmap, row, col, false);
            if (emit_cb) emit_cb(row, col, false, ctx);
        }
    }
    /* Then presses */
    for (uint32_t i = 0; i < press_cnt; i++) {
        uint8_t row = pressed[i].input_index;
        uint8_t col = pressed[i].output_index;
        if (row < RF_HALF_ROWS && col < RF_HALF_COLS) {
            rf_bitmap_set(bitmap, row, col, true);
            if (emit_cb) emit_cb(row, col, true, ctx);
        }
    }
}
```

- [ ] **Step 3: Write `test/test_half_matrix_diff.c`**

```c
#include "test_framework.h"
#include "../main/comm/rf/rf_packet.h"

/* Pull in the half_diff_emit implementation.
 * half_scan_task.c includes rf_packet.h (already in test) and has no
 * other ESP-IDF deps in half_diff_emit itself. */
#define TEST_HOST
/* Provide keyboard_btn_data_t locally (matches the stub in half_scan_task.h) */
typedef struct { uint8_t output_index; uint8_t input_index; } keyboard_btn_data_t;

/* Include the implementation directly from the source file, extracting only
 * half_diff_emit. We compile half_scan_task.c but stub out the RTOS parts. */
#include "../main/comm/rf/half_scan_task.h"

/* Capture emitted events */
typedef struct { uint8_t row, col; bool pressed; } emit_event_t;
static emit_event_t g_events[32];
static int g_evt_count;

static void capture_emit(uint8_t row, uint8_t col, bool pressed, void *ctx)
{
    (void)ctx;
    if (g_evt_count < 32) {
        g_events[g_evt_count].row = row;
        g_events[g_evt_count].col = col;
        g_events[g_evt_count].pressed = pressed;
        g_evt_count++;
    }
}

void test_half_matrix_diff(void)
{
    TEST_SUITE("half_matrix_diff");

    uint8_t bitmap[RF_HALF_BITMAP_BYTES];
    memset(bitmap, 0, sizeof(bitmap));
    g_evt_count = 0;

    /* Case 1: press two keys — no releases */
    keyboard_btn_data_t pressed[2] = { {.output_index=2, .input_index=1},
                                       {.output_index=5, .input_index=3} };
    half_diff_emit(bitmap, pressed, 2, NULL, 0, capture_emit, NULL);

    TEST_ASSERT_EQ(g_evt_count, 2, "two press events emitted");
    TEST_ASSERT(g_events[0].pressed, "first event is press");
    TEST_ASSERT_EQ(g_events[0].row, 1, "first press row");
    TEST_ASSERT_EQ(g_events[0].col, 2, "first press col");
    TEST_ASSERT(g_events[1].pressed, "second event is press");
    TEST_ASSERT_EQ(g_events[1].row, 3, "second press row");
    TEST_ASSERT_EQ(g_events[1].col, 5, "second press col");

    /* Bitmap should reflect both pressed */
    TEST_ASSERT(rf_bitmap_get(bitmap, 1, 2), "bitmap bit 1,2 set");
    TEST_ASSERT(rf_bitmap_get(bitmap, 3, 5), "bitmap bit 3,5 set");
    TEST_ASSERT(!rf_bitmap_get(bitmap, 0, 0), "bitmap bit 0,0 clear");

    /* Case 2: release first key, press a new one */
    g_evt_count = 0;
    keyboard_btn_data_t pressed2[2] = { {.output_index=5, .input_index=3},
                                        {.output_index=0, .input_index=4} };
    keyboard_btn_data_t released2[1] = { {.output_index=2, .input_index=1} };
    half_diff_emit(bitmap, pressed2, 2, released2, 1, capture_emit, NULL);

    /* Releases come first in emit order */
    TEST_ASSERT_EQ(g_evt_count, 2, "one release + one new press = 2 events");
    TEST_ASSERT(!g_events[0].pressed, "first event is release");
    TEST_ASSERT_EQ(g_events[0].row, 1, "released row");
    TEST_ASSERT_EQ(g_events[0].col, 2, "released col");
    TEST_ASSERT(g_events[1].pressed, "second event is press");
    TEST_ASSERT_EQ(g_events[1].row, 4, "new press row");
    TEST_ASSERT_EQ(g_events[1].col, 0, "new press col");

    /* Bitmap reflects final state: 3,5 + 4,0 pressed; 1,2 released */
    TEST_ASSERT(!rf_bitmap_get(bitmap, 1, 2), "1,2 cleared after release");
    TEST_ASSERT(rf_bitmap_get(bitmap, 3, 5), "3,5 still set");
    TEST_ASSERT(rf_bitmap_get(bitmap, 4, 0), "4,0 newly set");

    /* Case 3: out-of-bounds row/col are silently ignored */
    g_evt_count = 0;
    keyboard_btn_data_t oob[1] = { {.output_index=10, .input_index=7} };  /* col=10 > 6, row=7 > 4 */
    half_diff_emit(bitmap, oob, 1, NULL, 0, capture_emit, NULL);
    TEST_ASSERT_EQ(g_evt_count, 0, "out-of-bounds key ignored");

    /* Case 4: all-release, bitmap becomes zero */
    g_evt_count = 0;
    keyboard_btn_data_t rel_all[2] = { {.output_index=5, .input_index=3},
                                       {.output_index=0, .input_index=4} };
    half_diff_emit(bitmap, NULL, 0, rel_all, 2, capture_emit, NULL);
    TEST_ASSERT_EQ(g_evt_count, 2, "two releases emitted");
    uint8_t zero[RF_HALF_BITMAP_BYTES] = {0};
    TEST_ASSERT(memcmp(bitmap, zero, RF_HALF_BITMAP_BYTES) == 0, "bitmap fully cleared");
}
```

- [ ] **Step 4: Add to `test/CMakeLists.txt`**

In the `add_executable(test_runner …)` list, add `test_half_matrix_diff.c` and `../main/comm/rf/half_scan_task.c`:

```cmake
add_executable(test_runner
    ...existing files...
    test_half_matrix_diff.c
    ../main/comm/rf/half_scan_task.c
    test_main.c
)
```

Note: `../main/comm/rf/heartbeat.c` is already in the list (from Plan 2). `rf_packet.h` is a pure header — no `.c` to add.

- [ ] **Step 5: Add to `test/test_main.c`**

Add `extern void test_half_matrix_diff(void);` to the extern declarations block. Add a direct call `test_half_matrix_diff();` in `main()` after `test_heartbeat();`.

- [ ] **Step 6: Run host tests**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -10 && ./test_runner 2>&1 | tail -20
```

Expected: all existing tests pass, `test_half_matrix_diff` passes with no FAILs.

If `half_scan_task.c` compilation fails in the test build (because it includes FreeRTOS headers), wrap the RTOS parts in `#ifndef TEST_HOST` guards. The `half_diff_emit` function itself has no FreeRTOS deps — only the task and timer setup do.

- [ ] **Step 7: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/half_scan_task.h main/comm/rf/half_scan_task.c \
        test/test_half_matrix_diff.c test/CMakeLists.txt test/test_main.c
git commit -m "test(half): half_diff_emit bitmap logic + host tests"
```

---

## Task 7: Extend `rf_driver` with PTX functions (`rf_driver_init_tx` + `rf_driver_send`)

**Files:**
- Modify: `main/comm/rf/rf_driver.h`
- Modify: `main/comm/rf/rf_driver.c`

The PTX functions are added behind `#if CONFIG_KASE_HAS_RF_TX` to keep the dongle binary unchanged. They share all existing helpers (`csn_low/high`, `ce_low/high`, `spi_xfer`, `rf_driver_write_reg`, `rf_driver_read_reg`, `write_reg_buf`, `rf_driver_set_channel`).

### 7a — Add declarations to `rf_driver.h`

- [ ] **Step 1: Read current `rf_driver.h`**

```bash
cat /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/rf_driver.h
```

- [ ] **Step 2: Add PTX declarations at the end of `rf_driver.h`, before `#endif`**

```c
/* ── PTX mode — compiled only when KASE_HAS_RF_TX=y ─────────────── */
#if CONFIG_KASE_HAS_RF_TX

/* Initialize the radio in PTX mode (transmitter).
 * Sets TX_ADDR + RX_ADDR_P0 to the same 5-byte address (required for ESB auto-ACK).
 * Channel: cfg->channel. Data rate: 2 Mbps, 0 dBm, ARC=3, ARD=500 µs, DPL pipe 0.
 * cfg->shares_bus_first=true → initializes the SPI bus (set true for the single half radio).
 * Returns ESP_OK on success; sets radio->present to true. */
esp_err_t rf_driver_init_tx(rf_radio_t *radio, const rf_radio_cfg_t *cfg);

/* Transmit one payload (PTX, polled).
 * Writes W_TX_PAYLOAD, pulses CE high ~15 µs, polls STATUS until TX_DS (ACK received)
 * or MAX_RT (3 retries exhausted). Clears IRQ flags. Flushes TX FIFO on MAX_RT.
 * Timeout ~5 ms (ARC=3 × ARD=500 µs × 2 + margin).
 * Returns true on TX_DS (ACK from dongle). */
bool rf_driver_send(rf_radio_t *radio, const uint8_t *buf, uint8_t len);

/* Count of MAX_RT events accumulated since last reset.
 * half_scan_task reads this to fill PKT_HEARTBEAT.link_q, then clears it. */
extern uint32_t rf_tx_max_rt_count;

#endif /* CONFIG_KASE_HAS_RF_TX */
```

### 7b — Add implementation to `rf_driver.c`

- [ ] **Step 3: Verify existing register and command defines in `rf_driver.c`**

```bash
grep "^#define REG_\|^#define CMD_" /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/rf_driver.c
```

Note which are already defined. The PTX path needs three additional defines not present in the PRX driver:

| Symbol | Value | Usage |
|---|---|---|
| `REG_TX_ADDR` | `0x10` | 5-byte transmit address |
| `CMD_W_TX_PAYLOAD` | `0xA0` | Write payload to TX FIFO |
| `CMD_FLUSH_TX` | `0xE1` | Flush TX FIFO after MAX_RT |

- [ ] **Step 4: Add the PTX block to `rf_driver.c`**

Add at the end of the file, after the existing RX functions:

```c
/* ════════════════════════════════════════════════════════════════
 * PTX mode (half → dongle transmit)
 * Compiled only when KASE_HAS_RF_TX=y. Shares all helpers above.
 * ════════════════════════════════════════════════════════════════ */
#if CONFIG_KASE_HAS_RF_TX

/* Additional registers and commands for PTX mode */
#define REG_TX_ADDR         0x10
#define CMD_W_TX_PAYLOAD    0xA0
#define CMD_FLUSH_TX        0xE1

/* MAX_RT accumulator: read + reset by half_scan_task for PKT_HEARTBEAT.link_q */
uint32_t rf_tx_max_rt_count = 0;

esp_err_t rf_driver_init_tx(rf_radio_t *r, const rf_radio_cfg_t *cfg)
{
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;

    /* CSN + CE as GPIO outputs; CE low (PTX transmits only on CE pulse) */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << cfg->pin_csn) | (1ULL << cfg->pin_ce),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    csn_high(r);
    ce_low(r);

    /* SPI bus — half has one radio, always shares_bus_first=true */
    if (cfg->shares_bus_first) {
        spi_bus_config_t bus = {
            .mosi_io_num   = cfg->pin_mosi,
            .miso_io_num   = cfg->pin_miso,
            .sclk_io_num   = cfg->pin_sck,
            .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 64,
        };
        esp_err_t e = spi_bus_initialize(cfg->spi_host, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = cfg->clock_hz,
        .mode           = 0,    /* CPOL=0, CPHA=0 — safe for GPIO45 strapping pin */
        .spics_io_num   = -1,   /* manual CSN */
        .queue_size     = 1,
        .command_bits   = 0, .address_bits = 0,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(cfg->spi_host, &dev, &r->spi));

    vTaskDelay(pdMS_TO_TICKS(5));   /* power-on settle */

    if (!rf_driver_probe(r)) {
        r->present = false;
        return ESP_FAIL;
    }

    /* Configure registers in PTX mode */
    rf_driver_write_reg(r, REG_CONFIG,     0x00);   /* power down while configuring */
    rf_driver_write_reg(r, REG_EN_AA,      0x01);   /* auto-ack pipe 0 (dongle ACKs our TX) */
    rf_driver_write_reg(r, REG_EN_RXADDR,  0x01);   /* pipe 0 for ACK reception */
    rf_driver_write_reg(r, REG_SETUP_AW,   0x03);   /* 5-byte address */
    rf_driver_write_reg(r, REG_SETUP_RETR, 0x13);   /* ARD=500 µs (bits7:4=0x1), ARC=3 (bits3:0=0x3) */
    rf_driver_set_channel(r, cfg->channel);
    rf_driver_write_reg(r, REG_RF_SETUP,   0x0E);   /* 2 Mbps (RF_DR_HIGH=1, RF_DR_LOW=0), 0 dBm */
    rf_driver_write_reg(r, REG_FEATURE,    0x04);   /* EN_DPL (bit2) */
    rf_driver_write_reg(r, REG_DYNPD,      0x01);   /* DPL on pipe 0 */

    /* TX_ADDR and RX_ADDR_P0 must be the same 5-byte address for ESB auto-ACK.
     * Address = cfg->rx_addr[0..3] + cfg->addr_suffix. */
    uint8_t addr[5];
    memcpy(addr, cfg->rx_addr, 4);
    addr[4] = cfg->addr_suffix;
    write_reg_buf(r, REG_TX_ADDR,    addr, 5);
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);   /* MUST match TX_ADDR for ESB ACK */

    /* Clear any stale IRQ flags, flush TX FIFO */
    rf_driver_write_reg(r, REG_STATUS, 0x70);   /* clear RX_DR|TX_DS|MAX_RT */
    {
        uint8_t c = CMD_FLUSH_TX, rx_byte;
        csn_low(r); spi_xfer(r, &c, &rx_byte, 1); csn_high(r);
    }

    /* PTX mode, power up.
     * CONFIG = 0x3E = 0b00111110:
     *   bit7 MASK_RX_DR = 0 (not masked — RX IRQ enabled, though we don't use it)
     *   bit6 MASK_TX_DS = 0 (TX_DS IRQ enabled — signals ACK received)
     *   bit5 MASK_MAX_RT = 0? Wait — we want MAX_RT IRQ disabled in MVP to avoid
     *       confusion; but IRQ pin not used (polled). Mask bits affect only the IRQ pin.
     *
     * Spec says CONFIG=0x3E with:
     *   MASK_RX_DR=1 (bit6): mask RX IRQ (irrelevant in PTX)
     *   MASK_TX_DS=0 (bit5): TX_DS unmasked
     *   MASK_MAX_RT=0 (bit4): MAX_RT unmasked
     *   EN_CRC=1 (bit3): CRC enabled
     *   CRCO=1 (bit2): 2-byte CRC
     *   PWR_UP=1 (bit1): powered up
     *   PRIM_RX=0 (bit0): PTX mode
     *
     * 0x3E = 0b00111110 → MASK_RX_DR=0, MASK_TX_DS=0, MASK_MAX_RT=1, EN_CRC=1,
     *                       CRCO=1, PWR_UP=1, PRIM_RX=0
     *
     * Cross-check vs datasheet: bit positions in NRF24L01+ CONFIG register:
     *   bit6 = MASK_RX_DR, bit5 = MASK_TX_DS, bit4 = MASK_MAX_RT
     * 0x3E = 0b00111110 → bit6=0, bit5=1, bit4=1, bit3=1, bit2=1, bit1=1, bit0=0
     * → MASK_RX_DR=0 (RX IRQ enabled), MASK_TX_DS=1 (TX_DS IRQ masked = no IRQ pulse
     *   on ACK), MASK_MAX_RT=1 (MAX_RT IRQ masked = no IRQ pulse on fail).
     *
     * MVP uses polling (STATUS register), so all IRQ masks = irrelevant to function.
     * Keep 0x3E as in spec: MASK_TX_DS and MASK_MAX_RT masked = cleaner IRQ pin
     * in case it is ever monitored. RX_DR unmasked = fine (no RX expected in PTX).
     */
    rf_driver_write_reg(r, REG_CONFIG, 0x3E);
    vTaskDelay(pdMS_TO_TICKS(2));   /* Tpd2stby = 1.5 ms (datasheet) */
    /* CE stays LOW in PTX; it is pulsed only during rf_driver_send() */

    r->present = true;
    ESP_LOGI(TAG, "radio PTX ch=0x%02x addr=KaSe.%02x init OK", cfg->channel, cfg->addr_suffix);
    return ESP_OK;
}

bool rf_driver_send(rf_radio_t *r, const uint8_t *buf, uint8_t len)
{
    /* Write payload to TX FIFO */
    uint8_t tx[33], rx_buf[33];
    tx[0] = CMD_W_TX_PAYLOAD;
    memcpy(&tx[1], buf, len);
    csn_low(r);
    spi_xfer(r, tx, rx_buf, (size_t)(len + 1));
    csn_high(r);

    /* Pulse CE high ≥ 10 µs to trigger transmission */
    ce_high(r);
    esp_rom_delay_us(15);   /* 15 µs > 10 µs minimum (datasheet Table 15) */
    ce_low(r);

    /* Poll STATUS until TX_DS (bit5 = ACK received) or MAX_RT (bit4 = all retries failed).
     * Worst-case time: ARC=3 retries × ARD=500 µs × 2 (on-air) ≈ 3 ms + margin → 5 ms. */
    uint32_t deadline_us = (uint32_t)(esp_timer_get_time() + 5000);
    uint8_t status;
    do {
        status = rf_driver_read_reg(r, REG_STATUS);
        if (status & 0x30) break;   /* TX_DS=bit5 or MAX_RT=bit4 set */
    } while ((uint32_t)esp_timer_get_time() < deadline_us);

    bool success = (status & 0x20) != 0;   /* bit5 TX_DS = ACK received */

    if (status & 0x10) {
        /* MAX_RT: flush TX FIFO or the failed packet blocks all future sends */
        uint8_t c = CMD_FLUSH_TX;
        csn_low(r); spi_xfer(r, &c, rx_buf, 1); csn_high(r);
        rf_tx_max_rt_count++;
    }

    /* Clear TX_DS + MAX_RT flags in STATUS (write 1 to clear) */
    rf_driver_write_reg(r, REG_STATUS, 0x30);

    if (success) r->pkt_rx++;   /* reuse pkt_rx as pkt_tx_ok counter */
    return success;
}

#endif /* CONFIG_KASE_HAS_RF_TX */
```

- [ ] **Step 5: Build dongle to verify no regressions**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 | tail -10
```

Expected: `Project build complete.` — the `#if CONFIG_KASE_HAS_RF_TX` guards must keep the dongle binary identical in size/structure.

- [ ] **Step 6: Build half_left with PTX path compiled**

```bash
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 | tail -15
```

Expected: `Project build complete.` — `rf_driver_init_tx` and `rf_driver_send` now linked.

- [ ] **Step 7: Commit**

```bash
git add main/comm/rf/rf_driver.h main/comm/rf/rf_driver.c
git commit -m "feat(half): rf_driver PTX — rf_driver_init_tx + rf_driver_send"
```

---

## Task 8: Implement `half_scan_task.c` — full matrix + NRF TX

**Files:**
- Modify: `main/comm/rf/half_scan_task.c` (replace stub with full implementation)

This is the heart of the half firmware. It replaces the stub from Task 5 with the complete implementation.

- [ ] **Step 1: Write the full `main/comm/rf/half_scan_task.c`**

```c
/*
 * half_scan_task.c — KaSe half firmware main task.
 *
 * Owns the keyboard_button component (matrix scan) and the NRF24L01+ PTX
 * radio. On each key event, encodes a PKT_KEY and transmits it to the dongle.
 * A periodic esp_timer sends PKT_HEARTBEAT with the current pressed-key bitmap.
 *
 * Architecture: single FreeRTOS task (half_scan_task, prio 10, core 0).
 * The task body blocks forever on portMAX_DELAY — all work is done in callbacks.
 */
#include "half_scan_task.h"
#include "rf_driver.h"
#include "rf_packet.h"
#include "board.h"           /* BOARD_NRF_*, COLS*, ROWS*, BOARD_DEBOUNCE_TICKS, etc. */

#ifndef TEST_HOST
#include "keyboard_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#endif /* TEST_HOST */

static const char *TAG = "half_scan";

/* ── Radio state ────────────────────────────────────────────── */
static rf_radio_t s_radio;

/* ── Packet sequence counter — shared by PKT_KEY and PKT_HEARTBEAT ─ */
static volatile uint8_t s_seq = 0;

/* ── Local pressed-key bitmap (maintained for PKT_HEARTBEAT) ── */
static uint8_t s_pressed_bitmap[RF_HALF_BITMAP_BYTES];

/* ── Retry state: one pending retry stored on MAX_RT ──────────── */
static volatile bool           s_has_pending_retry = false;
static volatile rf_key_event_t s_pending_retry;

/* ── Link quality counter (MAX_RT events since last heartbeat) ─ */
/* Note: rf_tx_max_rt_count is the global in rf_driver.c. We snapshot
 * and reset it each heartbeat tick. */
extern uint32_t rf_tx_max_rt_count;

/* ── NRF radio config (built from board.h defines) ─────────── */
static rf_radio_cfg_t board_nrf_cfg(void)
{
    rf_radio_cfg_t c = {
        .spi_host        = BOARD_NRF_SPI_HOST,
        .pin_mosi        = BOARD_NRF_SPI_MOSI,
        .pin_miso        = BOARD_NRF_SPI_MISO,
        .pin_sck         = BOARD_NRF_SPI_SCK,
        .clock_hz        = BOARD_NRF_SPI_CLOCK_HZ,
        .pin_csn         = BOARD_NRF_CSN_GPIO,
        .pin_ce          = BOARD_NRF_CE_GPIO,
        .pin_irq         = BOARD_NRF_IRQ_GPIO,
        .channel         = BOARD_NRF_CHANNEL,
        .rx_addr         = { 'K', 'a', 'S', 'e' },   /* base address (4 bytes) */
        .addr_suffix     = BOARD_NRF_ADDR_SUFFIX,     /* 0x01 left, 0x02 right */
        .shares_bus_first = true,                     /* half has exactly one radio */
    };
    return c;
}

/* ────────────────────────────────────────────────────────────── */
/* half_diff_emit — pure bitmap diff helper (also host-tested).   */
/* ────────────────────────────────────────────────────────────── */
void half_diff_emit(
    uint8_t *bitmap,
    const keyboard_btn_data_t *pressed,  uint32_t press_cnt,
    const keyboard_btn_data_t *released, uint32_t release_cnt,
    void (*emit_cb)(uint8_t row, uint8_t col, bool pressed, void *ctx),
    void *ctx)
{
    /* Process releases first (matches keyboard convention) */
    for (uint32_t i = 0; i < release_cnt; i++) {
        uint8_t row = released[i].input_index;
        uint8_t col = released[i].output_index;
        if (row < RF_HALF_ROWS && col < RF_HALF_COLS) {
            rf_bitmap_set(bitmap, row, col, false);
            if (emit_cb) emit_cb(row, col, false, ctx);
        }
    }
    for (uint32_t i = 0; i < press_cnt; i++) {
        uint8_t row = pressed[i].input_index;
        uint8_t col = pressed[i].output_index;
        if (row < RF_HALF_ROWS && col < RF_HALF_COLS) {
            rf_bitmap_set(bitmap, row, col, true);
            if (emit_cb) emit_cb(row, col, true, ctx);
        }
    }
}

/* ── Transmit one key event — called from emit_cb ─────────── */
static void tx_key_event(uint8_t row, uint8_t col, bool pressed, void *ctx)
{
    (void)ctx;
    rf_key_event_t e = {
        .row      = row,
        .col      = col,
        .pressed  = pressed,
        .is_retry = false,
        .seq      = s_seq++,   /* post-increment; wraps 0xFF→0x00 naturally */
    };
    uint8_t buf[3];
    rf_encode_key(buf, &e);

    bool ok = rf_driver_send(&s_radio, buf, 3);
    if (!ok) {
        /* Store for retry in next heartbeat tick */
        s_pending_retry = e;
        s_pending_retry.is_retry = true;
        s_has_pending_retry = true;
        ESP_LOGD(TAG, "TX failed row=%u col=%u pressed=%u — queued retry", row, col, pressed);
    }
}

/* ── keyboard_button callback: called from the kb_button task ── */
static void keyboard_btn_cb(keyboard_btn_handle_t kbd_handle,
                            keyboard_btn_report_t  kbd_report,
                            void                  *user_data)
{
    (void)kbd_handle;
    (void)user_data;

    /* Delegate to half_diff_emit: it updates the bitmap and calls tx_key_event
     * for each change (releases first, then presses). */
    half_diff_emit(
        s_pressed_bitmap,
        kbd_report.key_data,         kbd_report.key_pressed_num,
        kbd_report.key_release_data, kbd_report.key_release_num,
        tx_key_event, NULL);
}

/* ── Heartbeat timer callback (100 ms periodic) ─────────────── */
static void heartbeat_timer_cb(void *arg)
{
    (void)arg;

    /* Best-effort retry for the last failed key event */
    if (s_has_pending_retry) {
        uint8_t buf[3];
        rf_key_event_t retry_evt = s_pending_retry;   /* snapshot (volatile struct copy) */
        retry_evt.seq = s_seq++;
        rf_encode_key(buf, &retry_evt);
        rf_driver_send(&s_radio, buf, 3);   /* no second retry on failure */
        s_has_pending_retry = false;
    }

    /* Build and transmit PKT_HEARTBEAT */
    rf_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));
    memcpy(hb.bitmap, s_pressed_bitmap, RF_HALF_BITMAP_BYTES);
    hb.batt_dV = 0;                     /* MVP: battery not measured */
    hb.link_q  = (uint8_t)(rf_tx_max_rt_count > 255 ? 255 : rf_tx_max_rt_count);
    hb.seq     = s_seq++;
    rf_tx_max_rt_count = 0;             /* reset link quality counter */

    uint8_t buf[9];
    rf_encode_heartbeat(buf, &hb);
    bool ok = rf_driver_send(&s_radio, buf, 9);
    if (!ok) {
        ESP_LOGD(TAG, "heartbeat TX failed (MAX_RT)");
    }
}

/* ── Main task body ─────────────────────────────────────────── */
static void half_scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "half_scan_task started");

    /* Reset all matrix GPIO pins to detach bootloader/UART functions.
     * This mirrors the gpio_reset_pin() calls in matrix_scan.c::matrix_setup(). */
    const int col_gpios[MATRIX_COLS] = {
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6
    };
    const int row_gpios[MATRIX_ROWS] = {
        ROWS0, ROWS1, ROWS2, ROWS3, ROWS4
    };
    for (int i = 0; i < MATRIX_COLS; i++) gpio_reset_pin(col_gpios[i]);
    for (int i = 0; i < MATRIX_ROWS; i++) gpio_reset_pin(row_gpios[i]);

    /* Initialize NRF24L01+ in PTX mode */
    rf_radio_cfg_t nrf_cfg = board_nrf_cfg();
    esp_err_t err = rf_driver_init_tx(&s_radio, &nrf_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NRF PTX init failed: %d — halting (safe: USB console OK)", err);
        vTaskDelay(portMAX_DELAY);
        return;
    }

    /* Initialize keyboard_button matrix driver */
    static int output_gpios[MATRIX_COLS];
    static int input_gpios[MATRIX_ROWS];
    for (int i = 0; i < MATRIX_COLS; i++) output_gpios[i] = col_gpios[i];
    for (int i = 0; i < MATRIX_ROWS; i++) input_gpios[i]  = row_gpios[i];

    keyboard_btn_config_t kbd_cfg = {
        .output_gpios     = output_gpios,
        .input_gpios      = input_gpios,
        .output_gpio_num  = MATRIX_COLS,
        .input_gpio_num   = MATRIX_ROWS,
        .active_level     = 1,                        /* COL2ROW: active high */
        .debounce_ticks   = BOARD_DEBOUNCE_TICKS,
        .ticks_interval   = BOARD_MATRIX_SCAN_INTERVAL_US,
        .enable_power_save = false,
        .priority         = 5,
        .core_id          = 0,
    };

    keyboard_btn_handle_t s_kbd = NULL;
    esp_err_t res = keyboard_button_create(&kbd_cfg, &s_kbd);
    if (res != ESP_OK || s_kbd == NULL) {
        ESP_LOGE(TAG, "keyboard_button_create failed: %d", res);
        vTaskDelay(portMAX_DELAY);
        return;
    }

    keyboard_btn_cb_config_t cb_cfg = {
        .event    = KBD_EVENT_PRESSED,   /* fires on any matrix change */
        .callback = keyboard_btn_cb,
        .user_data = NULL,
    };
    res = keyboard_button_register_cb(s_kbd, cb_cfg, NULL);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "keyboard_button_register_cb failed: %d", res);
    }

    /* Start heartbeat timer — 100 ms periodic */
    esp_timer_handle_t hb_timer;
    esp_timer_create_args_t timer_args = {
        .callback        = heartbeat_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "half_hb",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &hb_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(hb_timer, 100 * 1000));   /* 100 ms in µs */

    ESP_LOGI(TAG, "matrix + NRF PTX + heartbeat timer running");

    /* Task is event-driven via callbacks; block indefinitely */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

/* ── Public API ─────────────────────────────────────────────── */
void half_scan_task_start(void)
{
    ESP_LOGI(TAG, "creating half_scan_task (prio 10, stack 4KB, core 0)");
    BaseType_t ret = xTaskCreatePinnedToCore(
        half_scan_task, "half_scan",
        4096,   /* stack: 4 KB (no large buffers; NRF SPI max 33 bytes) */
        NULL, 10, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed: %d", (int)ret);
    }
}
```

- [ ] **Step 2: Build half_left — full implementation**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 | tail -20
```

Expected: `Project build complete.`

If build fails with `esp_rom_delay_us` not found: use `esp_rom_sys.h` include or replace with `vTaskDelay(1)` (1 ms, acceptable for test; for production use `esp_rom_delay_us(15)`).

- [ ] **Step 3: Build half_right**

```bash
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 | tail -10
```

Expected: `Project build complete.`

- [ ] **Step 4: Re-run host tests to confirm no regressions**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -5 && ./test_runner 2>&1 | tail -15
```

Expected: all tests pass including `test_half_matrix_diff`.

- [ ] **Step 5: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/half_scan_task.c
git commit -m "feat(half): half_scan_task full — keyboard_button + NRF PTX + heartbeat"
```

---

## Task 9: Bench validation — flash + NRF probe + end-to-end keypress

**Hardware required:**
- DevKitC ESP32-S3 with half PCB (matrix + NRF24L01+)
- Dongle board (already working from Plans 1+2)
- USB hub: half on `/dev/ttyUSB1` (CP2102N UART), dongle on `/dev/ttyUSB0` (CH340C)

### 9a — Flash and initial boot log

- [ ] **Step 1: Flash half_left**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh
idf.py -B build_half_left -DBOARD=kase_half_left -p /dev/ttyUSB1 flash monitor 2>&1 | head -40
```

**Expected boot log (in order):**

```
KeSp [KaSe Half Left]
Boot count: 1
half_scan_task started
probe: CONFIG=0x0E RF_SETUP=0x0E → OK
radio PTX ch=0x4C addr=KaSe.01 init OK
keyboard_button created: handle=0x...
keyboard_button callback registered
matrix + NRF PTX + heartbeat timer running
```

If probe returns `CONFIG=0x00 RF_SETUP=0x00 → ABSENT`:
- Check SPI wiring: MOSI=GPIO48, MISO=GPIO47, SCK=GPIO45, CSN=GPIO35
- Check CE=GPIO36 low at idle (NRF requires CE low during config)
- Verify VCC=3.3V on NRF module (not 5V)

If probe returns `CONFIG=0xFF RF_SETUP=0xFF → ABSENT`:
- SPI bus not reaching chip; check 100Ω series resistors + MISO pull-up

- [ ] **Step 2: Verify heartbeat transmission in dongle monitor**

Open a second terminal and attach to the dongle:

```bash
idf.py -B build_dongle -DBOARD=kase_dongle -p /dev/ttyUSB0 monitor
```

Expected every ~100 ms:

```
I rf_rx: pkt_rx++ (heartbeat) half=left seq=N batt=0 link_q=0
```

If no heartbeat arrives:
- Check half TX frequency by observing the half monitor — heartbeat_timer_cb should log at LOGD level (may need `idf.py -B build_half_left menuconfig` to enable `CONFIG_LOG_DEFAULT_LEVEL_DEBUG`)
- Verify channel and address match: half=0x4C/0x01, dongle NRF#1=0x4C/0x01
- Check dongle log for NRF#1 IRQ activity

### 9b — Keypress end-to-end

- [ ] **Step 3: Press a key on the half**

Press the key at physical position ROW0/COL0 (top-left corner of the half). Expected on the half monitor:

```
D half_scan: [not shown in INFO level — normal]
```

Expected on the dongle monitor:

```
I rf_rx: PKT_KEY half=left row=0 col=0 pressed=1 seq=N
I rf_rx: pkt_rx++ total=N
```

Expected on the host PC: character appears in focus application. The dongle maps (row=0, col=0) to global key via `keymaps[0][0][0]` — which on the default dongle keymap (Task 4 Plan 1) is `K_ESC`.

- [ ] **Step 4: Press + release several keys, observe no stuck keys**

Press 3 keys simultaneously, release in reverse order. Check dongle monitor shows correct press/release sequence and no extra force-press/force-release from heartbeat reconciliation.

Expected: `link_q=0` in heartbeat (no MAX_RT events), no `hb_reconcile: divergence` log from dongle.

### 9c — Robustness

- [ ] **Step 5: Disconnect and reconnect the half mid-typing**

While a key is held: physically disconnect the half USB power (simulating power loss). Expected on dongle after 250 ms:

```
W rf_rx: half_left timeout — force-releasing all keys
```

The stuck key must be released on the host PC. Reconnect the half — after next heartbeat, the link recovers and keys work again.

- [ ] **Step 6: Build and flash half_right, run simultaneously**

```bash
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build
idf.py -B build_half_right -DBOARD=kase_half_right -p /dev/ttyUSB1 flash
```

With both halves powered and dongle active: press keys on left and right simultaneously. Expected: correct characters on PC, no stuck keys, link_q=0 on both halves.

---

## Task 10: Final cleanup and commit

- [ ] **Step 1: Review all modified files**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git diff --stat HEAD~6  # or git log --oneline to count tasks committed
git status
```

Expected: `sdkconfig` shows as modified (it is gitignored or shared — do not commit it).

- [ ] **Step 2: Verify keyboard builds are still clean**

```bash
rm -f sdkconfig
idf.py -B build_v2 -DBOARD=kase_v2 -DIDF_TARGET=esp32s3 build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 3: Final host test run**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner
```

Expected: all tests pass, `Results: N passed, 0 failed`.

- [ ] **Step 4: Commit any outstanding staged changes**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git status
```

If `sdkconfig` is unstaged and tracked: it is intentionally shared and only committed when it changes functionally. Do not commit it here.

---

## Self-Review Checklist

- [ ] **Spec coverage**: all MVP sections of `2026-05-21-half-firmware-design.md` addressed
  - Section 2: GPIO pinout → `boards/kase_half_left/board.h` (exact GPIO values)
  - Section 3: role/flags → `Kconfig.projbuild` (HALF + HAS_RF_TX)
  - Section 4: `half_scan_task` architecture → Task 8
  - Section 5: `rf_driver_init_tx` register sequence → Task 7 (with CONFIG=0x3E analysis)
  - Section 6: retry applicatif + heartbeat → `heartbeat_timer_cb` in Task 8
  - Section 7: host test `test_half_matrix_diff` → Task 6
  - Section 8: build commands with `-DIDF_TARGET=esp32s3` → everywhere
  - Section 9: GPIO safety notes → `board.h` comments (GPIO39/42/45/3)

- [ ] **No placeholders**: all GPIO numbers concrete (`GPIO_NUM_48`, etc.), all register values hex-explicit (`0x3E`, `0x13`, `0x0E`), no "TBD" in code steps

- [ ] **Type consistency**: `MATRIX_ROWS=5`, `MATRIX_COLS=7`, `MAX_MATRIX_KEYS=35`, `RF_HALF_ROWS=5`, `RF_HALF_COLS=7`, `RF_HALF_BITMAP_BYTES=5` consistent across board.h and rf_packet.h

- [ ] **keyboard_button API**: callback signature `(keyboard_btn_handle_t, keyboard_btn_report_t, void*)` matches `keyboard_button.h`; event type `KBD_EVENT_PRESSED` for both press+release; `key_release_data[]` + `key_release_num` used for releases (not a bitmap diff)

- [ ] **sdkconfig.defaults naming**: two files (`sdkconfig.defaults.half_left`, `sdkconfig.defaults.half_right`) created to match the `kase_` → short-name loader

- [ ] **Retro-compatibility**: dongle builds verified after every task that modifies shared files (`rf_driver.h`, `rf_driver.c`, `main.c`, `CMakeLists.txt`)

- [ ] **Each task ends with a commit**: Tasks 1–9 each have a `git commit` step (Task 9 bench-only, no code commit; Task 10 is the wrap-up)

- [ ] **No Co-Authored-By lines in commit messages**: user preference

---

## Known Limitations / Deferred (out of scope for this plan)

| Limitation | Deferred to |
|---|---|
| `batt_dV` always 0 in heartbeat (battery ADC not read) | Phase 2 (GPIO15 ADC2_CH4 + GPIO16 switchable GND) |
| Light-sleep between matrix scans (power saving) | Phase 2 |
| Deep-sleep with wake-on-keypress | Phase 2 |
| IRQ-driven TX (currently polled STATUS) | Phase 2 (lower latency jitter) |
| ESP-NOW: OTA + config push from dongle to half | Phase 2 |
| E-ink display (SSD1681) on half | Phase 2 |
| Trackpad TPS43-201A-S (I2C) on half | Phase 2 |
| Channel/address pairing runtime change | Phase 2+ |
| Frequency hopping | Phase 2 if interference observed |
| Right half bench validation (requires second DevKitC) | Concurrent with Phase 2 |

## Out of scope for this plan

The following were explicitly considered and excluded from Plan 1 MVP:
- BLE HID bypass (half → host directly)
- USB HID on half
- Layer state display push (dongle → half → e-ink)
- Multi-half bus (more than 2 halves)
