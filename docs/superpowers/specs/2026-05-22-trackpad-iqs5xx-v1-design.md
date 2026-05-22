# IQS5xx Trackpad Driver v1 — Design Spec

**Date:** 2026-05-22
**Branch:** `dongle-firmware`
**Target:** KaSe half-left (ESP32-S3) + KaSe dongle (ESP32-S3)
**ESP-IDF:** 5.5
**Scope:** Cursor movement (RelX/RelY) + left-click via single-tap + 2-finger vertical scroll.

---

## 1. Overview and Scope

This spec covers filling the single TODO stub in `main/periph/trackpad/trackpad.c`:
the IQS5xx-B000 register read inside `trackpad_task`. All surrounding
infrastructure is already real and hardware-validated — only the I2C protocol
interaction and the gesture→HID-field mapping need to be implemented.

The dongle side requires one additional change: replacing the dropped-packet
stub in `rf_rx_task.c` with a forwarding call to `hid_send_mouse`.

### v1 Scope — IN

| Feature | Delivered by |
|---|---|
| Cursor movement (RelX, RelY) | IQS5xx registers 0x0014/0x0016 |
| Left-click (single-tap gesture) | GestureEvents0 bit 0 (SingleTap) |
| 2-finger vertical scroll | GestureEvents1 bit 1 (Scroll) via RelY |
| Activity gating (suppress zero packets) | Mapping logic in trackpad_task |

### v1 Scope — OUT

| Feature | Reason |
|---|---|
| Horizontal scroll | `hid_send_mouse` has no horizontal-wheel argument; field scroll_h exists in rf_trackpad_t but is left zero |
| Right-click | Out of scope; no gesture allocated |
| Multi-finger drag, flick, zoom | Complex gesture pipeline; deferred |
| Cursor acceleration / ballistics | Deferred; map raw delta for now |
| IQS5xx full re-configuration over I2C | OTP defaults are sufficient for movement |

---

## 2. Architecture and Data Flow

The pipeline spans two firmware roles. All segments except the two fill-in
points already exist and compile.

```
[HALF-LEFT firmware]

  RDY GPIO14 (active-low)
       │ NEGEDGE ISR
       ▼
  s_rdy_sem (binary semaphore)
       │ xSemaphoreTake
       ▼
  trackpad_task (prio 6, stack 3072, core 0)
       │
       ├── [FILL] i2c_master_transmit_receive:
       │     write reg addr {0x00,0x0F} → read 9 bytes
       │     → GestureEvents0, GestureEvents1, SystemInfo0,
       │        SystemInfo1, NumberOfFingers, RelX[2], RelY[2]
       │
       ├── [FILL] gesture → HID field mapping
       │     dx, dy, buttons, scroll_v
       │
       ├── [FILL] activity gate: skip rf_driver_send if all zero
       │
       ├── rf_encode_trackpad(buf, &tp)        ← already real
       │
       ├── half_spi_lock()                     ← already real
       ├── rf_driver_send(&s_radio, buf, 7)    ← already real (PKT_TYPE_TRACKPAD)
       └── half_spi_unlock()                   ← already real


[DONGLE firmware]

  rf_rx_task → drain_radio → PKT_TYPE_TRACKPAD
       │
       ├── rf_decode_trackpad(buf, n, &tp)     ← already real
       │
       └── [FILL] hid_send_mouse(tp.buttons, tp.dx, tp.dy, tp.scroll_v)
                  ↓
             hid_transport.c → tud_hid_mouse_report(REPORT_ID_MOUSE=2, …)
             (dongle is always USB; usb_bl_state=0)
```

No new files, no new tasks, no descriptor changes. The TinyUSB composite
descriptor already includes `TUD_HID_REPORT_DESC_MOUSE` in `usb_hid.c`.

### CMakeLists.txt linkage — confirmed

`comm/hid_transport.c` is in the **always-compiled** `srcs` set (line 13 of
`main/CMakeLists.txt`), not gated on any Kconfig flag. It is therefore present
in the dongle build without any CMakeLists change needed.

---

## 3. IQS5xx-B000 Register Read — Window Transfer Protocol

