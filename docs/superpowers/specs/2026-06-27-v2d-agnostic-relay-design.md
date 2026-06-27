# V2D ↔ M.2 dongle — keyboard-agnostic relay (slice 1) — Design

**Goal:** Make the M.2 dongle a keyboard-agnostic USB front-end: a "smart"
keyboard that has its own brain (V2D, which already runs the full keymap engine
and has an unused NRF24) sends **final HID reports** to the dongle over NRF24; the
dongle **relays them straight to USB HID**. Config/telemetry flow over ESP-NOW.
The existing split path (dumb halves → raw matrix → engine on dongle) is
**unchanged** (dual-mode). First incremental step toward the endgame (dongle =
pure relay + security, brain always in the keyboard). See
[[project_m2_module_pivot]].

## Assumptions (correct on review)

- **V2D is wireless via the dongle** — it uses its NRF24 (HID) + ESP-NOW (config).
  If V2D must ALSO keep a wired direct-USB mode, that's an independent existing
  path and stays; this slice only adds the wireless-via-dongle route.
- **V2D sends final HID reports**, not 16-bit keycodes — its engine already
  produces reports, so the dongle stays a pure relay (no engine on this path).
- **Security (OpenPGP/OTP) is parked.** This slice builds the agnostic transport
  only; it deliberately leaves a clean seam for a future `K_SEC_CONFIRM`/security
  channel (§6) but implements no security.

## Non-goals

- Split central/peripheral migration (endgame option 2) — later.
- Any OpenPGP/secure-boot/NVS-enc work (parked).
- Power-management/battery tuning for a wireless V2D (separate concern; reuse the
  half's light-sleep learnings if/when V2D goes battery).

## Architecture

### Roles & transports

| Keyboard | Brain | NRF24 payload to dongle | Config/telemetry |
|----------|-------|-------------------------|------------------|
| Split halves (today) | **dongle** | raw matrix (row/col) | ESP-NOW telemetry |
| V2D (new) | **V2D** | **final HID reports** | ESP-NOW (config push + telemetry) |

The dongle becomes **dual-mode**, selecting the path by the NRF packet type (and
by the paired device's declared type, §5):
- `PKT_TYPE_KEY`/`HEARTBEAT`/`TRACKPAD` (raw matrix) → existing engine path.
- **`PKT_TYPE_HIDREPORT` (new)** → forward the report straight to USB HID, no engine.

### Data flow (V2D)

```
V2D matrix scan → V2D keymap engine → HID report (kbd 8B / consumer / mouse)
   → NRF24 PKT_TYPE_HIDREPORT → dongle rf_rx → hid_transport USB send
host config (controller) → dongle USB CDC → ESP-NOW → V2D (stores + uses locally)
V2D telemetry (layer/battery) → ESP-NOW → dongle (existing EN_INFO_* channel)
```

### Components / files (firmware)

- `main/comm/rf/rf_packet.h` — add `PKT_TYPE_HIDREPORT` (0x5) + encode/decode for
  a report payload: `[type][report_id:1][len:1][report bytes…]` (≤ 30 B body, fits
  NRF24 32). Host-testable encode/decode (like the existing rf_packet tests).
- **V2D side (KEYBOARD role gains an optional NRF-TX path):**
  - A new `main/comm/rf/kbd_relay_tx.c` (or extend the keyboard role): after the
    engine builds a report (`hid_report.c`), if "relay mode" is enabled, send it
    via the NRF driver instead of / in addition to local USB.
  - V2D needs the NRF driver (`rf_driver.c`) + ESP-NOW (`espnow_*`) compiled in
    the KEYBOARD role (currently dongle/half-only) — gate behind a new
    `CONFIG_KASE_KBD_WIRELESS` so V1/V2 wired keyboards are unaffected.
- **Dongle side:**
  - `main/comm/rf/rf_rx_task.c` `drain_radio()` — handle `PKT_TYPE_HIDREPORT`:
    decode → `hid_transport` USB send (reuse the existing HID send path), skip the
    engine. Keep the raw-matrix branch unchanged.
- **Config over ESP-NOW:**
  - New ESP-NOW message(s) `EN_CFG_*` (keymap/settings chunks) — host→dongle(CDC)
    →V2D. Reuse the existing `espnow_link`/`espnow_msg` framing; add config opcodes
    alongside the `EN_INFO_*` telemetry ones. V2D stores config in its own NVS
    (it owns the brain).
- **Pairing:** reuse the NRF pairing (`rf_pairing`), but the pair-req declares a
  **device type** (`dumb-half` vs `smart-keyboard`) so the dongle routes the
  paired device's packets to the right path. Extend `rf_encode_pair_req` with a
  type byte (there is room).

## Error handling / edge cases

- A `PKT_TYPE_HIDREPORT` from an unpaired/unknown source → dropped (existing
  pairing gate). (RF authentication is the separate parked RF-auth spec; this
  slice does not add it — note the residual injection risk stays until RF-auth.)
- Malformed report (len out of range) → dropped, bounded copy into the USB report
  buffer (`len` clamped to the report size). Host-tested.
- Link loss (heartbeat timeout) → release all keys held via that keyboard (reuse
  the existing `hb_check_timeout` release path; for the HID-report path, send an
  all-zero report on timeout).
- Dual-mode safety: a device paired as `smart-keyboard` must not also drive the
  engine (ignore any raw-matrix packets from it), and vice-versa.

## Testing

- **Host (pure logic, parallel-safe):** `test/test_rf_packet.c` — add
  `PKT_TYPE_HIDREPORT` encode/decode round-trip + bounds (len clamp, oversize
  rejected). Pairing device-type byte round-trip.
- **HW validation (manual):** flash V2D (wireless mode) + dongle; pair; confirm
  typing on V2D appears on the host via the dongle's USB; confirm split halves
  still work simultaneously (dual-mode); confirm config push over ESP-NOW reaches
  V2D; confirm V2D + dongle coexist with no USB collision (different VIDs, already
  verified).

## Future seam (security, parked)

The agnostic relay makes a clean place for security later: a smart keyboard can
send a `K_SEC_CONFIRM` signal to the dongle over the same NRF/ESP-NOW channel, so
the dongle's (future) security module can be authorized from ANY keyboard — not
only a split half. This slice does **not** implement it; it just avoids designing
the relay in a way that would block it. (When RF-auth lands, the HIDREPORT path
gets the same trailer as the other input packets.)

## Open items

- **Wired V2D coexistence:** confirm whether V2D keeps a wired direct-USB mode
  (then relay mode is a runtime/build option) or becomes wireless-only.
- **Config-over-ESP-NOW scope:** which settings flow this way (full keymap? just
  layers?) vs staying on V2D's own USB CDC when wired — to scope `EN_CFG_*`.
- **Power:** wireless V2D battery/light-sleep is out of scope here; flag if it
  becomes a blocker for daily use.
