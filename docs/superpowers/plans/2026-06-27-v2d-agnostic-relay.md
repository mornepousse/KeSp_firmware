# V2D Agnostic Relay (slice 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A "smart" wireless keyboard (V2D, own brain, unused NRF24) sends final HID reports to the M.2 dongle over NRF24; the dongle relays them straight to USB HID (no engine). The rebind software configures V2D *through* the dongle by tunneling the existing KS/KR protocol over ESP-NOW. The split path (dumb halves → raw matrix → engine on dongle) is untouched (dual-mode).

**Architecture:** New `PKT_TYPE_HIDREPORT` on NRF for keyboard→dongle reports (dongle = pure relay). New `CONFIG_KASE_KBD_WIRELESS` compiles NRF-TX + ESP-NOW into the keyboard role (V1/V2 wired unaffected). The dongle's CDC dispatch routes config-class KS frames to the paired smart keyboard over ESP-NOW (chunked) and relays the KR back; dongle-class commands stay local. Spec: `docs/superpowers/specs/2026-06-27-v2d-agnostic-relay-design.md`.

**Tech Stack:** C11, ESP-IDF 5.5, existing host test harness, `hid_transport` (`hid_send_keyboard`/`hid_send_mouse`), `espnow_send`, the KS/KR binary protocol (`cdc_binary_protocol`).