The IQS5xx uses a *Communication Window* protocol (Azoteq AN171). The master
must begin an I2C transaction while RDY is asserted, complete a register read,
then explicitly end the window. The chip holds RDY asserted for a limited time;
if the master is too slow, the chip drops the window and deasserts RDY.

### 3.1 Register Map (read block)

All addresses are 16-bit, sent big-endian as the write phase of the transfer.
The read block is contiguous starting at address **0x000F** and covers 9 bytes:

| Offset | Address | Name | Width | Notes |
|--------|---------|------|-------|-------|
| 0 | 0x000F | GestureEvents0 | 1 byte | Tap, double-tap, swipe bits |
| 1 | 0x0010 | GestureEvents1 | 1 byte | Scroll, flick, palm bits |
| 2 | 0x0011 | SystemInfo0 | 1 byte | ShowReset in bit7 |
| 3 | 0x0012 | SystemInfo1 | 1 byte | (reserved in v1) |
| 4 | 0x0013 | NumberOfFingers | 1 byte | Touch count 0..5 |
| 5-6 | 0x0014 | RelativeX | 2 bytes | Signed 16-bit big-endian |
| 7-8 | 0x0016 | RelativeY | 2 bytes | Signed 16-bit big-endian |

Total read: 9 bytes into `uint8_t data[9]`.

Extract fields:

```c
uint8_t  gest0     = data[0];
uint8_t  gest1     = data[1];
uint8_t  sysinfo0  = data[2];
/* data[3] = SystemInfo1, unused in v1 */
uint8_t  n_fingers = data[4];
int16_t  rel_x     = (int16_t)((data[5] << 8) | data[6]);
int16_t  rel_y     = (int16_t)((data[7] << 8) | data[8]);
```

### 3.2 Single I2C Transaction

Use the ESP-IDF v5 new `i2c_master` driver already initialized in
`trackpad_init`. One call per RDY event:

```c
uint8_t reg_addr[2] = {0x00, 0x0F};  /* big-endian: MSB=0x00, LSB=0x0F */
uint8_t data[9];
esp_err_t err = i2c_master_transmit_receive(
    s_i2c_device,
    reg_addr, 2,   /* write: 2-byte register address */
    data, 9,       /* read: 9-byte contiguous block */
    50             /* timeout ms */
);
if (err != ESP_OK) {
    ESP_LOGW(TAG, "I2C read failed: %d", err);
    /* skip this event; do NOT send; do NOT end window (bus is already free) */
    continue;
}
```

**Note on STOP vs End-Window byte:** The `i2c_master_transmit_receive` call
issues a STOP condition after the read phase. On some IQS5xx revisions this
STOP is sufficient to end the communication window. On others (and per several
application notes), a separate 1-byte write to address 0xEEEE is required as
an explicit "End Communication Window" command. This is a **verify-at-bench
item** (see Section 7). The implementation should include the 0xEEEE write
guarded by a `#define IQS5XX_EXPLICIT_END_WINDOW 1` compile-time flag,
defaulting to enabled, so it can be toggled without a code change:

```c
#if IQS5XX_EXPLICIT_END_WINDOW
/* End Communication Window: write 1 byte (value irrelevant) to 0xEEEE */
uint8_t end_cmd[3] = {0xEE, 0xEE, 0x00};  /* addr MSB, addr LSB, dummy byte */
/* Best-effort; ignore error — chip may have already closed the window */
i2c_master_transmit(s_i2c_device, end_cmd, 3, 10);
#endif
```

### 3.3 Reset Acknowledgement (ShowReset)

On the first read after a hardware reset (RST pulse in `trackpad_init`),
`SystemInfo0` bit 7 (`ShowReset`) will be set to 1. The chip expects the
master to acknowledge this by writing 1 to the `ACK_RESET` bit in the
System Control register before normal operation resumes.

**Exact register and bit positions for ACK_RESET are a verify-at-bench item**
(Section 7 item 6). Based on AN171 and IQS5xx-B000 datasheet, the expected
location is:

- Register address: **0x0431** (System Control 0)
- Bit position: **bit 1** (`ACK_RESET` = 0x02)

