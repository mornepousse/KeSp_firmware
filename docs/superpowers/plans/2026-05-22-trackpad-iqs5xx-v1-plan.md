# Plan IQS5xx-v1 — Trackpad Driver: Movement + Tap-Click + 2-Finger Scroll

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fill the single TODO stub in `main/periph/trackpad/trackpad.c` with a real IQS5xx-B000
register read, implement the `trackpad_map()` pure function (gesture-to-HID mapping) with
TDD-first host tests, and replace the dropped-packet stub in `rf_rx_task.c` with a one-line
`hid_send_mouse` call. End state: half_left detects finger movement, tap-clicks, and 2-finger
vertical scroll; the dongle forwards all three to the USB HID mouse report. All three boards
(half_left, half_right, dongle) build green; host tests stay at 0 failed.

**Architecture:** Two fill-in points, both within existing infrastructure.

```
[HALF-LEFT firmware]

  RDY GPIO14 (active-low)
       │ NEGEDGE ISR  ← already real (trackpad_init)
       ▼
  s_rdy_sem (binary semaphore)
       │ xSemaphoreTake
       ▼
  trackpad_task (prio 6, stack 3072, core 0)
       │
       ├── [T2 FILL] i2c_master_transmit_receive:
       │     write {0x00,0x0F} → read data[9]
       │     → GestureEvents0/1, SystemInfo0/1, NFingers, RelX[2], RelY[2]
       │
       ├── [T2 FILL] ShowReset ACK once (SystemInfo0 bit7):
       │     write {0x04,0x31,0x02} to System Control 0
       │
       ├── [T2 FILL] trackpad_map(ge0, ge1, n_fingers, rel_x, rel_y,
       │                         &s_pending_release, &tp)
       │     → dx, dy, buttons, scroll_v, should_send
       │
       ├── [T2 FILL] activity gate: skip rf send if !should_send
       │
       ├── [T2 FILL] End-Window write: i2c_master_transmit {0xEE,0xEE,0x00}
       │              (behind IQS5XX_EXPLICIT_END_WINDOW, default=1)
       │
       ├── rf_encode_trackpad(buf, &tp)      ← already real
       ├── half_spi_lock()                   ← already real
       ├── rf_driver_send(&s_radio, buf, 7)  ← already real
       └── half_spi_unlock()                 ← already real


[DONGLE firmware]

  rf_rx_task → drain_radio → PKT_TYPE_TRACKPAD
       │
       ├── rf_decode_trackpad(buf, n, &tp)   ← already real
       │
       └── [T3 FILL] hid_send_mouse(tp.buttons, tp.dx, tp.dy, tp.scroll_v)
```

**Tech Stack:** ESP-IDF 5.5, `esp_driver_i2c` (I2C master v2 API — `i2c_master_transmit_receive`,
`i2c_master_transmit`), FreeRTOS, existing `rf_packet.h` and `hid_transport.h`.

**Spec reference:** `docs/superpowers/specs/2026-05-22-trackpad-iqs5xx-v1-design.md` — read in
full before touching any file. This plan is an executor's companion, not a substitute.

**Depends on:** Plan Bricks-1 (trackpad skeleton real and build-green: `trackpad_init()` real,
`trackpad_task()` TODO stub, `half_spi_lock/unlock` in place, `PKT_TYPE_TRACKPAD` stub on dongle).
All surrounding infrastructure is already real and hardware-validated.

**Build command (always `rm -f sdkconfig` when switching boards; run from repo root):**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_<target> -DBOARD=kase_<target> -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:|FAILED" | head -5
```

**Build targets:**
- `idf.py -B build_half_left  -DBOARD=kase_half_left  -DIDF_TARGET=esp32s3 build`
- `idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build`
- `idf.py -B build_dongle     -DBOARD=kase_dongle     -DIDF_TARGET=esp32s3 build`

**Host test command (baseline: 856 pass, 0 failed — must stay at 0 failed, grow in count):**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner
```

**Hardware (from `boards/kase_half_left/board.h`):**

| Signal        | GPIO     | Notes                                             |
|---------------|----------|---------------------------------------------------|
| SDA_TRACK     | GPIO40   | I2C data, 4.7 kΩ pull-up to 3.3V on PCB         |
| SCL_TRACK     | GPIO38   | I2C clock, 4.7 kΩ pull-up to 3.3V on PCB        |
| RST_TRACK     | GPIO13   | Reset active-low; 10 ms pulse at boot (already real) |
| RDY_TRACK     | GPIO14   | Data-ready IRQ, active-low, NEGEDGE (already real) |
| I2C address   | 0x74     | Default 7-bit; ADDR pin config — verify at bench |

---

## Baked-in facts (read before touching any file)

1. **`trackpad_task()` TODO stub sends zero packets unconditionally.** This plan replaces the
   stub body. The `trackpad_init()` function (I2C init, RST pulse, RDY ISR, I2C probe) is
   already real and must not be touched.

2. **`s_i2c_device` is already created in `trackpad_init()`.** Use it directly in
   `trackpad_task()` — no bus/device re-creation needed.

3. **`trackpad_map()` must be a pure function with no ESP-IDF types.** It takes primitive C
   types only (`uint8_t`, `int16_t`, `bool*`, pointer to `rf_trackpad_t`). The `rf_trackpad_t`
   struct from `rf_packet.h` contains only standard C types — including it in a file compiled
   with `-DTEST_HOST` is safe as long as `rf_packet.h` has no ESP-IDF includes (it does not —
   it only includes `<stdint.h>`, `<stdbool.h>`, `<string.h>`).

4. **`trackpad_map()` lives in `trackpad.c` and is declared in `trackpad.h`.** Do not create
   a separate `trackpad_mapping.c` file — the spec's "extract to trackpad_mapping.c" is a
   suggestion; the simpler approach is to guard the ESP-IDF-dependent code in `trackpad.c`
   with `#ifndef TEST_HOST` and expose `trackpad_map` + `clamp8` outside that guard.

5. **Host test compilation: `trackpad.c` must be compilable with `-DTEST_HOST`.** Guard all
   includes (`driver/gpio.h`, `driver/i2c_master.h`, `esp_log.h`, `freertos/...`, `board.h`,
   `half_spi.h`, `rf_driver.h`) and all functions except `trackpad_map` and `clamp8` with
   `#ifndef TEST_HOST`. `rf_packet.h` and `<string.h>` / `<stdbool.h>` remain unguarded
   (they are host-safe). The test file includes `trackpad.h` and `rf_packet.h` directly.

