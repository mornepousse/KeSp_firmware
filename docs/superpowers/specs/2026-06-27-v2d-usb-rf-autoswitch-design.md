# V2D USB↔NRF auto-switch + OLED indicator — design

**Date:** 2026-06-27
**Status:** approved (brainstorm)
**Scope:** wireless-relay keyboard build (`CONFIG_KASE_KBD_WIRELESS`, board kase_v2_debug)

## Goal

On the wireless V2D, deliver HID over its **own USB** whenever a USB host is
present, and **fall back to the NRF relay** (to the M.2 dongle) when USB is
unplugged. **USB is always first.** Show the active path (USB vs RF) on the OLED.

## Why not TinyUSB mount events

On the ESP32-S3, without VBUS sensing TinyUSB does not detect cable unplug:
neither the `tud_umount_cb` event nor the polled `tud_mounted()` state flips back
to false on disconnect (confirmed by the user on this hardware). So presence
detection uses a **hardware VBUS sense on a GPIO**, not the TinyUSB stack.

## Hardware

- USB VBUS (5 V) → resistor divider **100k / 100k** → ~2.5 V → `BOARD_VBUS_SENSE_GPIO`.
  - VBUS present → pin reads logic **HIGH**; unplugged → bottom resistor pulls **LOW**.
  - Digital read only (no ADC). GPIO configured **input, internal pulldown** as a
    belt-and-suspenders against a floating read if the divider is absent.
- `BOARD_VBUS_SENSE_GPIO` = **GPIO33** (default). Free, input-capable, non-strapping
  on the V2D. One `#define` in `boards/kase_v2_debug/board.h`; change to 34/35/36
  if a different pin is more accessible on the bodge. Nothing else depends on the
  specific number.

## Components

### 1. `usb_presence` module (new) — `main/comm/usb/usb_presence.{c,h}`
Tiny, single-responsibility presence reader. Compiled only when
`CONFIG_KASE_KBD_WIRELESS`.
- `void usb_presence_init(void);` — `gpio_config` the sense pin (input + pulldown).
- `bool usb_presence_active(void);` — **debounced** VBUS read: returns the stable
  level, flipping only after `USB_PRESENCE_DEBOUNCE_MS` (~50 ms) of consecutive
  agreeing raw reads. Internally polled each call; the caller polls it at the HID
  cadence and on the OLED refresh. No timer of its own (keep it passive/testable).
  - Raw read helper is isolated so the debounce logic is host-testable with a fake.

### 2. Pure routing decision — in `hid_report.c` (or `usb_presence.h` as a static inline)
```c
typedef enum { KBD_OUT_USB, KBD_OUT_RF, KBD_OUT_NONE } kbd_out_t;

/* USB-first: VBUS present -> USB; else relay if paired; else nothing. */
static inline kbd_out_t kbd_route_target(bool vbus_present, bool relay_active) {
    if (vbus_present) return KBD_OUT_USB;
    if (relay_active) return KBD_OUT_RF;
    return KBD_OUT_NONE;
}
```
This is the **only** decision logic and is **pure** → host-tested (TDD).

### 3. Routing integration — `main/input/hid_report.c`
The four existing `#if CONFIG_KASE_KBD_WIRELESS` sites (sender task ~L110, and
the `hid_queue==NULL` fallbacks in `send_hid_key`, `send_mouse_report`,
`send_hid_kb_mouse`) currently do `if (kbd_relay_active()) relay; else usb`.
Replace each with a switch on `kbd_route_target(usb_presence_active(),
kbd_relay_active())`:
- `KBD_OUT_USB` → `hid_send_*` (USB; `usb_bl_state==0`, no BLE on this build).
- `KBD_OUT_RF`  → `kbd_relay_send_*`.
- `KBD_OUT_NONE`→ drop (USB no-ops via `tud_hid_ready()` anyway).

To keep the four sites DRY, factor a small helper
`static kbd_out_t kbd_current_route(void)` in hid_report.c.

### 4. Transition safety
Keyboard reports are full-state and idempotent, so a path switch self-heals on
the next report. To avoid a key held *across* a switch lingering on the old path,
the sender task tracks the last route; on change it sends the **current** kb
report on the new path immediately (and a zero is unnecessary — the live state is
correct). Mouse is relative and simply resumes on the new path.

### 5. OLED indicator — `main/display/oled/oled_backend.c`
Reuse the existing `icon_path` slot in `oled_update_connection_icons()`. On the
wireless build, drive it from the cached route instead of `usb_bl_state`:
- `KBD_OUT_USB` → `flash` icon (existing asset).
- `KBD_OUT_RF`  → a radio icon (reuse `wifi` asset, already in the build).
Expose the cached route via `kbd_out_t kbd_report_active_route(void)` from
hid_report.c so the display reads it without re-polling the GPIO. BLE-specific
icon branches are already compiled out (no BLE on this build).

## Testing

- **Host (TDD):** `test/test_kbd_route.c` — exhaustive table for
  `kbd_route_target` (all 4 combinations of vbus_present × relay_active) +
  debounce logic for `usb_presence` via a fake raw-read source (no flip before
  N agreeing samples; flip after; bounce rejection). Added to
  `test/CMakeLists.txt` + `test/test_main.c`.
- **Hardware (with Mae):** wire the divider to GPIO33; verify typing goes over
  USB when plugged, switches to the dongle within ~50 ms of unplug, switches back
  on replug; OLED icon tracks the path.

## Out of scope
- The residual NRF link RF margin (~bodge antenna) — separate work.
- The dongle-side watchdog re-arm refinement — separate.
- Non-wireless keyboard builds (V1/V2) are unaffected (`#if KBD_WIRELESS`).