The implementation writes `{0x04, 0x31, 0x02}` (3 bytes: reg MSB, reg LSB,
value) once when `ShowReset` is detected. After ACK, normal reads proceed.
No full device reconfiguration is needed — the OTP defaults (400 kHz I2C,
address 0x74, relative XY enabled, gesture recognition on) are sufficient for
v1 movement and tap detection.

State machine for reset ACK:

```
on first entry to trackpad_task loop:
    s_need_reset_ack = true

each iteration, after reading data[]:
    if (s_need_reset_ack && (sysinfo0 & 0x80)) {
        /* write ACK_RESET to System Control 0 */
        uint8_t ack_cmd[3] = {0x04, 0x31, 0x02};
        i2c_master_transmit(s_i2c_device, ack_cmd, 3, 20);
        s_need_reset_ack = false;
        continue;   /* skip this event's data — RelX/RelY are garbage post-reset */
    }
```

If `ShowReset` is not set on the first read, `s_need_reset_ack` is cleared
and the first packet's data is used normally.

---

## 4. Gesture → HID Field Mapping

### 4.1 Gesture Bit Masks

| Symbol | Register | Bit | Value | Source |
|--------|----------|-----|-------|--------|
| `GEST0_SINGLE_TAP` | GestureEvents0 | 0 | 0x01 | AN171 §5.1 |
| `GEST1_SCROLL` | GestureEvents1 | 1 | 0x02 | AN171 §5.2 |

**These bit positions are per AN171 and must be verified against the exact
IQS5xx-B000 datasheet at implementation/bench time.** See Section 7 item 5.

### 4.2 Tunable Constants

```c
#define IQS5XX_SCROLL_DIV        8     /* divide RelY for scroll speed; tune at bench */
#define IQS5XX_EXPLICIT_END_WINDOW 1   /* 1 = send 0xEEEE end-window byte */
```

`SCROLL_DIV` should be exposed via a `#define` in `trackpad.c` (not in
`board.h`; it is chip behavior, not board wiring). Tune at bench (Section 7
item 4). A value of 8 gives ~1 scroll unit per 8 raw units of finger movement.

### 4.3 Mapping Logic (pure, extractable)

The mapping from raw IQS5xx fields to `rf_trackpad_t` fields follows this
precedence: scroll gesture overrides cursor movement; tap sets click; activity
gate suppresses zero packets.

```
given: gest0, gest1, n_fingers, rel_x, rel_y

scroll_active = (gest1 & GEST1_SCROLL) != 0

if scroll_active:
    dx       = 0
    dy       = 0
    scroll_v = clamp(rel_y / SCROLL_DIV, -127, +127)
    scroll_h = 0   /* OUT of v1 scope — always zero */
else:
    dx       = clamp(rel_x, -127, +127)
    dy       = clamp(rel_y, -127, +127)
    scroll_v = 0
    scroll_h = 0

tap_detected = (gest0 & GEST0_SINGLE_TAP) != 0
```

**Note on clamp:** `rel_x` and `rel_y` are `int16_t`. Clamping to `[-127,
+127]` (not `[-128, +127]`) avoids the ambiguity of `(int8_t)(-128)` in
two's-complement edge cases. Use a small inline:

```c
static inline int8_t clamp8(int16_t v)
{
    if (v >  127) return  127;
    if (v < -127) return -127;
    return (int8_t)v;
}
```

This function is pure (no I/O, no side effects) and is the primary candidate
for host-side unit testing. See Section 6.

### 4.4 Click State Machine (single-tap pulse)

The IQS5xx sets the SingleTap bit for exactly one RDY event (the event where
the gesture is recognized). A mouse HID button requires a press packet
followed by a release packet. Because the tap bit persists for only one read,
the firmware must synthesize the release in the following iteration.

State: one boolean `s_pending_release` (static in `trackpad_task`, initial
value `false`).

```
each iteration, after computing tap_detected:

if s_pending_release:
    buttons = 0x00           /* release */
    s_pending_release = false
    /* force send (activity gate: button release always sends) */
    force_send = true

else if tap_detected:
    buttons = 0x01           /* left-click press */
    s_pending_release = true
    force_send = true        /* button press always sends */

else:
    buttons = 0x00
    force_send = false
```