6. **`i2c_master_transmit_receive(dev, wbuf, wlen, rbuf, rlen, timeout_ms)` — one call per
   RDY event.** Write phase: 2-byte big-endian register address `{0x00, 0x0F}`. Read phase:
   9 bytes contiguous from GestureEvents0 through RelY LSB. Timeout: 50 ms.

7. **End-Window write is a separate `i2c_master_transmit` call.** The write buffer is
   `{0xEE, 0xEE, 0x00}` (3 bytes: reg addr MSB, reg addr LSB, dummy value). Guarded by
   `#if IQS5XX_EXPLICIT_END_WINDOW`. Best-effort — ignore return value.

8. **ShowReset ACK: write once, skip the data from that event.** Static `s_need_reset_ack`
   flag initialized to `true`. On the event where `sysinfo0 & 0x80` is set, write
   `{0x04, 0x31, 0x02}` via `i2c_master_transmit`, clear the flag, then `continue` (skip
   the rest of the loop body — RelX/RelY are garbage post-reset).

9. **`s_pending_release` is a `static bool` inside `trackpad_task`.** It persists across
   iterations of the for-loop. `trackpad_map()` receives a pointer to it and may set it to
   `true` (tap detected) or `false` (release emitted). The caller passes the current value
   in and reads back the new value through the same pointer.

10. **Activity gate: `trackpad_map()` returns `bool should_send`.** The caller does the RF
    send only when `should_send == true`. This keeps I2C reads and gesture decode
    unconditional; only the RF send is gated.

11. **dongle: `hid_send_mouse` is in `hid_transport.h` (already `#include`-able).** The
    function already exists in the always-compiled `comm/hid_transport.c` (line 13 of
    `main/CMakeLists.txt`). No CMakeLists or descriptor change needed. Add
    `#include "hid_transport.h"` to `rf_rx_task.c` if not already present — verify first.

12. **dongle: do NOT set `changed = true` for trackpad packets.** The `changed` flag drives
    `run_engine_cycle()` (keyboard HID report). Mouse reports are dispatched directly via
    `hid_send_mouse` and are independent.

13. **kase_v2 keyboard has a pre-existing BT Kconfig failure.** It is unrelated to this plan.
    Do not attempt to fix it; verify only that it does not worsen. Only half_left, half_right,
    and dongle are build targets for this plan.

14. **IDE clangd errors about missing esp-idf headers are false positives.** Trust `idf.py build`
    as ground truth. Do not modify include paths to satisfy clangd.

---

## Register / Constant Reference Table

All constants must be defined at the top of `trackpad.c` (inside `#ifndef TEST_HOST` for
ESP-IDF-dependent constants; outside the guard for the tunable ones used by `trackpad_map`):

```c
/* ── IQS5xx-B000 register addresses (16-bit, big-endian on wire) ── */
#define IQS5XX_REG_GESTURE_EVENTS_0   0x000F
#define IQS5XX_REG_GESTURE_EVENTS_1   0x0010
#define IQS5XX_REG_SYSTEM_INFO_0      0x0011
#define IQS5XX_REG_SYSTEM_INFO_1      0x0012
#define IQS5XX_REG_NUMBER_OF_FINGERS  0x0013
#define IQS5XX_REG_RELATIVE_X         0x0014   /* int16, big-endian */
#define IQS5XX_REG_RELATIVE_Y         0x0016   /* int16, big-endian */
#define IQS5XX_REG_SYSTEM_CONTROL_0   0x0431   /* ACK_RESET bit — VERIFY at bench */
#define IQS5XX_REG_END_WINDOW         0xEEEE   /* End Communication Window */

/* ── Gesture event bitmasks — VERIFY against IQS5xx-B000 datasheet ── */
#define IQS5XX_GEST0_SINGLE_TAP       0x01     /* GestureEvents0 bit 0 */
#define IQS5XX_GEST1_SCROLL           0x02     /* GestureEvents1 bit 1 */

/* ── SystemInfo0 bitmasks ── */
#define IQS5XX_SYSINFO0_SHOW_RESET    0x80     /* bit 7 — set after reset */

/* ── System Control 0 bitmasks ── */
#define IQS5XX_SYSCTRL0_ACK_RESET     0x02     /* bit 1 — VERIFY address at bench */

/* ── Read block ── */
#define IQS5XX_READ_START_ADDR        0x000F   /* GestureEvents0 */
#define IQS5XX_READ_BYTE_COUNT        9        /* through RelY LSB at 0x0017 */

/* ── Tunable constants (outside TEST_HOST guard — used by trackpad_map) ── */
#define IQS5XX_SCROLL_DIV             8        /* divide RelY for scroll speed; tune at bench */
#define IQS5XX_EXPLICIT_END_WINDOW    1        /* 1 = write 0xEEEE after read */
#define IQS5XX_INVERT_X               0        /* 1 = negate dx — verify at bench */
#define IQS5XX_INVERT_Y               0        /* 1 = negate dy — verify at bench */
```

Read block byte layout (9 bytes, offset from `data[0]`):

```
data[0]  GestureEvents0   0x000F  — tap, scroll bits
data[1]  GestureEvents1   0x0010  — scroll, flick bits
data[2]  SystemInfo0      0x0011  — ShowReset = bit7
data[3]  SystemInfo1      0x0012  — (unused in v1)
data[4]  NumberOfFingers  0x0013  — touch count 0..5
data[5]  RelativeX MSB    0x0014
data[6]  RelativeX LSB    0x0015
data[7]  RelativeY MSB    0x0016
data[8]  RelativeY LSB    0x0017
```

---

## File Structure

### Modified

