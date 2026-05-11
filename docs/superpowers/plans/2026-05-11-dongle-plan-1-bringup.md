# Plan 1 — Dongle Firmware Bring-up

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `kase_dongle` board variant to KaSe_firmware that builds, flashes, boots, exposes USB CDC binary protocol — without yet doing any RF or key processing. Foundation for Plans 2-5.

**Architecture:** Extends existing board variant system (V1/V2/V2D → +V_dongle). Introduces Kconfig flags `KASE_DEVICE_ROLE_*` and `KASE_HAS_*` that gate compilation of subsystems (matrix, display, BLE, RF, ESP-NOW). `main/CMakeLists.txt` source list becomes conditional. `main/main.c` skips matrix/display/BLE/Tama init when role=DONGLE.

**Tech Stack:** ESP-IDF 5.5, Kconfig, CMake. No new components yet — only build-system surgery.

**Spec reference:** `docs/superpowers/specs/2026-05-11-dongle-firmware-design.md` Sections 1, 2, 6, 10.

**Hardware (from `~/Documents/PCB-esp/dongle/dongle/dongle.kicad_sch` netlist) :**

| Signal | ESP32-S3 GPIO | Notes |
|---|---|---|
| SPI MOSI | GPIO5 | shared, R15 100Ω série |
| SPI MISO | GPIO6 | shared, R16 100Ω série |
| SPI SCK | GPIO7 | shared, R17 100Ω série |
| NRF#1 (half_L) CSN | GPIO13 | direct |
| NRF#1 CE | GPIO14 | direct |
| NRF#1 IRQ | GPIO8 | direct |
| NRF#2 (half_R) CSN | GPIO1 | direct |
| NRF#2 CE | GPIO4 | direct |
| NRF#2 IRQ | GPIO2 | direct |
| USB D+ / D- | GPIO20 / GPIO19 | native OTG |
| Bootstrap IO0 | GPIO0 | flash mode |

---

## File Structure

### Created

| Path | Responsibility |
|---|---|
| `boards/kase_dongle/board.h` | Pinout NRF + matrix dimensions (5×14 = 70 keys) + USB IDs + display backend = NONE + sleep params |
| `boards/kase_dongle/board_keymap.c` | Default 70-key keymap (5 layers minimum, QWERTY split) |
| `boards/kase_dongle/board_layout.c` | Layout names ("base", "lower", "raise", …) |
| `boards/kase_dongle/board_features.h` | Static feature flags consumed by `main.c` (HAS_DISPLAY=0, HAS_BLE=0, etc.) |
| `boards/kase_dongle/README.md` | Pinout doc + build instructions |
| `main/Kconfig.projbuild` | Kconfig device role + capability flags |
| `sdkconfig.defaults.dongle` | Per-board sdkconfig overrides (BT off, WiFi on, FreeRTOS_HZ=1000, etc.) |

### Modified

| Path | Change |
|---|---|
| `CMakeLists.txt` (root) | Sélectionner `sdkconfig.defaults.<board>` si présent |
| `main/CMakeLists.txt` | Sources conditionnelles selon Kconfig flags |
| `main/main.c` | Branchement role-dependent au boot (skip matrix/display/BLE/Tama si dongle) |

### Untouched (validates by hash post-merge)

V1/V2/V2D `board.h`/`board_keymap.c`/`board_layout.c`. Existing engine (`main/input/*`), CDC, USB, BLE code. `partitions.csv`. Tests dans `test/`.

---

## Task 1: Create branch `dongle-firmware`

**Files:** none (git operation)

- [ ] **Step 1: Verify clean working tree**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git status
```

Expected: `working tree clean` on `main` (the design spec was already committed).

- [ ] **Step 2: Create and switch to branch**

```bash
git checkout -b dongle-firmware
```

Expected: `Switched to a new branch 'dongle-firmware'`

- [ ] **Step 3: Verify branch**

```bash
git branch --show-current
```

Expected: `dongle-firmware`

---

## Task 2: Create `boards/kase_dongle/board.h`

**Files:**
- Create: `boards/kase_dongle/board.h`

Reference for layout choice: spec Section 6 says "[5][14] dans `MATRIX_STATE` : cols 0..6 = half_L, cols 7..13 = half_R". `MAX_MATRIX_KEYS=70`. Pas de display, pas de BLE. USB VID/PID dev = 0x303A:0x4001 (spec Section 5).

- [ ] **Step 1: Create directory**

```bash
mkdir -p boards/kase_dongle
```

- [ ] **Step 2: Write `board.h`**

```c
#ifndef BOARD_H
#define BOARD_H

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#endif

/* ── Product info ──────────────────────────────────────────── */
#define GATTS_TAG           "KaSe_Dongle"
#define MANUFACTURER_NAME   "KaSe"
#define PRODUCT_NAME        "KaSe Dongle"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0xD0   /* dongle, distinct des halves 0x01/0x02 */

/* ── Matrix dimensions (logical, 2 halves merged) ──────────── */
#define MATRIX_ROWS         5
#define MATRIX_COLS         14    /* 7 cols half_L + 7 cols half_R */
#define MAX_MATRIX_KEYS     (MATRIX_ROWS * MATRIX_COLS)   /* 70 */

/* Half mapping convention used by RF rx_task and to_global_pos() :
 *   half_L → MATRIX_STATE[row][0..6]
 *   half_R → MATRIX_STATE[row][7..13]
 */
#define COLS_PER_HALF       7
#define HALF_R_COL_OFFSET   7