The `force_send` flag bypasses the activity gate for button events. This
ensures the release packet is never suppressed even when there is no
movement in that iteration.

### 4.5 Activity Gate

To avoid flooding the NRF24 channel with zero-delta packets (which wastes
2.4 GHz airtime and can interfere with the keyboard key-event channel), only
send a PKT_TRACKPAD when there is something to report:

```
send_this_packet = force_send
               || (dx != 0)
               || (dy != 0)
               || (scroll_v != 0)
               || (buttons != 0x00)

if send_this_packet:
    tp.dx = dx; tp.dy = dy; tp.buttons = buttons;
    tp.scroll_v = scroll_v; tp.scroll_h = 0; tp.seq = s_seq++;
    rf_encode_trackpad(buf, &tp);
    half_spi_lock();
    rf_driver_send(&s_radio, buf, 7);
    half_spi_unlock();
/* else: drop silently — no log to avoid hot-path overhead */
```

---

## 5. Dongle Wiring — rf_rx_task.c

In `main/comm/rf/rf_rx_task.c`, function `drain_radio`, replace the existing
`PKT_TYPE_TRACKPAD` stub block:

```c
/* EXISTING stub (drop + log): */
} else if (type == PKT_TYPE_TRACKPAD) {
    rf_trackpad_t tp;
    if (rf_decode_trackpad(buf, n, &tp)) {
        (void)tp;
        ESP_LOGD(TAG, "PKT_TRACKPAD dx=%d dy=%d btn=0x%02x (stub — dropped)",
                 tp.dx, tp.dy, tp.buttons);
    }
}
```

Replace with:

```c
} else if (type == PKT_TYPE_TRACKPAD) {
    rf_trackpad_t tp;
    if (rf_decode_trackpad(buf, n, &tp)) {
        hid_send_mouse(tp.buttons, tp.dx, tp.dy, tp.scroll_v);
    }
}
```

Also add the include at the top of `rf_rx_task.c` if not already present:

```c
#include "hid_transport.h"
```

**No horizontal scroll:** `hid_send_mouse` signature is
`(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)` — four arguments, no
horizontal wheel. The `tp.scroll_h` field is always zero (enforced by the
half-side mapping logic) and is not forwarded. This is consistent with v1
scope.

**No `changed` flag:** The trackpad packet does NOT set `changed = true` in
`drain_radio`. The `changed` flag triggers `run_engine_cycle()` which drives
the keyboard HID report. Mouse reports are independent and dispatched directly
via `hid_send_mouse`. This is already the correct behaviour of the surrounding
code; no modification needed to the `changed` logic.

**Linkage:** `hid_transport.c` is in the always-compiled `srcs` set in
`main/CMakeLists.txt` (confirmed, line 13). No CMakeLists change needed.

---

## 6. Testing

### 6.1 Host-Testable Pure Logic

The gesture-to-HID mapping is a good candidate for extraction into a pure C
function with no ESP-IDF dependencies:

```c
/* Proposed signature (extract to trackpad_mapping.c / trackpad_mapping.h) */
typedef struct {
    int8_t  dx, dy;
    uint8_t buttons;
    int8_t  scroll_v;
    bool    pending_release_out;   /* new state of s_pending_release */
    bool    should_send;
} trackpad_mapping_result_t;

trackpad_mapping_result_t trackpad_map(
    uint8_t gest0,
    uint8_t gest1,
    int16_t rel_x,
    int16_t rel_y,
    bool    pending_release_in
);
```

This function is pure: no I/O, no FreeRTOS calls, no global state. It can be
compiled and tested on the host with the existing `test/` CMake framework.

**Recommended unit test cases:**