| Path | Change |
|------|--------|
| `main/periph/trackpad/trackpad.c` | Replace TODO stub in `trackpad_task`; add constants; add `trackpad_map` + `clamp8` pure functions; `#ifndef TEST_HOST` guards around ESP-IDF deps |
| `main/periph/trackpad/trackpad.h` | Declare `trackpad_map()` + `clamp8()` (host-safe signature) |
| `main/comm/rf/rf_rx_task.c` | Replace PKT_TYPE_TRACKPAD stub with `hid_send_mouse` one-liner; add `#include "hid_transport.h"` if absent |
| `test/test_trackpad_map.c` | New: host tests for `trackpad_map()` — 10 cases from spec §6.1 |
| `test/CMakeLists.txt` | Add `test_trackpad_map.c` + `../main/periph/trackpad/trackpad.c` to `add_executable` |
| `test/test_main.c` | Add `extern void test_trackpad_map(void)` + call |

### Untouched

```
main/periph/trackpad/trackpad.h      trackpad_init / trackpad_start — no change
main/comm/rf/rf_packet.h             rf_trackpad_t, rf_encode/decode — already real
main/comm/hid_transport.h/c          hid_send_mouse already exists + compiled on dongle
boards/kase_half_left/board.h        BOARD_TRACK_* already defined in Plan Bricks-1
main/CMakeLists.txt                  no new sources (trackpad.c already gated on TRACKPAD;
                                     hid_transport.c already always-compiled)
main/Kconfig.projbuild               no changes
sdkconfig                            rm before each board switch, never commit
```

---

## Task 1: `trackpad_map()` pure function + host tests (TDD)

**Rationale:** The gesture-to-HID mapping is the only logic-heavy piece of this plan. Write
and pass the host tests first, before touching any I2C code. A green test suite is a
prerequisite for Task 2.

**Files:**
- Modify: `main/periph/trackpad/trackpad.h`
- Modify: `main/periph/trackpad/trackpad.c`
- Create: `test/test_trackpad_map.c`
- Modify: `test/CMakeLists.txt`
- Modify: `test/test_main.c`

### Step 1.1 — Read existing `trackpad.h` and `trackpad.c`

```bash
cat /home/mae/Documents/GitHub/KaSe_firmware/main/periph/trackpad/trackpad.h
cat /home/mae/Documents/GitHub/KaSe_firmware/main/periph/trackpad/trackpad.c
```

Verify:
- `trackpad_init()` and `trackpad_start()` are declared in `trackpad.h`.
- `trackpad_task()` has the `/* TODO STUB */` block.
- `s_i2c_device` is already defined as a file-static.
- `extern rf_radio_t s_radio;` is present.

### Step 1.2 — Add `#ifndef TEST_HOST` guards to `trackpad.c`

Wrap all ESP-IDF-dependent includes and all functions except `clamp8` and `trackpad_map`
in `#ifndef TEST_HOST` / `#endif` guards.

The file structure after editing:

```c
/* ── Host-safe includes (no ESP-IDF) ── */
#include "trackpad.h"
#include "rf_packet.h"          /* rf_trackpad_t — host-safe (stdint only) */
#include <stdbool.h>
#include <stdint.h>

/* ── Tunable constants (outside TEST_HOST — used by trackpad_map) ── */
#define IQS5XX_GEST0_SINGLE_TAP       0x01
#define IQS5XX_GEST1_SCROLL           0x02
#define IQS5XX_SCROLL_DIV             8
#define IQS5XX_INVERT_X               0
#define IQS5XX_INVERT_Y               0
/* ... all other #define constants ... */

/* ── clamp8: pure, host-safe ── */
static inline int8_t clamp8(int16_t v)
{
    if (v >  127) return  127;
    if (v < -127) return -127;
    return (int8_t)v;
}

/* ── trackpad_map: pure, host-safe ── */
bool trackpad_map(uint8_t ge0, uint8_t ge1, uint8_t n_fingers,
                  int16_t rel_x, int16_t rel_y,
                  bool *pending_release_io, rf_trackpad_t *out)
{
    /* ... implementation ... */
}

#ifndef TEST_HOST
/* ── ESP-IDF-dependent includes ── */
#include "board.h"
#include "half_spi.h"
#include "rf_driver.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

/* ... all static variables, rdy_isr_handler, trackpad_init, trackpad_task,
       trackpad_start ... */

#endif /* TEST_HOST */
```

**Important:** `#include "rf_packet.h"` must remain OUTSIDE the `#ifndef TEST_HOST` guard,
because `trackpad_map`'s signature uses `rf_trackpad_t`. Verify that `rf_packet.h` only
includes `<stdint.h>`, `<stdbool.h>`, `<string.h>` — it does (confirmed from source).

### Step 1.3 — Add `trackpad_map()` declaration to `trackpad.h`

Add after the existing `trackpad_start()` declaration:

```c
/* ── Pure gesture-to-HID mapping function — host-testable ──────────
 *
 * Maps raw IQS5xx fields to rf_trackpad_t output fields.
 * No I/O, no FreeRTOS calls, no global state reads.
 *
 * Parameters:
 *   ge0              GestureEvents0 byte (data[0] from 9-byte read block)
 *   ge1              GestureEvents1 byte (data[1])
 *   n_fingers        NumberOfFingers byte (data[4]) — unused in v1 logic
 *   rel_x            RelativeX signed 16-bit (data[5..6] big-endian decoded)
 *   rel_y            RelativeY signed 16-bit (data[7..8] big-endian decoded)
 *   pending_release_io  In/out: current state of the tap-release state machine.
 *                       Set to true by the function when a tap press is emitted.
 *                       Cleared to false when the release packet is emitted.
 *                       The caller (trackpad_task) holds this as a static bool.
 *   out              Output: filled with dx, dy, buttons, scroll_v, scroll_h, seq.
 *                    seq is NOT set here — caller sets out->seq = s_seq++ after return.
 *                    scroll_h is always set to 0 (out of v1 scope).
 *
 * Returns true if a packet should be sent (activity gate passed).
 * Returns false if all output fields are zero and no button event — caller drops.
 *
 * Precedence: scroll gesture overrides cursor movement.
 * tap detection: buttons=0x01 on tap event; pending_release_io set to true.
 * release: buttons=0x00 on next call when pending_release_io is true; force-send.
 */
bool trackpad_map(uint8_t ge0, uint8_t ge1, uint8_t n_fingers,
                  int16_t rel_x, int16_t rel_y,
                  bool *pending_release_io, rf_trackpad_t *out);
```