/* ── No local matrix on dongle — pinout is unused but defined for compile ─ */
#define ROWS0  GPIO_NUM_NC
#define ROWS1  GPIO_NUM_NC
#define ROWS2  GPIO_NUM_NC
#define ROWS3  GPIO_NUM_NC
#define ROWS4  GPIO_NUM_NC
#define COLS0  GPIO_NUM_NC
#define COLS1  GPIO_NUM_NC
#define COLS2  GPIO_NUM_NC
#define COLS3  GPIO_NUM_NC
#define COLS4  GPIO_NUM_NC
#define COLS5  GPIO_NUM_NC
#define COLS6  GPIO_NUM_NC
#define COLS7  GPIO_NUM_NC
#define COLS8  GPIO_NUM_NC
#define COLS9  GPIO_NUM_NC
#define COLS10 GPIO_NUM_NC
#define COLS11 GPIO_NUM_NC
#define COLS12 GPIO_NUM_NC
#define COLS13 GPIO_NUM_NC

/* ── NRF24L01+ pinout (extracted from dongle.kicad_sch netlist) ─ */
#define BOARD_NRF_SPI_HOST       SPI2_HOST
#define BOARD_NRF_SPI_MOSI       GPIO_NUM_5
#define BOARD_NRF_SPI_MISO       GPIO_NUM_6
#define BOARD_NRF_SPI_SCK        GPIO_NUM_7
#define BOARD_NRF_SPI_CLOCK_HZ   (10 * 1000 * 1000)   /* 10 MHz, NRF24 datasheet max */

/* NRF#1 = half_L (channel ch_left, default 0x4C) */
#define BOARD_NRF1_CSN_GPIO      GPIO_NUM_13
#define BOARD_NRF1_CE_GPIO       GPIO_NUM_14
#define BOARD_NRF1_IRQ_GPIO      GPIO_NUM_8

/* NRF#2 = half_R (channel ch_right, default 0x52) */
#define BOARD_NRF2_CSN_GPIO      GPIO_NUM_1
#define BOARD_NRF2_CE_GPIO       GPIO_NUM_4
#define BOARD_NRF2_IRQ_GPIO      GPIO_NUM_2

/* ── No display backend on dongle ──────────────────────────── */
/* DELIBERATELY NO #define BOARD_DISPLAY_BACKEND_* here so root CMakeLists
 * detects the absence and skips display sources. */

/* ── Display sleep / deep sleep — no display, no batt → both 0 ─ */
#define BOARD_DISPLAY_SLEEP_MS    0
#define BOARD_SLEEP_MINS          0

/* ── No LED strip on dongle ────────────────────────────────── */
#define BOARD_HAS_LED_STRIP       0

/* ── Matrix scanning placeholders (consumed by code that may compile but
 *    won't actually scan — gated by CONFIG_KASE_HAS_LOCAL_MATRIX=n) ──── */
#define BOARD_MATRIX_COL2ROW
#define BOARD_MATRIX_SCAN_INTERVAL_US  1000
#define BOARD_MATRIX_SETTLING_US       0
#define BOARD_MATRIX_RECOVERY_US       0
#define BOARD_DEBOUNCE_TICKS           3

/* ── USB identification ────────────────────────────────────── */
/* Dev VID/PID until first public release.
 * Migrate to pid.codes (VID 0x1209) before v4.0 release. */
#define BOARD_USB_VID             0x303A
#define BOARD_USB_PID             0x4001

#endif /* BOARD_H */
```

Save this as `boards/kase_dongle/board.h`.

- [ ] **Step 3: Verify file creation**

```bash
ls -la boards/kase_dongle/board.h && head -20 boards/kase_dongle/board.h
```

Expected: file exists, prints the `#ifndef BOARD_H` header.

---

## Task 3: Create `boards/kase_dongle/board_features.h`

**Files:**
- Create: `boards/kase_dongle/board_features.h`

This file documents the static capability flags for this board. Used by `main.c` for `#if`-conditional init blocks (in addition to Kconfig flags in CMake which gate compilation).

- [ ] **Step 1: Write `board_features.h`**

```c
#ifndef BOARD_FEATURES_H
#define BOARD_FEATURES_H

/* Static capability flags for kase_dongle board.
 * These mirror the Kconfig flags but are visible to C code (Kconfig flags
 * gate CMake compilation; these gate runtime behavior in main.c). */

#define BOARD_HAS_LOCAL_MATRIX    0   /* No local matrix scan */
#define BOARD_HAS_DISPLAY         0   /* No display */
#define BOARD_HAS_BLE             0   /* No Bluetooth */
#define BOARD_HAS_TAMA            0   /* No virtual pet */
#define BOARD_HAS_RF_RX           1   /* 2x NRF24L01+ on shared SPI */
#define BOARD_HAS_ESPNOW          1   /* ESP-NOW cold path (OTA, config) */
#define BOARD_HAS_USB_HID         1   /* USB HID output (NKRO + mouse + consumer) */
#define BOARD_HAS_USB_CDC         1   /* CDC binary protocol */

#endif /* BOARD_FEATURES_H */
```

Save as `boards/kase_dongle/board_features.h`.

- [ ] **Step 2: Verify**

```bash
cat boards/kase_dongle/board_features.h
```

Expected: file content as written.

---

## Task 4: Create `boards/kase_dongle/board_keymap.c`

**Files:**
- Create: `boards/kase_dongle/board_keymap.c`
- Reference: `boards/kase_v2/board_keymap.c` (for format)

Layout : 5 rows × 14 cols (7 left + 7 right). Default keymap = standard QWERTY split. Reuse the keymap macros from `key_definitions.h`.

- [ ] **Step 1: Read reference**

```bash
head -80 boards/kase_v2/board_keymap.c
```

Note: the existing format uses `keymaps[NUM_LAYOUTS][MATRIX_ROWS][MATRIX_COLS] = { ... }` with rows of `{ K_xx, K_yy, ... }`.

- [ ] **Step 2: Write `board_keymap.c`**