| Test | Inputs | Expected output |
|------|--------|-----------------|
| No touch | gest0=0, gest1=0, rel_x=0, rel_y=0, pending=false | dx=0, dy=0, btn=0, sv=0, send=false |
| Movement | gest0=0, gest1=0, rel_x=50, rel_y=-20, pending=false | dx=50, dy=-20, btn=0, sv=0, send=true |
| Clamp positive | rel_x=200 | dx=127 |
| Clamp negative | rel_x=-200 | dx=-127 |
| Single tap press | gest0=GEST0_SINGLE_TAP, rel_x=0, rel_y=0, pending=false | btn=0x01, pending_out=true, send=true |
| Single tap release | gest0=0, pending=true | btn=0x00, pending_out=false, send=true |
| Scroll active | gest1=GEST1_SCROLL, rel_x=30, rel_y=64, pending=false | dx=0, dy=0, sv=8 (64/8), send=true |
| Scroll clamp | gest1=GEST1_SCROLL, rel_y=1200 | sv=127 |
| Scroll zero | gest1=GEST1_SCROLL, rel_y=3 | sv=0, send=false (3/8=0) |
| Movement + scroll mutual exclusion | gest1=GEST1_SCROLL, rel_x=50, rel_y=64 | dx=0, dy=0, sv=8 |

The `scroll_zero` case (rel_y=3, SCROLL_DIV=8 → sv=0) is a known edge: the
activity gate should drop this packet. Verifying it does not sends is
important for NRF channel hygiene.

### 6.2 Hardware Integration Test (bench)

After wiring and flashing:

1. Load KaSe Controller (remapping software) and observe mouse cursor moves
   with finger drag.
2. Tap trackpad once — single left-click should register (visible in xev or
   equivalent).
3. Two-finger drag vertically — scroll events should appear (test in browser
   or text editor).
4. Confirm no phantom packets when finger is lifted (activity gate working).
5. Measure NRF packet rate under active use vs idle — should drop to zero at
   idle.

---

## 7. Verify-at-Bench Checklist

These items cannot be resolved by reading the datasheet alone; physical
measurement or chip-revision-specific documentation is required. They are
**not solved blind** in this spec — they are flagged as open.

| # | Item | Risk if wrong | Resolution |
|---|------|---------------|------------|
| 1 | **RDY polarity** — skeleton uses `GPIO_INTR_NEGEDGE` (active-low). IQS5xx-B000 RDY can be active-HIGH depending on chip variant/OTP config. | ISR fires at wrong edge; trackpad never responds or fires continuously | Probe GPIO14 with oscilloscope or logic analyser at startup; if no activity, flip to `GPIO_INTR_POSEDGE` |
| 2 | **End-Communication-Window mechanism** — spec includes 0xEEEE write controlled by `IQS5XX_EXPLICIT_END_WINDOW`. On some revisions a STOP alone is sufficient; on others omitting 0xEEEE causes the chip to lock the bus. | Bus hangs; all subsequent reads time out | Test with flag enabled (default) first; if bus hangs try disabled |
| 3 | **dx/dy axis sign and orientation** — depends on physical mounting of the TPS43-201A-S module (up/down, left/right, rotated). RelX/RelY may need to be negated or swapped. | Cursor moves inverted or wrong axis | Move finger right, check if cursor moves right; negate `rel_x` or `rel_y` as needed. Add `#define IQS5XX_INVERT_X 0` / `INVERT_Y 0` compile-time flags. |
| 4 | **SCROLL_DIV value** — default 8 is a starting estimate. 2-finger scroll speed may be too slow or too fast. | Poor UX | Tune at bench; 4–16 is typical range |
| 5 | **Gesture event bit positions** — GEST0 bit0 = SingleTap and GEST1 bit1 = Scroll are per AN171 §5. The exact IQS5xx-B000 variant (B000 vs B001) may shift these. | Tap does not register or scroll triggers on wrong gesture | Dump GestureEvents0/1 raw bytes while performing gestures; identify which bits toggle |
| 6 | **ShowReset / ACK_RESET register** — spec uses address 0x0431 bit1 based on AN171. Confirm against IQS5xx-B000 datasheet section "System Control Registers". | Reset-ack write goes to wrong register; chip stays in post-reset state and ignores subsequent commands | Read SystemInfo0 after reset; if ShowReset stays set, the ack-write address is wrong |

---

## 8. Register / Constant Reference Table