Also add `#include "rf_packet.h"` to `trackpad.h` (needed for `rf_trackpad_t` in the
`trackpad_map` signature):

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rf_packet.h"    /* rf_trackpad_t */
```

### Step 1.4 — Implement `trackpad_map()` in `trackpad.c`

Full implementation (place immediately after the constant definitions, before
`#ifndef TEST_HOST`):

```c
bool trackpad_map(uint8_t ge0, uint8_t ge1, uint8_t n_fingers,
                  int16_t rel_x, int16_t rel_y,
                  bool *pending_release_io, rf_trackpad_t *out)
{
    (void)n_fingers;   /* unused in v1 — gesture bits carry scroll intent */

    /* Zero the output; caller sets seq after return */
    out->dx       = 0;
    out->dy       = 0;
    out->buttons  = 0;
    out->scroll_v = 0;
    out->scroll_h = 0;   /* always 0 — horizontal scroll out of v1 scope */

    bool force_send = false;

    /* ── Axis inversion (compile-time flags; default off) ── */
    if (IQS5XX_INVERT_X) rel_x = -rel_x;
    if (IQS5XX_INVERT_Y) rel_y = -rel_y;

    /* ── Scroll vs cursor (scroll gesture takes priority) ── */
    bool scroll_active = (ge1 & IQS5XX_GEST1_SCROLL) != 0;
    if (scroll_active) {
        /* 2-finger vertical scroll: divide RelY, clamp to [-127, +127] */
        out->scroll_v = clamp8((int16_t)(rel_y / IQS5XX_SCROLL_DIV));
        /* dx, dy remain 0 — scroll gesture suppresses cursor movement */
    } else {
        /* Cursor movement: clamp RelX/RelY to HID int8_t range */
        out->dx = clamp8(rel_x);
        out->dy = clamp8(rel_y);
    }

    /* ── Click state machine (single-tap pulse synthesis) ── */
    bool tap_detected = (ge0 & IQS5XX_GEST0_SINGLE_TAP) != 0;

    if (*pending_release_io) {
        /* Previous iteration emitted press — emit release now */
        out->buttons          = 0x00;
        *pending_release_io   = false;
        force_send            = true;   /* release always sends (even if no movement) */
    } else if (tap_detected) {
        /* Tap recognized this event — emit press */
        out->buttons          = 0x01;  /* left button */
        *pending_release_io   = true;  /* arm release for next iteration */
        force_send            = true;  /* press always sends */
    }
    /* else: buttons stays 0x00 */

    /* ── Activity gate ── */
    bool send = force_send
             || (out->dx       != 0)
             || (out->dy       != 0)
             || (out->scroll_v != 0)
             || (out->buttons  != 0x00);

    return send;
}
```

### Step 1.5 — Write `test/test_trackpad_map.c`

Write the test BEFORE verifying the implementation compiles (TDD discipline).

```c
/*
 * test_trackpad_map.c — Host tests for trackpad_map() pure function.
 *
 * Contract under test (spec §6.1, §4.3, §4.4, §4.5):
 *   - Scroll gesture (ge1 & GEST1_SCROLL) → scroll_v = clamp(rel_y / SCROLL_DIV)
 *     dx=0, dy=0. Scroll beats cursor.
 *   - Cursor (no scroll) → dx=clamp(rel_x), dy=clamp(rel_y), scroll_v=0.
 *   - Single tap (ge0 & GEST0_SINGLE_TAP) → buttons=0x01, pending_out=true, send=true.
 *   - Pending release → buttons=0x00, pending_out=false, send=true (force).
 *   - Activity gate: all-zero output → send=false.
 *   - Scroll-zero edge case: rel_y < SCROLL_DIV → scroll_v=0 → send=false.
 *
 * Run: cd test/build && cmake .. && make && ./test_runner
 */
#include "test_framework.h"
#include "../main/periph/trackpad/trackpad.h"

/* Helpers */
static rf_trackpad_t g_out;
static bool          g_pending;

static bool call_map(uint8_t ge0, uint8_t ge1, int16_t rx, int16_t ry)
{
    return trackpad_map(ge0, ge1, 0, rx, ry, &g_pending, &g_out);
}

void test_trackpad_map(void)
{
    TEST_SUITE("trackpad_map");

    /* ── Case 1: No touch — all zero, no send ─────────────────── */
    g_pending = false;
    bool sent = call_map(0x00, 0x00, 0, 0);
    TEST_ASSERT(!sent,             "no-touch: should_send must be false");
    TEST_ASSERT_EQ(g_out.dx,       0, "no-touch: dx must be 0");
    TEST_ASSERT_EQ(g_out.dy,       0, "no-touch: dy must be 0");
    TEST_ASSERT_EQ(g_out.buttons,  0, "no-touch: buttons must be 0");
    TEST_ASSERT_EQ(g_out.scroll_v, 0, "no-touch: scroll_v must be 0");

    /* ── Case 2: Movement — rel_x=50, rel_y=-20, no gesture ────── */
    g_pending = false;
    sent = call_map(0x00, 0x00, 50, -20);
    TEST_ASSERT(sent,               "movement: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx,  50,  "movement: dx must be 50");
    TEST_ASSERT_EQ(g_out.dy, -20,  "movement: dy must be -20");
    TEST_ASSERT_EQ(g_out.scroll_v, 0, "movement: scroll_v must be 0");
    TEST_ASSERT_EQ(g_out.buttons,  0, "movement: buttons must be 0");

    /* ── Case 3: Clamp positive — rel_x=200 → dx=127 ────────────── */
    g_pending = false;
    sent = call_map(0x00, 0x00, 200, 0);
    TEST_ASSERT(sent,               "clamp-pos: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx, 127,  "clamp-pos: dx must be 127");

    /* ── Case 4: Clamp negative — rel_x=-200 → dx=-127 ─────────── */
    g_pending = false;
    sent = call_map(0x00, 0x00, -200, 0);
    TEST_ASSERT(sent,               "clamp-neg: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx, -127, "clamp-neg: dx must be -127");

    /* ── Case 5: Single tap press ────────────────────────────────── */
    g_pending = false;
    sent = call_map(0x01, 0x00, 0, 0);   /* ge0 = GEST0_SINGLE_TAP */
    TEST_ASSERT(sent,                    "tap-press: should_send must be true");
    TEST_ASSERT_EQ(g_out.buttons, 0x01, "tap-press: buttons must be 0x01");
    TEST_ASSERT(g_pending,              "tap-press: pending must be true after tap");

    /* ── Case 6: Single tap release (pending=true, no gesture) ───── */
    /* g_pending is still true from Case 5 */
    sent = call_map(0x00, 0x00, 0, 0);
    TEST_ASSERT(sent,                    "tap-release: should_send must be true (forced)");
    TEST_ASSERT_EQ(g_out.buttons, 0x00, "tap-release: buttons must be 0x00");
    TEST_ASSERT(!g_pending,             "tap-release: pending must be false after release");

    /* ── Case 7: Scroll active — rel_x=30, rel_y=64 → scroll_v=8 ── */
    g_pending = false;
    sent = call_map(0x00, 0x02, 30, 64);   /* ge1 = GEST1_SCROLL */
    TEST_ASSERT(sent,                      "scroll: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx,        0,    "scroll: dx must be 0 (suppressed)");
    TEST_ASSERT_EQ(g_out.dy,        0,    "scroll: dy must be 0 (suppressed)");
    TEST_ASSERT_EQ(g_out.scroll_v,  8,    "scroll: scroll_v must be 64/8=8");

    /* ── Case 8: Scroll clamp — rel_y=1200 → scroll_v=127 ────────── */
    g_pending = false;
    sent = call_map(0x00, 0x02, 0, 1200);
    TEST_ASSERT(sent,                      "scroll-clamp: should_send must be true");
    TEST_ASSERT_EQ(g_out.scroll_v, 127,   "scroll-clamp: scroll_v must be 127");

    /* ── Case 9: Scroll zero activity gate ───────────────────────── */
    /* rel_y=3, SCROLL_DIV=8 → 3/8=0 → scroll_v=0 → send=false       */
    g_pending = false;
    sent = call_map(0x00, 0x02, 0, 3);
    TEST_ASSERT(!sent,                     "scroll-zero: should_send must be false (3/8=0)");
    TEST_ASSERT_EQ(g_out.scroll_v,  0,    "scroll-zero: scroll_v must be 0");
    TEST_ASSERT_EQ(g_out.dx,        0,    "scroll-zero: dx must be 0");

    /* ── Case 10: Scroll vs cursor mutual exclusion ───────────────── */
    /* Both scroll gesture and rel_x=50 → dx must be 0, scroll_v=8   */
    g_pending = false;
    sent = call_map(0x00, 0x02, 50, 64);
    TEST_ASSERT(sent,                      "scroll-mutex: should_send must be true");
    TEST_ASSERT_EQ(g_out.dx,        0,    "scroll-mutex: dx must be 0 (scroll wins)");
    TEST_ASSERT_EQ(g_out.dy,        0,    "scroll-mutex: dy must be 0 (scroll wins)");
    TEST_ASSERT_EQ(g_out.scroll_v,  8,    "scroll-mutex: scroll_v must be 8");
    TEST_ASSERT_EQ(g_out.buttons,   0,    "scroll-mutex: buttons must be 0");
}
```