**Decisions baked in (flagged — correct if wrong):** V2D wireless-via-dongle, relay behind `CONFIG_KASE_KBD_WIRELESS` (wired V2D not foreclosed — it's a build/runtime switch). V2D sends **final HID reports** (dongle pure relay), not 16-bit keycodes.

---

## File structure

- Modify `main/comm/rf/rf_packet.h` — `PKT_TYPE_HIDREPORT` (0x5), kbd/mouse encode+decode; pair-req device-type byte.
- Modify `test/test_rf_packet.c` — round-trip + bounds tests.
- Modify `main/Kconfig.projbuild` — `CONFIG_KASE_KBD_WIRELESS`.
- Modify `main/CMakeLists.txt` — under KEYBOARD role + `KASE_KBD_WIRELESS`, compile `rf_driver.c`, `espnow_*`, and a new `comm/rf/kbd_relay_tx.c`.
- Create `main/comm/rf/kbd_relay_tx.c` + `.h` — keyboard-side: build HIDREPORT from a report, NRF-send; ESP-NOW KS-frame receive → local KS dispatch → KR back.
- Modify `main/input/hid_report.c` — divert output to the relay when relay mode is active.
- Modify `main/comm/rf/rf_rx_task.c` — dongle: handle `PKT_TYPE_HIDREPORT` → `hid_send_keyboard`/`hid_send_mouse`; smart-keyboard link-loss → zero report.
- Create `main/comm/rf/cfg_bridge.c` + `.h` — dongle: `cfg_is_dongle_local(cmd_id)` routing predicate (host-testable) + ESP-NOW tunnel (chunk KS frame ↔ V2D).
- Modify `main/comm/cdc/cdc_binary_protocol.c` — in dispatch, route config-class frames via `cfg_bridge` when a smart keyboard is paired.
- Modify `main/comm/rf/rf_pairing.{c,h}` — device-type in pairing (dumb-half / smart-keyboard).

---

### Task 1: `PKT_TYPE_HIDREPORT` encode/decode — TDD

**Files:** Modify `main/comm/rf/rf_packet.h`; Test `test/test_rf_packet.c`

- [ ] **Step 1: Failing test** in `test/test_rf_packet.c` (add call to `test_rf_packet`):

```c
static void test_rf_pkt_hidreport_roundtrip(void) {
    /* keyboard report: modifier + 6 keys */
    uint8_t kb[6] = {0x04,0x05,0,0,0,0};
    uint8_t buf[32];
    uint16_t n = rf_encode_hidreport_kbd(buf, 0x02, kb);
    TEST_ASSERT_EQ(n, 9, "kbd hidreport = 1+1+1+6");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_HIDREPORT, "type 0x5");
    uint8_t mod, kbo[6]; uint8_t btn; int8_t x,y,w; uint8_t sub;
    TEST_ASSERT(rf_decode_hidreport(buf, n, &sub, &mod, kbo, &btn, &x, &y, &w), "decode kbd");
    TEST_ASSERT(sub==0 && mod==0x02 && kbo[0]==0x04 && kbo[1]==0x05, "kbd fields");
    /* mouse report */
    n = rf_encode_hidreport_mouse(buf, 0x01, 5, -3, 1);
    TEST_ASSERT_EQ(n, 6, "mouse hidreport = 1+1+1+1+1+1");
    TEST_ASSERT(rf_decode_hidreport(buf, n, &sub, &mod, kbo, &btn, &x, &y, &w), "decode mouse");
    TEST_ASSERT(sub==1 && btn==0x01 && x==5 && y==-3 && w==1, "mouse fields");
    /* truncated → reject */
    TEST_ASSERT(!rf_decode_hidreport(buf, 2, &sub, &mod, kbo, &btn, &x, &y, &w), "runt rejected");
}
```

- [ ] **Step 2: Run, expect FAIL** (`PKT_TYPE_HIDREPORT`/encoders undefined): `cd test && rm -rf build && cmake -S . -B build >/dev/null && cmake --build build 2>&1 | tail -3`
- [ ] **Step 3: Implement** in `rf_packet.h`:

```c
#define PKT_TYPE_HIDREPORT 0x5
#define RF_HID_SUB_KBD   0
#define RF_HID_SUB_MOUSE 1

static inline uint16_t rf_encode_hidreport_kbd(uint8_t *buf, uint8_t modifier, const uint8_t kb[6]) {
    buf[0] = (PKT_TYPE_HIDREPORT << 4); buf[1] = RF_HID_SUB_KBD; buf[2] = modifier;
    memcpy(buf + 3, kb, 6); return 9;
}
static inline uint16_t rf_encode_hidreport_mouse(uint8_t *buf, uint8_t buttons, int8_t x, int8_t y, int8_t wheel) {
    buf[0] = (PKT_TYPE_HIDREPORT << 4); buf[1] = RF_HID_SUB_MOUSE;
    buf[2] = buttons; buf[3] = (uint8_t)x; buf[4] = (uint8_t)y; buf[5] = (uint8_t)wheel; return 6;
}
static inline bool rf_decode_hidreport(const uint8_t *buf, uint16_t len, uint8_t *sub,
        uint8_t *mod, uint8_t kb[6], uint8_t *btn, int8_t *x, int8_t *y, int8_t *wheel) {
    if (len < 2 || rf_packet_type(buf, len) != PKT_TYPE_HIDREPORT) return false;
    *sub = buf[1];
    if (buf[1] == RF_HID_SUB_KBD)   { if (len < 9) return false; *mod = buf[2]; memcpy(kb, buf+3, 6); return true; }
    if (buf[1] == RF_HID_SUB_MOUSE) { if (len < 6) return false; *btn = buf[2];
        *x=(int8_t)buf[3]; *y=(int8_t)buf[4]; *wheel=(int8_t)buf[5]; return true; }
    return false;
}
```

- [ ] **Step 4: Run, expect PASS.** `./scripts/check.sh --host-only`
- [ ] **Step 5: Commit** `git add main/comm/rf/rf_packet.h test/test_rf_packet.c && git commit -m "feat(rf): PKT_TYPE_HIDREPORT encode/decode (TDD)"`

---

### Task 2: Pairing device-type byte — TDD

**Files:** Modify `main/comm/rf/rf_packet.h`, `main/comm/rf/rf_pairing.h`; Test `test/test_rf_packet.c`

- [ ] **Step 1: Failing test:**

```c
static void test_rf_pair_devtype(void) {
    uint8_t buf[16]; uint8_t mac[6]={1,2,3,4,5,6};
    uint16_t n = rf_encode_pair_req2(buf, mac, 0x01, RF_DEV_SMART_KBD);
    uint8_t mo[6], slot, dev;
    TEST_ASSERT(rf_decode_pair_req2(buf, n, mo, &slot, &dev), "decode v2 pair req");
    TEST_ASSERT(dev == RF_DEV_SMART_KBD && slot == 0x01, "devtype + slot");
}
```

- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** in `rf_pairing.h`: `#define RF_DEV_DUMB_HALF 0` / `#define RF_DEV_SMART_KBD 1`; in `rf_packet.h` add a 9-byte pair-req variant (type, mac[6], slot, devtype) `rf_encode_pair_req2` / `rf_decode_pair_req2` (keep the old 8-byte for legacy halves; `rf_decode_pair_req2` treats len==8 as `RF_DEV_DUMB_HALF`).
- [ ] **Step 4: Run, expect PASS.** `./scripts/check.sh --host-only`
- [ ] **Step 5: Commit** `git commit -am "feat(rf): pairing device-type (dumb-half vs smart-keyboard) (TDD)"`

---

### Task 3: Config routing predicate `cfg_is_dongle_local` — TDD

**Files:** Create `main/comm/rf/cfg_bridge.h`, `main/comm/rf/cfg_bridge.c`; Test `test/test_cfg_bridge.c` (register in test_main.c + CMakeLists)

- [ ] **Step 1: Failing test** in `test/test_cfg_bridge.c`:

```c
#include "test_framework.h"
#include "../main/comm/rf/cfg_bridge.h"
#include "../main/comm/cdc/cdc_binary_protocol.h"
static void test_cfg_routing(void) {
    /* dongle-local: RF + pairing + battery + OTA */
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_RF_STATUS), "RF_STATUS local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_RF_PAIR_LIST), "PAIR_LIST local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_BATTERY), "BATTERY local");
    /* config-class: keymap/macros/etc → forwarded to the smart keyboard */
    TEST_ASSERT(!cfg_is_dongle_local(KS_CMD_SETKEY), "SETKEY forwarded");
    TEST_ASSERT(!cfg_is_dongle_local(KS_CMD_MACRO_ADD), "MACRO forwarded");
}
void test_cfg_bridge(void){ TEST_SUITE("cfg_bridge"); test_cfg_routing(); }
```
(Use the actual KS_CMD ids present in `cdc_binary_protocol.h`.)

- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `cfg_bridge.c` (pure): `bool cfg_is_dongle_local(uint8_t cmd_id)` — a small allow-list of dongle-local ids (the `0xB*` RF/battery/monitor block, OTA, NVS-reset, DFU); everything else (keymap/macro/combo/leader/stats/sec) is config-class → forwarded. No ESP-IDF deps.
- [ ] **Step 4: Run, expect PASS.** `./scripts/check.sh --host-only`
- [ ] **Step 5: Commit** `git commit -am "feat(rf): cfg_bridge routing predicate (dongle-local vs forward) (TDD)"`

---

### Task 4: `CONFIG_KASE_KBD_WIRELESS` + build wiring

**Files:** Modify `main/Kconfig.projbuild`, `main/CMakeLists.txt`

- [ ] **Step 1:** Add to `Kconfig.projbuild`: `config KASE_KBD_WIRELESS bool "Keyboard talks to the M.2 dongle wirelessly (NRF HID + ESP-NOW config)" default n`.
- [ ] **Step 2:** In `CMakeLists.txt`, under the KEYBOARD-role block, `if(CONFIG_KASE_KBD_WIRELESS)` append `comm/rf/rf_driver.c comm/rf/kbd_relay_tx.c comm/espnow/espnow_link.c comm/espnow/espnow_info.c` (unconditional-require pattern like esp_wifi for any new component deps).
- [ ] **Step 3: Build** a wired board to confirm no regression: `source ~/esp/esp-idf/export.sh && idf.py -B build_kase_v2 -DBOARD=kase_v2 -DSDKCONFIG=build_kase_v2/sdkconfig build` → complete.
- [ ] **Step 4: Commit** `git commit -am "feat(kbd): CONFIG_KASE_KBD_WIRELESS gate (wired boards unaffected)"`

---

### Task 5: Keyboard-side relay TX (`kbd_relay_tx.c`)

**Files:** Create `main/comm/rf/kbd_relay_tx.{c,h}`; Modify `main/input/hid_report.c`

- [ ] **Step 1:** `kbd_relay_tx.h`: `void kbd_relay_init(void);` `bool kbd_relay_active(void);` `void kbd_relay_send_kbd(uint8_t modifier, const uint8_t kb[6]);` `void kbd_relay_send_mouse(uint8_t btn, int8_t x, int8_t y, int8_t wheel);`
- [ ] **Step 2:** `kbd_relay_tx.c`: init NRF driver + pair (reuse `rf_pairing`, declare `RF_DEV_SMART_KBD`); `kbd_relay_send_*` build the HIDREPORT packet (`rf_encode_hidreport_*`) and `rf_driver_send`. `kbd_relay_active()` = paired && `CONFIG_KASE_KBD_WIRELESS`.
- [ ] **Step 3:** In `hid_report.c` `send_hid_key()` / `send_hid_kb_mouse()`, when `kbd_relay_active()`, call `kbd_relay_send_*` **instead of** the local `hid_send_*` (guard with `#if CONFIG_KASE_KBD_WIRELESS` + runtime `kbd_relay_active()`).
- [ ] **Step 4: Build** a wireless keyboard: `idf.py -B build_kase_v2d_wl -DBOARD=kase_v2_debug -DSDKCONFIG=build_kase_v2d_wl/sdkconfig build` after adding `CONFIG_KASE_KBD_WIRELESS=y` to the v2_debug defaults (or a temp). Expected: complete.
- [ ] **Step 5: Commit** `git commit -am "feat(kbd): relay HID reports over NRF when wireless (smart keyboard)"`

---

### Task 6: Dongle RX — relay HIDREPORT to USB

**Files:** Modify `main/comm/rf/rf_rx_task.c`

- [ ] **Step 1:** In `drain_radio()`, add a `PKT_TYPE_HIDREPORT` branch (only for a slot paired as `RF_DEV_SMART_KBD`):

```c
} else if (type == PKT_TYPE_HIDREPORT) {
    uint8_t sub, mod, kb[6], btn; int8_t x, y, w;
    if (rf_decode_hidreport(buf, n, &sub, &mod, kb, &btn, &x, &y, &w)) {
        if (sub == RF_HID_SUB_KBD)        hid_send_keyboard(mod, kb);
        else if (sub == RF_HID_SUB_MOUSE) hid_send_mouse(btn, x, y, w);
        changed = false;   /* no engine, no MATRIX_STATE */
    }
}
```
Include `hid_transport.h`. Do NOT run `run_engine_cycle()` for this path.

- [ ] **Step 2:** Link-loss for a smart keyboard: when its heartbeat times out, send a zero keyboard report once: `uint8_t z[6]={0}; hid_send_keyboard(0, z);` (in the existing timeout handling, gated on the slot's device type).
- [ ] **Step 3: Build** dongle: `idf.py -B build_kase_dongle ... build`. Expected: complete (note: dongle build needs the secure-boot signing key from the parked work — if it blocks, temporarily build without the secure-boot lines, since that work is parked).
- [ ] **Step 4: Commit** `git commit -am "feat(rf): dongle relays PKT_TYPE_HIDREPORT to USB HID (no engine)"`

---

### Task 7: Config bridge — dongle CDC → ESP-NOW tunnel

**Files:** Modify `main/comm/rf/cfg_bridge.c`, `main/comm/cdc/cdc_binary_protocol.c`, `main/comm/rf/kbd_relay_tx.c`

- [ ] **Step 1:** `cfg_bridge.c` (target side): `void cfg_bridge_forward(uint8_t cmd, const uint8_t *frame, uint16_t len)` — `espnow_send(smart_kbd_mac, EN_KS_FRAME, frame, len)` (chunk if `len > 240`: send `EN_KS_CHUNK` parts with a 2-byte (idx,total) header; V2D reassembles). Add `EN_KS_FRAME`/`EN_KS_CHUNK`/`EN_KR_FRAME` opcodes in `espnow_msg.h`.
- [ ] **Step 2:** In `cdc_binary_protocol.c` dispatch (around line 226), before `handler(...)`: `if (!cfg_is_dongle_local(cmd_id) && smart_kbd_paired()) { cfg_bridge_forward(cmd_id, full_frame, frame_len); return; }` — the KR comes back asynchronously via the ESP-NOW recv (Step 4) and is written to USB CDC.
- [ ] **Step 3:** V2D side (`kbd_relay_tx.c`): ESP-NOW recv of `EN_KS_FRAME`/`EN_KS_CHUNK` → reassemble → feed into the **existing** KS dispatch (`ks_*`/`find_handler`) so V2D's own handlers process it (keymap lives here) → capture the KR response → `espnow_send(dongle_mac, EN_KR_FRAME, kr, kr_len)`.
- [ ] **Step 4:** Dongle: ESP-NOW recv of `EN_KR_FRAME` → write the bytes to USB CDC (the rebind software receives the KR as if local).
- [ ] **Step 5: Build** dongle + wireless V2D. Expected: both complete.
- [ ] **Step 6: Commit** `git commit -am "feat(rf): config bridge — tunnel KS/KR over ESP-NOW (dongle↔smart keyboard)"`

---

### Task 8: Full regression + HW validation

**Files:** none (validation).

- [ ] **Step 1:** `./scripts/check.sh --host-only` green; build the affected boards.
- [ ] **Step 2: HW (manual, record in spec):** flash wireless V2D + dongle, pair (declares smart-keyboard); confirm: typing on V2D reaches host via dongle USB; split halves still work simultaneously (dual-mode); rebind software (KeSp) configures V2D *through* the dongle (SETKEY round-trips over ESP-NOW); V2D + dongle coexist (different VIDs); smart-keyboard link-loss releases keys (zero report).
- [ ] **Step 3: Commit** any doc updates.

---

## Notes for the executor

- **Dual-mode invariant:** a slot paired `RF_DEV_DUMB_HALF` drives the engine (raw matrix, unchanged); a slot paired `RF_DEV_SMART_KBD` only feeds the HIDREPORT relay + the config bridge. Never mix.
- **Security is parked** — this slice adds NO RF authentication; the HIDREPORT/ESP-NOW paths are unauthenticated (same residual risk as today's split). When the parked RF-auth lands, the HIDREPORT path takes the same trailer.
- **Dongle secure-boot is parked** — if the committed dongle config's secure-boot lines block a quick build, build against a temp sdkconfig without them (the secure-boot rollout is suspended per the pivot).
- Keep `kbd_relay_tx` and `cfg_bridge` focused; do not widen the smart keyboard's surface beyond HID-out + KS-config-in.