```c
/* IQS5xx-B000 register addresses (16-bit, big-endian on wire) */
#define IQS5XX_REG_GESTURE_EVENTS_0   0x000F
#define IQS5XX_REG_GESTURE_EVENTS_1   0x0010
#define IQS5XX_REG_SYSTEM_INFO_0      0x0011
#define IQS5XX_REG_SYSTEM_INFO_1      0x0012
#define IQS5XX_REG_NUMBER_OF_FINGERS  0x0013
#define IQS5XX_REG_RELATIVE_X         0x0014   /* int16, big-endian */
#define IQS5XX_REG_RELATIVE_Y         0x0016   /* int16, big-endian */
#define IQS5XX_REG_SYSTEM_CONTROL_0   0x0431   /* ACK_RESET bit — VERIFY */
#define IQS5XX_REG_END_WINDOW         0xEEEE   /* End Communication Window */

/* Gesture event bitmasks — VERIFY against IQS5xx-B000 datasheet */
#define IQS5XX_GEST0_SINGLE_TAP       0x01     /* GestureEvents0 bit 0 */
#define IQS5XX_GEST1_SCROLL           0x02     /* GestureEvents1 bit 1 */

/* SystemInfo0 bitmasks */
#define IQS5XX_SYSINFO0_SHOW_RESET    0x80     /* bit 7 — set after reset */

/* System Control 0 bitmasks */
#define IQS5XX_SYSCTRL0_ACK_RESET     0x02     /* bit 1 — VERIFY address */

/* Tunable behaviour */
#define IQS5XX_SCROLL_DIV             8        /* tune at bench */
#define IQS5XX_EXPLICIT_END_WINDOW    1        /* 1 = write 0xEEEE after read */
#define IQS5XX_INVERT_X               0        /* 1 = negate dx — verify at bench */
#define IQS5XX_INVERT_Y               0        /* 1 = negate dy — verify at bench */

/* Read block layout: start address and byte count */
#define IQS5XX_READ_START_ADDR        0x000F   /* GestureEvents0 */
#define IQS5XX_READ_BYTE_COUNT        9        /* through RelY LSB at 0x0018 */
```

### Read block byte layout (offset from start of 9-byte buffer)

```
buf[0]    GestureEvents0        0x000F
buf[1]    GestureEvents1        0x0010
buf[2]    SystemInfo0           0x0011
buf[3]    SystemInfo1           0x0012
buf[4]    NumberOfFingers       0x0013
buf[5]    RelativeX MSB         0x0014
buf[6]    RelativeX LSB         0x0015
buf[7]    RelativeY MSB         0x0016
buf[8]    RelativeY LSB         0x0017
```

---

## 9. Out of Scope (v1)

These items are explicitly deferred. They must not be added to the v1
implementation without a separate spec update.

- Horizontal scroll (`scroll_h`) — blocked by `hid_send_mouse` signature
- Right-click (GestureEvents0 press-and-hold or 2-finger tap)
- Middle-click
- Multi-finger gestures (3-finger swipe, pinch)
- Drag and drop (touch-and-move)
- Cursor ballistics / acceleration curves
- Per-user sensitivity configuration (NVS-persisted SCROLL_DIV etc.)
- Trackpad firmware update (IQS5xx has its own OTP; out of scope entirely)
- Absolute coordinate reporting (touchpad mode vs mouse mode toggle)

---

## 10. Implementation Checklist (for the executor)

Ordered by dependency:

- [ ] Define all constants and macros in `trackpad.c` (Section 8)
- [ ] Extract `clamp8()` inline; extract `trackpad_map()` pure function if
      doing host tests (Section 6.1)
- [ ] Write host unit tests for `trackpad_map()` (Section 6.1 table)
- [ ] Implement IQS5xx read in `trackpad_task`: reg addr write → 9-byte read
      → optional 0xEEEE end-window write (Section 3.2)
- [ ] Implement ShowReset / ACK_RESET handling (Section 3.3)
- [ ] Implement gesture mapping + click state machine + activity gate
      (Sections 4.3, 4.4, 4.5)
- [ ] Replace PKT_TRACKPAD stub in `rf_rx_task.c` with `hid_send_mouse` call;
      add `#include "hid_transport.h"` (Section 5)
- [ ] Flash half + dongle; run bench verification (Section 7 checklist)
- [ ] Adjust axis inversion, SCROLL_DIV, RDY edge as needed