### Step 1.6 — Add to `test/CMakeLists.txt`

In the `add_executable(test_runner ...)` source list, add:

```cmake
    test_trackpad_map.c
    ../main/periph/trackpad/trackpad.c
```

In `target_include_directories`, add:

```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/periph/trackpad
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/rf
```

(The `main/comm/rf` path is needed so `trackpad.c` can include `rf_packet.h` via the
existing relative path. Verify the include path resolves before building.)

### Step 1.7 — Add to `test/test_main.c`

```c
/* Add declaration at top with other externs: */
extern void test_trackpad_map(void);

/* Add call after test_eink_pack(): */
test_trackpad_map();
```

### Step 1.8 — Run host tests — must pass before Task 2

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -15
./test_runner 2>&1 | grep -E "trackpad_map|FAIL|passed|failed"
```

Expected: all 10 `test_trackpad_map` cases pass. Previous 856 tests also pass.

Common failure modes:
- `rf_packet.h not found` → add `${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/rf` to
  `target_include_directories` in `test/CMakeLists.txt`.
- `undefined reference to trackpad_map` → verify `trackpad.c` is in the CMake source list
  and that `trackpad_map` is defined OUTSIDE the `#ifndef TEST_HOST` guard.
- `implicit declaration of clamp8` → `clamp8` must be defined (or its `static inline`
  definition placed) before `trackpad_map` in the file and outside `#ifndef TEST_HOST`.

Do not proceed to Task 2 if any case fails. Fix `trackpad_map` (not the test).

### Step 1.9 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/trackpad/trackpad.h \
        main/periph/trackpad/trackpad.c \
        test/test_trackpad_map.c \
        test/CMakeLists.txt \
        test/test_main.c
git commit -m "feat(trackpad): trackpad_map() pure fn + host tests (TDD, 10 cases)"
```

---

## Task 2: IQS5xx register read in `trackpad_task` + build half_left

**Files:**
- Modify: `main/periph/trackpad/trackpad.c`

Replace the `/* TODO STUB */` block in `trackpad_task` with the full IQS5xx window-transfer
read sequence, ShowReset ACK, `trackpad_map` call, activity-gated RF send, and End-Window
write.

### Step 2.1 — Read current `trackpad_task` stub

```bash
grep -n "TODO STUB\|TODO\|Stub\|s_seq\|rf_encode\|half_spi" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/periph/trackpad/trackpad.c
```

Confirm the stub block spans from `xSemaphoreTake` to `half_spi_unlock()`. Note exact line
numbers to replace.

### Step 2.2 — Add `s_need_reset_ack` and `s_pending_release` static variables

Inside `#ifndef TEST_HOST`, add alongside the existing static variables:

```c
/* ShowReset ACK state — cleared after first successful ACK write */
static bool s_need_reset_ack = true;

/* Click state machine — true when waiting to emit button-release packet */
static bool s_pending_release = false;
```

### Step 2.3 — Replace the `trackpad_task` stub body

Replace everything between `xSemaphoreTake(s_rdy_sem, portMAX_DELAY);` and the closing
`}` of the `for (;;)` loop with:

```c
        /* Wait for RDY pin to go low (data-ready from IQS5xx) */
        xSemaphoreTake(s_rdy_sem, portMAX_DELAY);

        /* ── Step 1: Read 9-byte block from GestureEvents0 (0x000F) ── */
        /* IQS5xx window-transfer: write 2-byte reg addr, read 9 bytes.
         * Must complete while RDY is asserted. Timeout 50 ms. */
        uint8_t reg_addr[2] = {
            (IQS5XX_READ_START_ADDR >> 8) & 0xFF,   /* 0x00 */
            (IQS5XX_READ_START_ADDR)      & 0xFF,   /* 0x0F */
        };
        uint8_t data[IQS5XX_READ_BYTE_COUNT];
        esp_err_t err = i2c_master_transmit_receive(
            s_i2c_device,
            reg_addr, sizeof(reg_addr),
            data, sizeof(data),
            50   /* timeout ms */
        );
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2C read failed: %d — skip event", err);
            /* Do not end window — bus is already free after failed transaction */
            continue;
        }

        /* ── Step 2: End Communication Window (best-effort) ─────────── */
        /* Some IQS5xx revisions require an explicit 0xEEEE write to close
         * the window. Others close on the I2C STOP from step 1.
         * IQS5XX_EXPLICIT_END_WINDOW=1 by default — disable at bench if
         * bus hangs (see bench checklist item 2). */
#if IQS5XX_EXPLICIT_END_WINDOW
        {
            uint8_t end_cmd[3] = {0xEE, 0xEE, 0x00};
            /* Best-effort: chip may already have closed the window — ignore error */
            i2c_master_transmit(s_i2c_device, end_cmd, sizeof(end_cmd), 10);
        }
#endif

        /* ── Step 3: Extract fields from read block ─────────────────── */
        uint8_t  ge0       = data[0];   /* GestureEvents0 */
        uint8_t  ge1       = data[1];   /* GestureEvents1 */
        uint8_t  sysinfo0  = data[2];   /* SystemInfo0 — ShowReset = bit7 */
        uint8_t  n_fingers = data[4];   /* NumberOfFingers */
        int16_t  rel_x     = (int16_t)((data[5] << 8) | data[6]);
        int16_t  rel_y     = (int16_t)((data[7] << 8) | data[8]);

        /* ── Step 4: ShowReset ACK (once after hardware reset) ───────── */
        /* On first read after RST pulse, ShowReset (sysinfo0 bit7) is set.
         * Acknowledge by writing ACK_RESET bit to System Control 0 (0x0431).
         * Skip the data from this event — RelX/RelY are garbage post-reset.
         * Register address and bit position: VERIFY against IQS5xx-B000
         * datasheet (spec §7 item 6). Expected: addr=0x0431, bit1=0x02. */
        if (s_need_reset_ack) {
            if (sysinfo0 & IQS5XX_SYSINFO0_SHOW_RESET) {
                uint8_t ack_cmd[3] = {
                    (IQS5XX_REG_SYSTEM_CONTROL_0 >> 8) & 0xFF,   /* 0x04 */
                    (IQS5XX_REG_SYSTEM_CONTROL_0)      & 0xFF,   /* 0x31 */
                    IQS5XX_SYSCTRL0_ACK_RESET                     /* 0x02 */
                };
                esp_err_t ack_err = i2c_master_transmit(s_i2c_device,
                                                         ack_cmd, sizeof(ack_cmd),
                                                         20);
                if (ack_err == ESP_OK) {
                    ESP_LOGI(TAG, "ShowReset ACK sent — entering normal operation");
                } else {
                    ESP_LOGW(TAG, "ShowReset ACK failed: %d", ack_err);
                }
                s_need_reset_ack = false;
                continue;   /* skip this event's movement data */
            } else {
                /* ShowReset not set on first read — chip was already running */
                s_need_reset_ack = false;
                ESP_LOGI(TAG, "ShowReset not set on first read — normal operation");
            }
        }

        /* ── Step 5: Map gesture fields → HID output ────────────────── */
        rf_trackpad_t tp;
        bool should_send = trackpad_map(ge0, ge1, n_fingers,
                                        rel_x, rel_y,
                                        &s_pending_release, &tp);

        /* ── Step 6: Activity gate ───────────────────────────────────── */
        if (!should_send) {
            /* All fields zero, no button event — drop silently.
             * No log here: this is the common idle case (hot path). */
            continue;
        }

        /* ── Step 7: Encode and transmit ────────────────────────────── */
        tp.seq = s_seq++;
        uint8_t buf[7];
        rf_encode_trackpad(buf, &tp);
        half_spi_lock();
        rf_driver_send(&s_radio, buf, 7);
        half_spi_unlock();
```

### Step 2.4 — Build half_left

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:|FAILED" | head -5
```

Expected: `Project build complete.`

Common failure modes:
- `i2c_master_transmit not declared` → verify ESP-IDF v5.x `driver/i2c_master.h` is included
  inside `#ifndef TEST_HOST`. Check that `esp_driver_i2c` is in `priv_requires` in
  `main/CMakeLists.txt` (already added in Plan Bricks-1).
- `undefined reference to trackpad_map` → the function must be outside `#ifndef TEST_HOST`.
- `implicit declaration of clamp8` → ensure `clamp8` definition precedes `trackpad_map`.
- `esp_err_t` or `ESP_LOGW` undefined in host build → those are inside `#ifndef TEST_HOST`
  in `trackpad_task`; verify the guard placement is correct.

### Step 2.5 — Verify half_right also builds

```bash
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:|FAILED" | head -5
```

Expected: `Project build complete.` (half_right has `KASE_HAS_TRACKPAD=y` but no physical
trackpad — `trackpad_init()` returns false from I2C probe; task never starts. No impact.)

### Step 2.6 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/trackpad/trackpad.c
git commit -m "feat(trackpad): IQS5xx read + ShowReset ACK + gesture map + activity gate"
```

---

## Task 3: Dongle — `PKT_TRACKPAD` → `hid_send_mouse` one-liner

**Files:**
- Modify: `main/comm/rf/rf_rx_task.c`

Replace the dropped-packet stub in `drain_radio()` with a forwarding call to `hid_send_mouse`.

### Step 3.1 — Verify `hid_transport.h` is not already included

```bash
grep -n "hid_transport" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/rf_rx_task.c
```

If no output: add `#include "hid_transport.h"` at the top of the includes block.
If already present: proceed to Step 3.2.

### Step 3.2 — Check the stub block

```bash
grep -n "PKT_TYPE_TRACKPAD\|hid_send_mouse\|TODO STUB\|stub" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/rf_rx_task.c
```

The existing stub (from Plan Bricks-1):

```c
        } else if (type == PKT_TYPE_TRACKPAD) {
            rf_trackpad_t tp;
            if (rf_decode_trackpad(buf, n, &tp)) {
                /* TODO STUB: forward to mouse HID report. ... */
                (void)tp;
                ESP_LOGD(TAG, "PKT_TRACKPAD dx=%d dy=%d btn=0x%02x (stub — dropped)",
                         tp.dx, tp.dy, tp.buttons);
            }
        }
```

### Step 3.3 — Replace stub with `hid_send_mouse` call

Replace the entire `else if (type == PKT_TYPE_TRACKPAD)` block with:

```c
        } else if (type == PKT_TYPE_TRACKPAD) {
            rf_trackpad_t tp;
            if (rf_decode_trackpad(buf, n, &tp)) {
                /* Forward mouse data directly — bypasses keyboard engine cycle.
                 * hid_send_mouse signature: (buttons, x, y, wheel).
                 * scroll_h is always 0 (out of v1 scope; no horizontal wheel arg). */
                hid_send_mouse(tp.buttons, tp.dx, tp.dy, tp.scroll_v);
            }
        }
```

Note: do NOT set `changed = true` here. The `changed` flag drives `run_engine_cycle()`
(keyboard HID report). Mouse reports are independent of the keyboard report path.

Also add the include at the top of the include block if not already present:

```c
#include "hid_transport.h"
```

### Step 3.4 — Build dongle

```bash
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:|FAILED" | head -5
```

Expected: `Project build complete.`

Common failure modes:
- `undefined reference to hid_send_mouse` → verify `hid_transport.c` is in the always-
  compiled `srcs` list in `main/CMakeLists.txt` (line 13, confirmed in spec §2). If it is
  gated on a Kconfig flag, the dongle build must have that flag enabled.
- `hid_transport.h not found` → verify the include path for `comm/` is in `INCLUDE_DIRS`
  in `main/CMakeLists.txt`. It should already be present (the dongle uses `hid_transport`
  for keyboard reports too).

### Step 3.5 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/rf_rx_task.c
git commit -m "feat(dongle): PKT_TRACKPAD → hid_send_mouse (replace drop stub)"
```

---

## Task 4: Build-green all three boards + host tests green

Final verification pass. Run all builds from clean state (rm sdkconfig between each), then
run host tests.

### Step 4.1 — Build half_left

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:|FAILED" | head -5
```

Expected: `Project build complete.`

### Step 4.2 — Build half_right

```bash
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:|FAILED" | head -5
```

Expected: `Project build complete.`

### Step 4.3 — Build dongle

```bash
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:|FAILED" | head -5
```

Expected: `Project build complete.`

### Step 4.4 — Run host tests

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner
```

Expected: `0 failed`. Count must be >= 866 (856 baseline + 10 new trackpad_map cases).
If count is lower, a test was silently removed — investigate before proceeding.

### Step 4.5 — Commit if any files were adjusted during Task 4

If no files changed (everything already committed in T1–T3): no commit needed.
If build errors forced a fix: stage the fixed file and commit:

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add <fixed-file>
git commit -m "fix(trackpad): <short description of fix>"
```

---

## Task 5: Bench validation

Flash half_left and dongle. Validate all three features against the spec §7 verify-at-bench
checklist. Work through the items in the exact order below — item 1 (RDY polarity) is the
most likely blocker and must be resolved before any other item is meaningful.

### Step 5.1 — ITEM 1 (CRITICAL FIRST): RDY polarity

**Risk:** If RDY polarity is wrong, the ISR fires at the wrong edge. The trackpad either
never responds (no semaphore give) or fires continuously (semaphore flooded). This must
be resolved before any gesture testing is possible.

Procedure:
1. Flash half_left: `idf.py -B build_half_left -p /dev/ttyUSB0 flash monitor`
2. Without touching the trackpad, observe the log. If `trackpad_task started` appears but
   no I2C reads ever fire (no `I2C read failed` and no mouse cursor movement), the ISR is
   not firing.
3. Probe GPIO14 with an oscilloscope or logic analyser at startup. The IQS5xx asserts RDY
   low when it has data ready (active-low in default OTP config).
4. If the probe shows RDY goes HIGH on data-ready (chip is wired active-HIGH):
   - In `trackpad_init()`, change `GPIO_INTR_NEGEDGE` → `GPIO_INTR_POSEDGE`.
   - Rebuild and reflash.
5. If the probe shows RDY goes LOW on data-ready (as expected): keep `GPIO_INTR_NEGEDGE`.

Only proceed to Step 5.2 once the ISR fires (I2C reads are attempted and either succeed or
produce `I2C read failed` logs).

### Step 5.2 — ITEM 2: End-Window mechanism

**Risk:** If the IQS5xx on this specific board revision requires the 0xEEEE write to close
the window but `IQS5XX_EXPLICIT_END_WINDOW=0`, subsequent reads may time out or the bus
may lock.

Procedure:
1. Flash with `IQS5XX_EXPLICIT_END_WINDOW=1` (default). Touch the trackpad and drag a finger.
2. Observe: if cursor moves and no `I2C read failed` logs appear → window mechanism is working.
3. If `I2C read failed` appears on every second read (alternating) → the chip may be locking
   the bus because the 0xEEEE write conflicts with its window-close. Try
   `IQS5XX_EXPLICIT_END_WINDOW=0`, rebuild, reflash.
4. If reads time out consistently regardless of the flag → suspect I2C address or wiring
   (check Step 5.5).

### Step 5.3 — ITEM 5: Gesture bit positions (dump raw bytes)

**Risk:** GEST0 bit0 = SingleTap and GEST1 bit1 = Scroll are per AN171 §5. The exact
IQS5xx-B000 variant may shift these.

Procedure:
1. Add a temporary log inside the `if (!should_send)` branch (before `continue`):
   `ESP_LOGI(TAG, "raw ge0=0x%02x ge1=0x%02x nf=%d rx=%d ry=%d", ge0, ge1, n_fingers, (int)rel_x, (int)rel_y);`
2. Tap the trackpad with one finger. Observe ge0: bit 0 should toggle (0x01).
3. Drag two fingers vertically. Observe ge1: bit 1 should toggle (0x02).
4. If different bits toggle, update `IQS5XX_GEST0_SINGLE_TAP` and `IQS5XX_GEST1_SCROLL`
   to match the observed bit positions.
5. Remove the temporary log before committing.

### Step 5.4 — ITEM 6: ShowReset / ACK_RESET register

**Risk:** Register 0x0431 bit1 is per AN171. If the IQS5xx-B000 variant uses a different
address, ShowReset stays set and the chip ignores subsequent commands.

Procedure:
1. On first boot, observe the log. `ShowReset ACK sent` must appear exactly once.
2. If `ShowReset not set on first read` appears instead: the chip initialized before the
   I2C probe settled — this is acceptable; normal operation proceeds.
3. If `ShowReset ACK sent` appears but the chip continues to assert ShowReset on every
   subsequent read (logs on every iteration): the ACK write went to the wrong register.
   Consult the IQS5xx-B000 datasheet section "System Control Registers" for the exact
   address and update `IQS5XX_REG_SYSTEM_CONTROL_0` and `IQS5XX_SYSCTRL0_ACK_RESET`.

### Step 5.5 — ITEM 3: Axis sign and orientation

**Risk:** The TPS43-201A-S module mounting direction determines whether RelX/RelY match
expected cursor directions.

Procedure:
1. Move finger to the right. Cursor should move right. If it moves left:
   set `IQS5XX_INVERT_X 1` in `trackpad.c`, rebuild, reflash.
2. Move finger downward. Cursor should move down. If it moves up:
   set `IQS5XX_INVERT_Y 1`, rebuild, reflash.
3. If axes are swapped (right-drag moves cursor vertically): swap the `rel_x`/`rel_y`
   variables in `trackpad_task` after extraction (before calling `trackpad_map`).

### Step 5.6 — ITEM 4: SCROLL_DIV tuning

**Risk:** Default `SCROLL_DIV=8` may be too slow or too fast for the specific trackpad
surface and finger speed.

Procedure:
1. Place two fingers and drag vertically in a browser or text editor. One screenful of
   scroll per ~3 cm of finger movement is a reasonable baseline.
2. If scroll is too slow (small finger movement → little scroll): decrease `SCROLL_DIV`
   (try 4 or 6).
3. If scroll is too fast: increase `SCROLL_DIV` (try 12 or 16).
4. Rebuild and reflash after each change. Commit the final value.

### Step 5.7 — Full feature validation

Run each validation item and mark it done:

- [ ] Cursor moves with finger drag (one finger, no gesture bits set). Confirm via `xev`
  or mouse cursor on screen.
- [ ] Tap once → single left-click registers. Confirm via `xev` (ButtonPress + ButtonRelease
  pair within ~50 ms).
- [ ] Two-finger vertical drag → scroll events in browser or text editor.
- [ ] Lift finger mid-drag → cursor stops (no phantom packets from activity gate).
- [ ] Idle (no touch) → no NRF packets transmitted. Confirm by monitoring NRF packet rate
  counter in the half heartbeat or by observing the dongle log at `ESP_LOGD` level.

### Step 5.8 — Commit tuned constants (if changed)

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/trackpad/trackpad.c
git commit -m "fix(trackpad): bench tuning — axis inversion / SCROLL_DIV / RDY edge / gesture bits"
```

---

## Self-review checklist

- [ ] `trackpad_map()` is outside `#ifndef TEST_HOST` — host tests can link it.
- [ ] `clamp8()` is outside `#ifndef TEST_HOST` and defined before `trackpad_map`.
- [ ] All 10 host test cases pass; `_test_fail_count == 0`; count >= 866.
- [ ] `IQS5XX_EXPLICIT_END_WINDOW`, `IQS5XX_SCROLL_DIV`, `IQS5XX_INVERT_X/Y` are defined
  as named constants in `trackpad.c`, not as magic numbers inside functions.
- [ ] Register addresses (`0x000F`, `0x0431`, `0xEEEE`) are referenced via `#define`
  constants, not hardcoded in `trackpad_task`.
- [ ] `s_need_reset_ack = true` is initialized at definition (static — always true at
  start; no explicit init needed beyond the declaration).
- [ ] `s_pending_release = false` is initialized at definition.
- [ ] `should_send == false` path uses `continue`, not a nested `if` block (keeps the hot
  path flat and avoids deep nesting).
- [ ] `tp.seq = s_seq++` is set by the caller (`trackpad_task`), not by `trackpad_map`.
  The test does not check `seq` (it is caller-assigned).
- [ ] `trackpad_map` does NOT log anything — no `ESP_LOGI` inside the pure function.
- [ ] `hid_send_mouse` call on the dongle uses the correct argument order:
  `(tp.buttons, tp.dx, tp.dy, tp.scroll_v)` — matches `hid_transport.h` signature
  `(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)`.
- [ ] `changed` is NOT set to true in the `PKT_TYPE_TRACKPAD` branch of `drain_radio`.
- [ ] All three boards (half_left, half_right, dongle) build green with `Project build complete.`
- [ ] `kase_v2` keyboard BT Kconfig failure is unchanged (not worsened).
- [ ] Bench checklist item 1 (RDY polarity) is validated FIRST before any gesture testing.

---

## Out of scope / deferred to future plans

| Item | Reason |
|------|--------|
| Horizontal scroll (`scroll_h`) | `hid_send_mouse` has no horizontal-wheel argument; field left zero |
| Right-click | No gesture allocated in v1 spec; deferred |
| Middle-click | No gesture allocated in v1 spec; deferred |
| Multi-finger drag, pinch, flick | Complex gesture pipeline; deferred |
| Cursor acceleration / ballistics | Map raw delta for now; deferred |
| Per-user sensitivity NVS config | `SCROLL_DIV` is compile-time only in v1; deferred |
| Absolute coordinate reporting | Touchpad mode vs mouse mode toggle; deferred |
| IQS5xx firmware update | OTP only; out of scope entirely |
| `trackpad_init` changes | Already real and hardware-validated; not touched |
| `half_spi`, `rf_driver`, descriptor changes | Already real; not touched |
| kase_v2 keyboard BT Kconfig failure | Pre-existing, unrelated; not touched |