```c
/*
 * Default keymap for kase_dongle (5 rows × 14 cols = 70 keys).
 * Cols 0..6  = half_L (left), cols 7..13 = half_R (right).
 *
 * Layout — base layer (QWERTY split-ergo) :
 *
 *   ESC | 1  | 2  | 3  | 4  | 5  | -  ||  =  | 6  | 7  | 8  | 9  | 0  | BSP
 *   TAB | Q  | W  | E  | R  | T  | [  ||  ]  | Y  | U  | I  | O  | P  | \
 *   CTL | A  | S  | D  | F  | G  | (  ||  )  | H  | J  | K  | L  | ;  | '
 *   SHFT| Z  | X  | C  | V  | B  | LO ||  RA | N  | M  | ,  | .  | /  | ENT
 *   --  | -- | -- | GUI| ALT|SPC | LO ||  RA | SPC|RGT |LFT |UP  |DWN | --
 */

#include "key_definitions.h"
#include "keymap.h"

#define K_NONE  0x0000

uint16_t keymaps[NUM_LAYOUTS][MATRIX_ROWS][MATRIX_COLS] = {
    /* Layer 0 — base */
    {
        { K_ESC,  K_1,    K_2,    K_3,    K_4,    K_5,    K_MINUS,
          K_EQ,   K_6,    K_7,    K_8,    K_9,    K_0,    K_BSPC },
        { K_TAB,  K_Q,    K_W,    K_E,    K_R,    K_T,    K_LBR,
          K_RBR,  K_Y,    K_U,    K_I,    K_O,    K_P,    K_BSLS },
        { K_LCTL, K_A,    K_S,    K_D,    K_F,    K_G,    K_LPRN,
          K_RPRN, K_H,    K_J,    K_K,    K_L,    K_SCLN, K_QUOT },
        { K_LSFT, K_Z,    K_X,    K_C,    K_V,    K_B,    MO(1),
          MO(2),  K_N,    K_M,    K_COMM, K_DOT,  K_SLSH, K_ENT },
        { K_NONE, K_NONE, K_NONE, K_LGUI, K_LALT, K_SPC,  MO(1),
          MO(2),  K_SPC,  K_RIGHT, K_LEFT, K_UP,  K_DOWN, K_NONE },
    },
    /* Layer 1 — lower (numbers/symbols) */
    {
        { K_GRV,  K_F1,   K_F2,   K_F3,   K_F4,   K_F5,   K_NONE,
          K_NONE, K_F6,   K_F7,   K_F8,   K_F9,   K_F10,  K_DEL },
        { K_TAB,  K_1,    K_2,    K_3,    K_4,    K_5,    K_NONE,
          K_NONE, K_6,    K_7,    K_8,    K_9,    K_0,    K_BSLS },
        { K_LCTL, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE, K_LEFT, K_DOWN, K_UP,   K_RIGHT,K_NONE, K_NONE },
        { K_LSFT, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE, K_NONE, K_NONE, K_HOME, K_END,  K_NONE, K_NONE },
        { K_NONE, K_NONE, K_NONE, K_LGUI, K_LALT, K_SPC,  K_NONE,
          K_NONE, K_SPC,  K_NONE, K_NONE, K_NONE, K_NONE, K_NONE },
    },
    /* Layer 2 — raise (symbols/nav) */
    {
        { K_TILD, K_F11,  K_F12,  K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_DEL },
        { K_TAB,  K_EXLM, K_AT,   K_HASH, K_DLR,  K_PERC, K_NONE,
          K_NONE, K_CIRC, K_AMPR, K_ASTR, K_LPRN, K_RPRN, K_BSLS },
        { K_LCTL, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE, K_UNDS, K_PLUS, K_LCBR, K_RCBR, K_PIPE, K_NONE },
        { K_LSFT, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE,
          K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE, K_NONE },
        { K_NONE, K_NONE, K_NONE, K_LGUI, K_LALT, K_SPC,  K_NONE,
          K_NONE, K_SPC,  K_NONE, K_NONE, K_NONE, K_NONE, K_NONE },
    },
    /* Layers 3..NUM_LAYOUTS-1: empty (filled by user via controller app) */
};
```

Save as `boards/kase_dongle/board_keymap.c`.

> **Note** : si `K_LBR`, `K_RBR`, `K_TILD`, etc. n'existent pas exactement avec ces noms dans `key_definitions.h`, l'agent doit grep le fichier (`grep "K_LBR\|K_RBR\|K_TILD" main/input/key_definitions.h`) et substituer les noms exacts. Les keycodes peuvent être différents selon les conventions du projet — le but de cette task est juste un keymap par défaut compilable.

- [ ] **Step 3: Validate references in `key_definitions.h`**

```bash
grep -E "^#define K_(LBR|RBR|TILD|EXLM|AT|HASH|DLR|PERC|CIRC|AMPR|ASTR|UNDS|PLUS|LCBR|RCBR|PIPE|GRV|MINUS|EQ|BSLS|LPRN|RPRN|SCLN|QUOT|COMM|DOT|SLSH|ESC|TAB|BSPC|ENT|DEL|HOME|END|LCTL|LSFT|LALT|LGUI|SPC|LEFT|RIGHT|UP|DOWN|F[0-9]+) " main/input/key_definitions.h | head -40
```

Expected: each used symbol resolves. If some don't, edit `board_keymap.c` to use the actual names (e.g., maybe `K_BACKSPACE` instead of `K_BSPC`).

---

## Task 5: Create `boards/kase_dongle/board_layout.c`

**Files:**
- Create: `boards/kase_dongle/board_layout.c`
- Reference: `boards/kase_v2/board_layout.c` and `boards/kase_layout.inc`

Layout names array — used by status display (which we don't have on dongle, but the symbol must resolve for the engine).

- [ ] **Step 1: Read reference**

```bash
cat boards/kase_v2/board_layout.c | head -60
```

- [ ] **Step 2: Write `board_layout.c`**

```c
/*
 * Layout names for kase_dongle.
 * These are used by status display / controller app to label layers.
 */

#include "keymap.h"

const char *default_layout_names[NUM_LAYOUTS] = {
    "Base",
    "Lower",
    "Raise",
    "Adjust",
    "L4",
    "L5",
    "L6",
    "L7",
    /* Trim or extend to match NUM_LAYOUTS exactly. */
};
```

Adapt the array length so it exactly matches `NUM_LAYOUTS` (defined in `keymap.h`). Run:

```bash
grep -E "^#define NUM_LAYOUTS" main/input/keymap.h
```

If `NUM_LAYOUTS` is, say, 8, the array above is correct. If different, add/remove entries.

- [ ] **Step 3: Verify file**

```bash
cat boards/kase_dongle/board_layout.c
```

---

## Task 6: Add Kconfig device role + capability flags

**Files:**
- Create: `main/Kconfig.projbuild`

`Kconfig.projbuild` is auto-discovered by ESP-IDF's build system; no explicit registration needed.

- [ ] **Step 1: Write `main/Kconfig.projbuild`**

```kconfig
menu "KaSe firmware"

choice KASE_DEVICE_ROLE
    prompt "Device role"
    default KASE_DEVICE_ROLE_KEYBOARD
    help
        Selects the type of device this firmware runs on.
        KEYBOARD = standalone keyboard with local matrix (V1, V2, V2D).
        DONGLE   = USB receiver dongle for split wireless setup.

config KASE_DEVICE_ROLE_KEYBOARD
    bool "Keyboard (V1/V2/V2D)"

config KASE_DEVICE_ROLE_DONGLE
    bool "Dongle (RX from halves)"

endchoice

config KASE_HAS_LOCAL_MATRIX
    bool
    default y if KASE_DEVICE_ROLE_KEYBOARD
    default n
    help
        Compiles matrix_scan.c and keyboard_task.c local-scan path.

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
        Compiles NRF24L01+ RX driver and rf_rx_task (Plan 2).

config KASE_HAS_ESPNOW
    bool
    default y if KASE_DEVICE_ROLE_DONGLE
    default n
    help
        Compiles ESP-NOW cold path stack (Plan 5).

endmenu
```

Save as `main/Kconfig.projbuild`.

- [ ] **Step 2: Verify Kconfig is parsable**

```bash
source ~/esp/esp-idf/export.sh && cd /home/mae/Documents/GitHub/KaSe_firmware && idf.py reconfigure -B build_v2 -DBOARD=kase_v2 2>&1 | tail -20
```

Expected: no Kconfig parse errors, build dir reconfigured. If error mentions `Kconfig:`, fix syntax.

---

## Task 7: Create `sdkconfig.defaults.dongle`

**Files:**
- Create: `sdkconfig.defaults.dongle`

Per-board sdkconfig overrides, merged on top of `sdkconfig.defaults` when `BOARD=kase_dongle`.

- [ ] **Step 1: Write `sdkconfig.defaults.dongle`**

```
# KaSe dongle board overrides — merged on top of sdkconfig.defaults

# --- Device role ---
CONFIG_KASE_DEVICE_ROLE_DONGLE=y

# --- Bluetooth disabled (saves ~80KB heap, no BLE on dongle) ---
CONFIG_BT_ENABLED=n
CONFIG_BT_CONTROLLER_ENABLED=n
CONFIG_BT_BLUEDROID_ENABLED=n

# --- WiFi enabled for ESP-NOW (Plan 5), STA mode no AP ---
CONFIG_ESP_WIFI_ENABLED=y
CONFIG_ESP_WIFI_NVS_ENABLED=n
CONFIG_ESP32_WIFI_TASK_PINNED_TO_CORE_1=y

# --- Power management off (USB powered, no battery) ---
CONFIG_PM_ENABLE=n

# --- FreeRTOS tick = 1ms (matches latency budget in spec Section 3) ---
CONFIG_FREERTOS_HZ=1000

# --- Network stack minimal (no TCP/IP needed) ---
CONFIG_LWIP_TCP_ENABLED=n
CONFIG_LWIP_UDP_ENABLED=n
CONFIG_LWIP_IPV6=n

# --- ESP-LCD not needed on dongle ---
CONFIG_LV_USE_LOG=n

# --- TinyUSB: 1 HID interface, 1 CDC interface (Plan 3 expands HID descriptor) ---
CONFIG_TINYUSB_HID_COUNT=1
CONFIG_TINYUSB_CDC_COUNT=1

# --- Logs : only WARN+ in prod, DEBUG compiled out for hot path ---
CONFIG_LOG_DEFAULT_LEVEL_WARN=y
CONFIG_LOG_MAXIMUM_LEVEL_WARN=y
```

Save as `sdkconfig.defaults.dongle`.

- [ ] **Step 2: Verify file**

```bash
cat sdkconfig.defaults.dongle | head -20
```

---

## Task 8: Update root `CMakeLists.txt` to load board-specific defaults

**Files:**
- Modify: `CMakeLists.txt`

Add board-specific sdkconfig defaults file selection so `idf.py -DBOARD=kase_dongle` picks up `sdkconfig.defaults.dongle` automatically.

- [ ] **Step 1: Read current `CMakeLists.txt`**

```bash
cat CMakeLists.txt
```

Note the existing structure: `BOARD_DIR` is set, then `project(KeSp)` is called.

- [ ] **Step 2: Edit `CMakeLists.txt`**

Add after the `BOARD_DIR` block, before `project(KeSp)` :

```cmake
# Per-board sdkconfig defaults: load <repo>/sdkconfig.defaults.<short> if present.
# Short name = portion of BOARD after "kase_" (e.g., kase_dongle → "dongle").
string(REGEX REPLACE "^kase_" "" _BOARD_SHORT "${BOARD}")
set(_PER_BOARD_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.${_BOARD_SHORT}")
if(EXISTS "${_PER_BOARD_DEFAULTS}")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults" "${_PER_BOARD_DEFAULTS}")
    message(STATUS "Using per-board sdkconfig defaults: ${_PER_BOARD_DEFAULTS}")
else()
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
endif()
```

The full edited `CMakeLists.txt` should look like:

```cmake
# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
cmake_minimum_required(VERSION 3.16)

# Board selection: idf.py -DBOARD=kase_v1 build
if(NOT DEFINED BOARD)
    set(BOARD "kase_v2_debug" CACHE STRING "Target board")
endif()
set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}" CACHE INTERNAL "")
if(NOT EXISTS "${BOARD_DIR}/board.h")
    message(FATAL_ERROR "Board '${BOARD}' not found: ${BOARD_DIR}/board.h does not exist")
endif()

# Per-board sdkconfig defaults
string(REGEX REPLACE "^kase_" "" _BOARD_SHORT "${BOARD}")
set(_PER_BOARD_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.${_BOARD_SHORT}")
if(EXISTS "${_PER_BOARD_DEFAULTS}")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults" "${_PER_BOARD_DEFAULTS}")
    message(STATUS "Using per-board sdkconfig defaults: ${_PER_BOARD_DEFAULTS}")
else()
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
endif()

# Detect display backend from board.h
file(READ "${BOARD_DIR}/board.h" _board_h_content)
if(_board_h_content MATCHES "BOARD_DISPLAY_BACKEND_ROUND")
    set(DISPLAY_BACKEND "round" CACHE INTERNAL "")
else()
    set(DISPLAY_BACKEND "oled" CACHE INTERNAL "")
endif()

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(KeSp)
```

Apply the change with `Edit` tool on the original file.

- [ ] **Step 3: Verify edit**

```bash
grep -A2 "Per-board sdkconfig" CMakeLists.txt
```

Expected: the new block is present.

---

## Task 9: Make `main/CMakeLists.txt` conditional

**Files:**
- Modify: `main/CMakeLists.txt`

Source list becomes conditional on Kconfig flags so dongle build skips matrix/display/BLE/Tama sources.

- [ ] **Step 1: Rewrite `main/CMakeLists.txt`**

Replace the entire file content with:

```cmake
# Always-compiled core sources
set(srcs
    "main.c"
    "sys/cpu_time.c"
    "sys/nvs_utils.c"
    "app/dfu_manager.c"
    "app/littlefs_manager.c"

    # USB (always present : HID + CDC)
    "comm/usb/usb_hid.c"
    "comm/hid_transport.c"
    "comm/cdc/cdc_acm_com.c"
    "comm/cdc/cdc_ota.c"
    "comm/cdc/cdc_binary_protocol.c"
    "comm/cdc/cdc_binary_cmds.c"

    # Engine (always compiled — runs on both keyboard and dongle)
    "input/key_processor.c"
    "input/tap_hold.c"
    "input/tap_dance.c"
    "input/combo.c"
    "input/leader.c"
    "input/key_features.c"
    "input/hid_report.c"
    "input/keymap.c"
    "input/key_stats.c"
    "input/keyboard_actions.c"

    # Board-specific keymap + layout (always provided per board)
    "${BOARD_DIR}/board_keymap.c"
    "${BOARD_DIR}/board_layout.c"
)

# Local matrix scan + keyboard task — keyboard role only
if(CONFIG_KASE_HAS_LOCAL_MATRIX)
    list(APPEND srcs
        "input/matrix_scan.c"
        "input/keyboard_task.c"
    )
endif()

# Display backend + status display — keyboard role only
if(CONFIG_KASE_HAS_DISPLAY)
    if(DISPLAY_BACKEND STREQUAL "round")
        list(APPEND srcs
            "display/round/spi_round_display.c"
            "display/round/round_ui.c"
            "display/round/round_backend.c"
        )
    else()
        list(APPEND srcs
            "display/oled/i2c_oled_display.c"
            "display/oled/oled_backend.c"
        )
    endif()
    list(APPEND srcs
        "display/status_display.c"
        "display/assets/img_usb.c"
        "display/assets/img_signal.c"
        "display/assets/img_bluetooth.c"
    )
    if(BOARD_HAS_LED_STRIP)
        list(APPEND srcs "led/led_strip_anim.c")
    endif()
endif()

# BLE HID — keyboard role only
if(CONFIG_KASE_HAS_BLE)
    list(APPEND srcs
        "comm/ble/esp_hidd_prf_api.c"
        "comm/ble/hid_dev.c"
        "comm/ble/hid_device_le_prf.c"
        "comm/ble/hid_bluetooth_manager.c"
    )
endif()

# Tama virtual pet — keyboard role only
if(CONFIG_KASE_HAS_TAMA)
    list(APPEND srcs
        "tama/tama_engine.c"
        "tama/tama_render.c"
    )
endif()

# RF RX (NRF24) — dongle role only (Plan 2 fills these in)
if(CONFIG_KASE_HAS_RF_RX)
    # Plan 2 will add: comm/rf/rf_driver.c, rf_rx_task.c, heartbeat.c
endif()

# ESP-NOW cold path — dongle role only (Plan 5 fills these in)
if(CONFIG_KASE_HAS_ESPNOW)
    # Plan 5 will add: comm/espnow/*.c
endif()

# PRIV_REQUIRES — keep all that current keyboard build needs;
# bt is gated by Kconfig CONFIG_BT_ENABLED so it's safe to leave.
set(priv_requires
    esp_driver_gpio
    esp_timer
    nvs_flash
    unity
    esp_driver_i2c
    app_update
)

if(CONFIG_KASE_HAS_BLE)
    list(APPEND priv_requires bt esp_hid)
endif()

if(CONFIG_KASE_HAS_DISPLAY)
    list(APPEND priv_requires esp_lcd)
endif()

if(CONFIG_KASE_HAS_ESPNOW)
    list(APPEND priv_requires esp_wifi esp_event nvs_flash)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "config"
        "sys"
        "app"
        "display"
        "display/oled"
        "display/round"
        "led"
        "display/assets"
        "comm"
        "comm/usb"
        "comm/cdc"
        "comm/ble"
        "tama"
        "input"
        "${BOARD_DIR}"
    PRIV_REQUIRES ${priv_requires}
)

if(CONFIG_KASE_HAS_DISPLAY)
    littlefs_create_partition_image(storage ../flash_data FLASH_IN_PROJECT)
endif()
```

Use `Write` to replace the file.

- [ ] **Step 2: Verify**

```bash
head -30 main/CMakeLists.txt
```

Expected: starts with `# Always-compiled core sources`.

---

## Task 10: Refactor `main/main.c` for role-conditional init

**Files:**
- Modify: `main/main.c`

Wrap matrix/display/BLE/Tama init in `#if !CONFIG_KASE_DEVICE_ROLE_DONGLE` blocks. Keep CDC + tinyusb + keymap NVS load + OTA validation always-on.

- [ ] **Step 1: Read current `main.c`**

```bash
wc -l main/main.c && head -60 main/main.c
```

Note: 251 lines, includes a lot of headers.

- [ ] **Step 2: Edit `main/main.c`** — wrap the entire safe-mode-conditional block + matrix init + keyboard task + BLE init in `#if !CONFIG_KASE_DEVICE_ROLE_DONGLE`. The dongle role gets a stripped-down `app_main` that just initializes USB and CDC.

Apply this Edit (single replacement covering app_main body):

Replace the existing `app_main` function (from `void app_main(void) {` to the closing `}`) with:

```c
void app_main(void) {
  ESP_LOGI(TAG, "--------------- KeSp [%s] ----------------", PRODUCT_NAME);

  /* Crash-loop detection */
  if (boot_crash_magic != BOOT_CRASH_MAGIC || boot_crash_count > 100) {
    boot_crash_magic = BOOT_CRASH_MAGIC;
    boot_crash_count = 0;
  }
  boot_crash_count++;
  ESP_LOGI(TAG, "Boot count: %lu", (unsigned long)boot_crash_count);

  if (boot_crash_count > BOOT_CRASH_LIMIT) {
    ESP_LOGW(TAG, "Crash loop detected (%lu boots) — SAFE MODE",
             (unsigned long)boot_crash_count);
    safe_mode = true;
    boot_crash_count = 0;
  }

  /* USB + CDC always (both keyboard and dongle expose USB) */
  kase_tinyusb_init();
  init_cdc_commands();
  {
    extern void cdc_binary_cmds_init(void);
    cdc_binary_cmds_init();
  }

  /* Keymap NVS init always (engine consumes keymaps in both roles) */
  keymap_init_nvs();

  if (!safe_mode) {
    load_keymaps((uint16_t *)keymaps,
                 LAYERS * MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t));
    load_layout_names(default_layout_names, LAYERS);
    load_macros(macros_list, MAX_MACROS);
    load_key_stats();
    load_bigram_stats();

    extern void tap_dance_load(void);
    extern void combo_load(void);
    extern void leader_load(void);
    tap_dance_load();
    combo_load();
    leader_load();
    key_override_load();

#if !CONFIG_KASE_DEVICE_ROLE_DONGLE
    extern void tama_engine_init(void);
    bt_devices_load();
    tama_engine_init();
#endif
  } else {
    ESP_LOGW(TAG, "Safe mode: skipping NVS config loading");
  }

#if !CONFIG_KASE_DEVICE_ROLE_DONGLE
  /* --- Keyboard-only init: display, matrix, BLE, LED strip --- */
  if (!safe_mode) {
    ESP_LOGI(TAG, "display init");
#if !SKIP_STATUS_DISPLAY
    {
      extern const display_backend_t
#ifdef BOARD_DISPLAY_BACKEND_ROUND
          round_display_backend;
      display_set_backend(&round_display_backend);
#else
          oled_display_backend;
      display_set_backend(&oled_display_backend);
#endif
    }
    status_display_start();
    xTaskCreatePinnedToCore(status_display_task, "status_disp", 6144, NULL, 2,
                            &status_display_task_handle, 1);
#endif
  } else {
    ESP_LOGW(TAG, "Safe mode: skipping display");
  }

  rtc_matrix_deinit();
  ESP_LOGI(TAG, "Matrix setup init");
  matrix_setup();

  ESP_LOGI(TAG, "Keyboard manager init");
  keyboard_manager_init();

  ESP_LOGI(TAG, "Task Matrix init");
  TaskHandle_t xHandleMatrixKeyboard = NULL;
  static uint8_t ucParameterToPass;
  xTaskCreatePinnedToCore(vTaskKeyboard, "Matrix_Keyboard", 6144,
                          &ucParameterToPass, 3, &xHandleMatrixKeyboard, 0);

  if (!safe_mode) {
    ESP_LOGI(TAG, "bluetooth init check");
    if (load_bt_state()) {
      ESP_LOGI(TAG, "Starting bluetooth (saved state: ON)");
      init_hid_bluetooth();
      uint8_t saved_mode = load_io_mode();
      if (saved_mode == 1 && hid_bluetooth_is_initialized()) {
        usb_bl_state = 1;
        ESP_LOGI(TAG, "Restored HID output mode: BLE");
      }
    } else {
      ESP_LOGI(TAG, "Bluetooth disabled (saved state: OFF)");
    }

#if BOARD_HAS_LED_STRIP
    ESP_LOGI(TAG, "LED Strip init");
    led_strip_test();
    led_strip_start_task();
#endif
  }
#else  /* CONFIG_KASE_DEVICE_ROLE_DONGLE */
  /* --- Dongle role: no matrix, no display, no BLE.
   *     Plan 2 will add NRF init + rf_rx_task here.
   *     Plan 5 will add ESP-NOW init.
   *     For now: just announce we're alive. --- */
  ESP_LOGI(TAG, "Dongle role: matrix/display/BLE skipped, awaiting RF stack (Plan 2)");
#endif /* CONFIG_KASE_DEVICE_ROLE_DONGLE */

  xTaskCreatePinnedToCore(cpu_time_logger_task, "cpu_time", 4096, NULL, 2, NULL,
                          1);

  /* Boot succeeded */
  boot_crash_count = 0;
  esp_ota_mark_app_valid_cancel_rollback();
  ESP_LOGI(TAG, "Boot OK%s", safe_mode ? " (SAFE MODE)" : "");

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
```

- [ ] **Step 3: Wrap `status_display_task` definition (which references display + tama)**

The `status_display_task` function (lines ~50-110) uses `is_layer_changed`, `current_layout`, `status_display_*`, `tama_engine_*` — all unavailable in dongle build. Wrap it:

Add `#if !CONFIG_KASE_DEVICE_ROLE_DONGLE` right after the function comment (`// Task handling status display updates...`) and `#endif` after the function's closing `}`.

Also wrap the includes that aren't compiled on dongle. At the top of `main.c`, change:

```c
#include "display_backend.h"
#include "hid_bluetooth_manager.h"
#include "led_strip_anim.h"
#include "matrix_scan.h"
#include "status_display.h"
#include "tama_engine.h"
```

to:

```c
#if !CONFIG_KASE_DEVICE_ROLE_DONGLE
#include "display_backend.h"
#include "hid_bluetooth_manager.h"
#include "led_strip_anim.h"
#include "matrix_scan.h"
#include "status_display.h"
#include "tama_engine.h"
#endif
```

- [ ] **Step 4: Verify diff is contained**

```bash
git diff --stat main/main.c
```

Expected: only `main/main.c` modified, ~30-50 lines changed.

---

## Task 11: Build `kase_dongle`

**Files:** none (build operation)

- [ ] **Step 1: Source ESP-IDF**

```bash
source ~/esp/esp-idf/export.sh
cd /home/mae/Documents/GitHub/KaSe_firmware
```

- [ ] **Step 2: Configure dongle build**

```bash
rm -rf build_dongle
idf.py -B build_dongle -DBOARD=kase_dongle reconfigure 2>&1 | tail -20
```

Expected: `-- Using per-board sdkconfig defaults: .../sdkconfig.defaults.dongle`. No errors.

- [ ] **Step 3: Verify Kconfig values**

```bash
grep -E "CONFIG_KASE_DEVICE_ROLE|CONFIG_KASE_HAS_|CONFIG_BT_ENABLED|CONFIG_ESP_WIFI_ENABLED|CONFIG_FREERTOS_HZ" build_dongle/sdkconfig | sort
```

Expected (sdkconfig uses `# CONFIG_X is not set` for unset bools, `CONFIG_X=y/n` for set) :
```
# CONFIG_BT_ENABLED is not set
CONFIG_ESP_WIFI_ENABLED=y
CONFIG_FREERTOS_HZ=1000
CONFIG_KASE_DEVICE_ROLE_DONGLE=y
# CONFIG_KASE_HAS_BLE is not set
# CONFIG_KASE_HAS_DISPLAY is not set
CONFIG_KASE_HAS_ESPNOW=y
# CONFIG_KASE_HAS_LOCAL_MATRIX is not set
CONFIG_KASE_HAS_RF_RX=y
# CONFIG_KASE_HAS_TAMA is not set
```

- [ ] **Step 4: Build**

```bash
idf.py -B build_dongle -DBOARD=kase_dongle build 2>&1 | tail -30
```

Expected: `Project build complete.` and a `KeSp.bin` produced.

- [ ] **Step 5: Verify binary exists**

```bash
ls -la build_dongle/KeSp.bin
```

Expected: file exists, size ~600KB-1MB.

- [ ] **Step 6: If build fails** : the most likely causes are :
  - Symbol referenced from `main.c` that's only defined in a removed source. Resolution : add it to the always-compiled list, or wrap the caller.
  - Kconfig `CONFIG_KASE_DEVICE_ROLE_DONGLE` not propagating to C. Verify with `grep CONFIG_KASE build_dongle/config/sdkconfig.h`. The `#if !CONFIG_KASE_DEVICE_ROLE_DONGLE` becomes `#if !1` when defined, which is `#if 0` — correct.
  - Headers like `key_definitions.h` referencing keycodes not present. See Task 4 step 3.

Iterate fixes until build is green.

---

## Task 12: Verify V1/V2/V2D builds untouched

**Files:** none (build operation)

The whole point of "minimal refactor" — V1/V2/V2D must produce binaries equivalent to pre-branch (modulo the new Kconfig defaults populated, which shouldn't affect functional output if defaults are correct).

- [ ] **Step 1: Get pre-branch binary hashes** (from main, before this branch's changes)

```bash
# Build each board against main to get reference hashes
for board in kase_v1 kase_v2 kase_v2_debug; do
    short=${board#kase_}
    git stash || true
    git checkout main -- main/CMakeLists.txt main/main.c CMakeLists.txt 2>/dev/null
    rm -rf build_ref_$short
    idf.py -B build_ref_$short -DBOARD=$board reconfigure build 2>&1 | tail -5
    sha256sum build_ref_$short/KeSp.bin > ref_$short.sha256
    cat ref_$short.sha256
done
git stash pop || git checkout dongle-firmware -- main/CMakeLists.txt main/main.c CMakeLists.txt
```

Save the sha256 sums.

- [ ] **Step 2: Build each board on `dongle-firmware` branch**

```bash
for board in kase_v1 kase_v2 kase_v2_debug; do
    short=${board#kase_}
    rm -rf build_$short
    idf.py -B build_$short -DBOARD=$board reconfigure build 2>&1 | tail -5
    sha256sum build_$short/KeSp.bin
done
```

- [ ] **Step 3: Diff hashes**

```bash
for short in v1 v2 v2_debug; do
    echo "=== $short ==="
    diff <(awk '{print $1}' ref_$short.sha256) <(sha256sum build_$short/KeSp.bin | awk '{print $1}') && echo OK || echo MISMATCH
done
```

**Expected** : `OK` for all 3. **If MISMATCH** : the refactor altered the keyboard build.

Acceptable causes of mismatch :
- Build timestamps embedded in binary (then `git describe`-derived version string differs trivially). To check, run `esptool.py --chip esp32s3 image_info build_v2/KeSp.bin` on both and diff non-timestamp fields.

Unacceptable causes :
- Different code paths compiled (the Kconfig defaults must keep `CONFIG_KASE_DEVICE_ROLE_KEYBOARD=y` for V1/V2/V2D).
- Sources missing from the new conditional CMakeLists.

Fix any unacceptable mismatches by verifying the conditional CMake includes the same files for keyboard role as before.

- [ ] **Step 4: Cleanup ref builds**

```bash
rm -rf build_ref_*  ref_*.sha256
```

---

## Task 13: Document `boards/kase_dongle/README.md`

**Files:**
- Create: `boards/kase_dongle/README.md`

- [ ] **Step 1: Write README**

```markdown
# kase_dongle — KaSe Split Wireless Dongle

USB receiver for the KaSe split-wireless keyboard. Receives keystrokes from 2 halves
via NRF24L01+ and presents as USB HID composite + CDC binary to the host.

Hardware : `~/Documents/PCB-esp/dongle/dongle/` (KiCad 9 project, M.2 Key B 3042 form factor).

## Build & flash

```bash
source ~/esp/esp-idf/export.sh
idf.py -B build_dongle -DBOARD=kase_dongle build
idf.py -B build_dongle -p /dev/ttyUSB0 flash      # via CH340C on hub port 2
```

## Pinout (ESP32-S3-WROOM-2)

| Signal | GPIO | Notes |
|---|---|---|
| SPI MOSI | GPIO5 | shared, R15 100Ω série |
| SPI MISO | GPIO6 | shared, R16 100Ω série |
| SPI SCK | GPIO7 | shared, R17 100Ω série |
| NRF#1 (half_L) CSN / CE / IRQ | GPIO13 / GPIO14 / GPIO8 | |
| NRF#2 (half_R) CSN / CE / IRQ | GPIO1 / GPIO4 / GPIO2 | |
| USB D+/D- | GPIO20/GPIO19 | native OTG full-speed |
| Bootstrap IO0 | GPIO0 | flash mode trigger via CH340 RTS |

## Status

**Plan 1 (bring-up)** : board variant créé, CDC USB live, pas encore de RF / engine actif.

Voir `docs/superpowers/plans/2026-05-11-dongle-plan-*.md` pour les phases suivantes.
```

Save as `boards/kase_dongle/README.md`.

---

## Task 14: Commit Plan 1

**Files:** all created/modified above.

- [ ] **Step 1: Review diff**

```bash
git status
git diff --stat
```

Expected files :
- New: `boards/kase_dongle/board.h`, `board_keymap.c`, `board_layout.c`, `board_features.h`, `README.md`
- New: `main/Kconfig.projbuild`, `sdkconfig.defaults.dongle`
- Modified: `CMakeLists.txt`, `main/CMakeLists.txt`, `main/main.c`

- [ ] **Step 2: Stage and commit**

```bash
git add boards/kase_dongle/ main/Kconfig.projbuild sdkconfig.defaults.dongle \
        CMakeLists.txt main/CMakeLists.txt main/main.c

git commit -m "feat(dongle): plan 1 — kase_dongle board variant bring-up

Adds boards/kase_dongle/ with board.h (NRF + SPI pinout extracted from
dongle.kicad_sch netlist), default 70-key keymap, layout names, capability
flags. Introduces Kconfig device role choice (KEYBOARD vs DONGLE) with
capability flags (HAS_LOCAL_MATRIX, HAS_DISPLAY, HAS_BLE, HAS_TAMA,
HAS_RF_RX, HAS_ESPNOW) gating CMake compilation.

main.c wraps display/matrix/BLE/Tama init in #if !DONGLE blocks; dongle
role builds with USB + CDC + engine + keymap NVS only. RF stack and
ESP-NOW left as TODO comments for Plans 2 and 5.

Per-board sdkconfig.defaults.<short> mechanism added in root CMakeLists
(picks up sdkconfig.defaults.dongle automatically when BOARD=kase_dongle).

V1/V2/V2D builds verified bit-identical (modulo timestamps) to pre-branch
main."
```

- [ ] **Step 3: Verify commit**

```bash
git log --oneline -3
```

Expected: top commit is "feat(dongle): plan 1 — kase_dongle board variant bring-up".

---

## Self-Review Checklist (run mentally after writing this plan)

- ✅ Spec coverage : Sections 1, 2, 6, 10 of the design spec are addressed (board variant + Kconfig + sdkconfig defaults + main.c refactor + V1/V2/V2D non-regression).
- ✅ No placeholders : all GPIO numbers concrete, all sdkconfig keys named, all source paths real.
- ✅ Type consistency : `MATRIX_ROWS=5`, `MATRIX_COLS=14`, `MAX_MATRIX_KEYS=70`, `COLS_PER_HALF=7` consistent across `board.h` and conceptual mentions.
- ✅ Each task ends with a commit step (Task 14 covers all together since Plan 1 is small enough to commit atomically; could be split per task if iterating).
- ⚠️ One TBD remains : keycode names in `board_keymap.c` Task 4 — explicitly flagged with the grep validation step. The agent must adapt to actual `key_definitions.h` symbol names.

## Out of scope for Plan 1

- NRF24 driver and rf_rx_task → **Plan 2**
- Engine input_source integration → **Plan 2**
- USB composite HID descriptor (mouse/consumer/system) → **Plan 3**
- CDC dongle commands (RF config, halves OTA) → **Plan 4**
- ESP-NOW cold path → **Plan 5**

After Plan 1 the dongle binary boots, exposes USB CDC, but does no key processing. The keymap is loaded into RAM but `MATRIX_STATE` stays zero forever (no scanner, no RF feeder yet).
