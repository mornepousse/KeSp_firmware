# Plan RF-2 — Pairing Flow: PKT_PAIR_REQ/ACK, KS_CMD_RF_PAIR_START, BOOT-button Trigger, NVS Persistence

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the KaSe RF pairing rendezvous on top of Plan RF-1's `set_id`/
`rf_apply_set_id` foundation. A half transmits `PKT_PAIR_REQ` (its WiFi MAC) on the fixed
pairing channel/address; the dongle, after a controller `KS_CMD_RF_PAIR_START` CDC command,
listens on the pairing rendezvous, assigns a slot (positional: first→left, second→right),
persists the half's MAC to NVS, and replies `PKT_PAIR_ACK` ({set_id, dongle_mac, slot}). The
dongle hot-switches its radios to the derived per-set address/channel WITHOUT rebooting (USB
stays up); the half persists `set_id`/`slot`/`mac_dongle` to NVS and reboots to apply. Pure
bits (PKT_PAIR_* encode/decode, slot-assignment logic) are host-tested first (TDD). End state:
all three RF boards build green; host tests stay at 0 failed and grow.

**Architecture:** Five additions, all reusing Plan RF-1's `rf_pairing.{c,h}` and the spec's
exact byte layouts. The pairing state machine runs inside the existing `rf_rx_task` (no new
task) via a `pairing_mode` flag checked at the top of the RX loop. The dongle's PKT_PAIR_ACK
TX uses a self-contained raw-register PTX burst in `rf_pairing.c` (the dongle build does NOT
compile `rf_driver_send`, which is `KASE_HAS_RF_TX`-only — so the ACK TX is implemented locally
against the existing register helpers).

```
Controller (USB)     Dongle (rf_rx_task)        Pairing ch/addr (NRF)         Half (half_scan_task)
──────────────────   ────────────────────────   ───────────────────────────   ─────────────────────
KS_CMD_RF_PAIR_START                                                           BOOT (GPIO0) held 3 s
 {reset:u8}    ─────► bin_cmd_rf_pair_start
                      [reset=1 → NVS clear]
                      pairing_mode = true                                      pairing_mode = true
                      window_deadline = now+30s                                window deadline = now+30s
                      L radio → PAIR ch+addr PRX                               radio → PAIR ch+addr PTX
                      ◄── KR_OK {set_id, paired_count}                         REQ loop @200ms:
                                                 ◄── PKT_PAIR_REQ {half_mac} ── PKT_PAIR_REQ
                      slot = paired_count+1
                      dedup vs stored mac_*
                      NVS: mac_left|mac_right
                           paired_count++
                      L radio → PAIR PTX
                                                 ── PKT_PAIR_ACK ────────────► decode ACK
                                                    {set_id,dongle_mac,slot}    NVS: set_id, slot,
                      L radio → PAIR PRX                                              mac_dongle
                      (await 2nd half or 30s)                                  stop REQ loop
                      on close / paired:                                       vTaskDelay 2s
                        hot-switch BOTH radios →                               esp_restart()
                        derived per-set addr+ch                               (re-init applies RF-1
                        (rf_apply_set_id + reprogram regs)                     derivation from NVS)
                        pairing_mode = false
```

**Tech Stack:** ESP-IDF 5.5, `esp_mac.h` (`esp_read_mac`), `nvs.h`, `driver/gpio.h`
(BOOT GPIO0 read), `esp_timer.h`, FreeRTOS, Plan RF-1's `rf_pairing.{c,h}` and existing
`rf_driver.{c,h}` register helpers. Pure encode/decode + slot logic compile under `-DTEST_HOST`.

**Spec reference:** `docs/superpowers/specs/2026-05-22-rf-pairing-addressing-design.md` —
sections 3.1–3.6, 4, 5.4, 5.5, 5.6, 10.1. Read in full before touching any file.

**Depends on:** Plan RF-1 merged and build-green. RF-2 reuses RF-1's EXACT API — do NOT change
these signatures:
```c
bool     rf_derive_addr(uint16_t set_id, uint8_t slot, uint8_t addr_out[4],
                        uint8_t *suffix_out, uint8_t *channel_out);   /* pure */
uint8_t  rf_derive_wifi_ch(uint16_t set_id);                          /* pure */
uint16_t crc16_ccitt(const uint8_t *data, uint32_t len);             /* pure */
uint16_t rf_compute_set_id(void);                                    /* firmware */
void     rf_apply_set_id(rf_radio_cfg_t *cfg, uint16_t set_id, uint8_t slot); /* firmware */
uint16_t rf_pairing_load_set_id_dongle(void);                        /* firmware */
uint16_t rf_pairing_load_set_id_half(uint8_t fallback_slot, uint8_t *slot_out); /* firmware */
```
RF-2 ADDS to `rf_pairing.{c,h}` and `rf_packet.h`; it does not modify RF-1's functions.

**Build / host-test commands:** identical to Plan RF-1.

```bash
# Build (rm sdkconfig when switching boards; run from repo root):
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_<target> -DBOARD=kase_<target> -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
# Targets: build_dongle/kase_dongle, build_half_left/kase_half_left, build_half_right/kase_half_right

# Host tests (baseline after RF-1: ~900 pass, 0 failed):
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner
```

---

## Baked-in facts (read before touching any file)

1. **PKT type nibbles (spec §3.3):** `PKT_TYPE_PAIR_REQ = 0xF` (already a comment-reserved
   value, but NOT currently `#define`d in `rf_packet.h` — RF-2 adds it), `PKT_TYPE_PAIR_ACK =
   0xE` (new). The existing `rf_packet.h` defines `PKT_TYPE_KEY=0x1`, `PKT_TYPE_HEARTBEAT=0x2`,
   `PKT_TYPE_TRACKPAD=0x3`. The high nibble of byte 0 is the type; `rf_packet_type()` already
   extracts it. The pair packets carry flags `0x0` in the low nibble.

2. **PKT_PAIR_REQ wire format (spec §3.3):** 7 bytes. `buf[0] = 0xF0`, `buf[1..6] =
   half_wifi_mac[6]`. Sent by the half.

3. **PKT_PAIR_ACK wire format (spec §3.3):** 10 bytes. `buf[0] = 0xE0`, `buf[1..2] = set_id`
   BIG-ENDIAN (hi first), `buf[3..8] = dongle_wifi_mac[6]`, `buf[9] = slot` (0x01/0x02). Sent
   by the dongle.

4. **The encode/decode helpers are `static inline` in `rf_packet.h` (spec §5.5).** They use
   only `<stdint.h>`/`<stdbool.h>`/`<string.h>` (already included). They are host-testable via
   `test_rf_packet.c` which already includes `rf_packet.h`. Add the four helpers exactly as in
   spec §5.5: `rf_encode_pair_req`, `rf_encode_pair_ack`, `rf_decode_pair_req`,
   `rf_decode_pair_ack`, plus the two type `#define`s and the `rf_pair_ack_t` struct.

5. **`rf_pair_ack_t` struct (spec §5.5):** `{ uint16_t set_id; uint8_t dongle_wifi_mac[6];
   uint8_t slot; }`. `rf_pair_req_t` is `{ uint8_t half_wifi_mac[6]; }` but the decode helper
   writes into a caller `uint8_t mac_out[6]` rather than the struct — match the spec's
   `rf_decode_pair_req(buf, len, mac_out)` signature.

6. **Slot assignment is positional (spec §3.4):** `paired_count = 0` → assign `0x01` (left);
   `paired_count = 1` → assign `0x02` (right); `paired_count = 2` → window closed. Extract this
   into a PURE function `rf_pairing_assign_slot(uint8_t paired_count, uint8_t *slot_out)`
   returning `false` when `paired_count >= 2`. Host-testable, lives OUTSIDE `#ifndef TEST_HOST`
   in `rf_pairing.c`.

7. **Dedup (spec §3.5):** if an incoming REQ's MAC already equals a stored `mac_left` /
   `mac_right`, the dongle should re-ACK with the SAME slot (so a half that missed the first
   ACK and re-sends gets the same assignment) WITHOUT incrementing `paired_count`. Extract a
   pure helper `rf_pairing_match_slot(const uint8_t *mac, const uint8_t *mac_left, const
   uint8_t *mac_right, uint8_t *slot_out)` returning true if the MAC matches an already-stored
   peer (slot 0x01 or 0x02). Host-testable, pure.

8. **The dongle build does NOT compile `rf_driver_send` / `rf_driver_init_tx`.** Those are
   gated on `CONFIG_KASE_HAS_RF_TX` (half only). The dongle is `CONFIG_KASE_HAS_RF_RX`. So the
   PKT_PAIR_ACK transmission on the dongle CANNOT call `rf_driver_send`. Implement a local
   raw-register PTX burst in `rf_pairing.c` (firmware section) using the EXISTING exposed
   helpers from `rf_driver.h`: `rf_driver_read_reg`, `rf_driver_write_reg`,
   `rf_driver_set_channel`. For multi-byte address/payload writes and CE control, RF-2 adds two
   thin helpers to `rf_driver.{c,h}` (compiled for the RX role too): `rf_driver_write_reg_buf`
   (exposes the existing static `write_reg_buf`) and `rf_driver_pair_ack_tx` is REJECTED (keeps
   too much logic in the driver). Final decision: see Fact 9.

9. **FINAL dongle-ACK-TX decision (authoritative — implement this).** Add ONE new function to
   `rf_driver.{c,h}`, compiled in BOTH roles (it has no `KASE_HAS_RF_TX` guard):

   ```c
   /* Out-of-band one-shot PTX on an arbitrary channel+5-byte address, used only
    * by the dongle pairing flow to send PKT_PAIR_ACK. Switches the radio to PTX,
    * transmits once (CE pulse), polls TX_DS/MAX_RT, then restores PRX on `restore_ch`
    * + `restore_addr` and re-asserts CE high (resume listening). Returns true on TX_DS.
    * The caller must NOT hold an RX read in flight (serialize via pairing_mode). */
   bool rf_driver_oob_tx(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                         const uint8_t *payload, uint8_t len,
                         uint8_t restore_ch, const uint8_t restore_addr[5]);
   ```

   This encapsulates the spec §5.4 sequence (a→i) in the driver where the register helpers and
   CE control already live. `rf_pairing.c` calls it. The function is the cleanest home because
   `ce_low`/`ce_high`/`csn_*`/`spi_xfer`/`write_reg_buf` are all file-static in `rf_driver.c`.
   Register sequence implemented inside it (mirrors `rf_driver_send` + `rf_driver_init` PRX
   restore):
   - `ce_low(r)`; set channel `ch`; write `REG_TX_ADDR` = addr (5B); write `REG_RX_ADDR_P0` =
     addr (5B); `REG_CONFIG = 0x3E` (PTX power-up, EN_CRC|CRCO, PRIM_RX=0); delay 2 ms.
   - `REG_STATUS = 0x70`; FLUSH_TX; W_TX_PAYLOAD; CE pulse 15 µs; poll STATUS for TX_DS/MAX_RT
     (5 ms deadline); FLUSH_TX on MAX_RT; clear STATUS 0x30.
   - Restore: `ce_low`; set channel `restore_ch`; write `REG_RX_ADDR_P0` = restore_addr (5B);
     `REG_CONFIG = 0x3F` (PRX power-up, PRIM_RX=1); delay 2 ms; `REG_STATUS = 0x70`; FLUSH_RX;
     `ce_high` (resume listening).

10. **Pairing rendezvous constants (spec §3.2):** add to `rf_pairing.h` (host-safe section is
    fine — they are plain macros):
    ```c
    #define RF_PAIR_CHANNEL  0x28                          /* 40 dec, 2440 MHz */
    #define RF_PAIR_ADDR     { 'K', 'S', 'P', 'R', 0xFF }  /* 5 bytes */
    ```

11. **Half BOOT button = GPIO0 (active-low).** The ESP32-S3 DevKitC BOOT button is on GPIO0,
    pulled high, reads 0 when pressed. The half firmware has NO existing button handling — RF-2
    adds it. Configure GPIO0 as input with internal pull-up; poll it. The half is event-driven
    (`half_scan_task` blocks on `portMAX_DELAY`), so add a small polling loop in a new
    low-priority task, OR poll inside the heartbeat timer callback (100 ms tick — convenient,
    no new task). DECISION (adopted): poll in `heartbeat_timer_cb` — it already runs every
    100 ms. Track consecutive-low ticks; at 30 ticks (~3 s held) trigger pairing. This avoids a
    new task and the 100 ms granularity is fine for a 3 s hold. `gpio_get_level` is ISR-safe
    and timer-callback-safe.

12. **Half pairing must run off the timer/callback context, not block it.** The REQ loop +
    ACK-wait is a multi-second operation. Spawn a dedicated `half_pairing_task` (prio 4, stack
    4096, core 0) when the 3 s hold completes; the timer callback only sets a flag / creates
    the task once (guard with a static `s_pairing_active` bool so a held button does not spawn
    repeatedly). The pairing task switches the radio to PAIR ch/addr (it owns the radio for the
    window; the half's normal TX is paused — acceptable, the user initiated pairing). It uses
    `half_spi_lock()` around every NRF transaction (the SPI2 bus lock from Plan Bricks-1).

13. **Half radio reuse:** the half already has `rf_radio_t s_radio` (non-static, extern-shared
    with trackpad). The pairing task transmits PKT_PAIR_REQ via the EXISTING `rf_driver_send`
    after switching `s_radio` to the PAIR address/channel — but `rf_driver_send` uses the
    radio's CURRENTLY-PROGRAMMED TX_ADDR. To retarget, call a new
    `rf_driver_set_tx_address(&s_radio, RF_PAIR_ADDR)` + `rf_driver_set_channel(&s_radio,
    RF_PAIR_CHANNEL)` before the REQ loop. Add `rf_driver_set_tx_address` to `rf_driver.{c,h}`
    inside the `CONFIG_KASE_HAS_RF_TX` guard (it writes `REG_TX_ADDR` + `REG_RX_ADDR_P0` = addr,
    5B, with CE low). For receiving the ACK, the half must briefly enter PRX on the PAIR addr:
    add `rf_driver_recv_oob` is REJECTED (too heavy). Final decision: see Fact 14.

14. **FINAL half-ACK-RX decision (authoritative — implement this).** The half listens for the
    ACK by switching to PRX on the PAIR address between REQ bursts. Add ONE function to
    `rf_driver.{c,h}` inside `#if CONFIG_KASE_HAS_RF_TX`:

    ```c
    /* Pairing-only: switch the (normally-PTX) half radio to PRX on the given
     * channel+5-byte address, wait up to timeout_ms for one RX payload, copy it to
     * buf (max maxlen), return its length (0 on timeout). Leaves the radio in PRX —
     * the caller switches back to PTX (set_tx_address) for the next REQ burst. */
    uint16_t rf_driver_pair_listen(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                                   uint8_t *buf, uint16_t maxlen, uint32_t timeout_ms);
    ```

    Internals: `ce_low`; set channel; `REG_RX_ADDR_P0` = addr (5B); `REG_CONFIG = 0x3F` (PRX);
    delay 2 ms; `ce_high`; poll `rf_driver_rx_available` until timeout; on data, `rf_driver_read_rx`;
    `ce_low`. The half's pairing task alternates: set_tx_address+send REQ, then pair_listen 150 ms
    for the ACK, repeat for the 30 s window.

15. **NVS schema (spec §4.1) — namespace `rf` (`RF_STORAGE_NAMESPACE` from RF-1):**
    Dongle keys: `mac_left` (blob 6B), `mac_right` (blob 6B), `paired_count` (u8).
    Half keys: `set_id` (u16), `slot` (u8), `mac_dongle` (blob 6B).
    MAC blobs are RAW 6-byte arrays (Plan 3 reads them as ESP-NOW peer MACs — keep exactly
    6-byte blobs under exactly these key names). All-zero MAC = "not paired" sentinel.

16. **CDC command (spec §3.1, §5.6):** `KS_CMD_RF_PAIR_START` is added to the `ks_cmd_id_t`
    enum in `cdc_binary_protocol.h` in the Diagnostics range (0xB0-0xBF) — use `0xB2` (0xB0
    MATRIX_TEST, 0xB1 NVS_RESET are taken). The handler `bin_cmd_rf_pair_start` is added to
    `cdc_binary_cmds.c`'s `bin_cmd_table`, GUARDED so it only registers/compiles on the dongle
    (`#if CONFIG_KASE_HAS_RF_RX`) — the handler calls into dongle-only `rf_rx_*` pairing entry
    points. Request: `[reset:u8]`. Response: `KR_OK` with `[set_id_hi, set_id_lo, paired_count]`
    (3 bytes), returned immediately once the window starts (async — the exchange runs in
    `rf_rx_task`).

17. **Dongle pairing entry point:** add `bool rf_rx_pair_start(uint8_t reset, uint16_t
    *set_id_out, uint8_t *paired_count_out)` to `rf_rx_task.{c,h}`. It (a) on `reset` clears
    NVS `mac_left`/`mac_right`/`paired_count`; (b) loads current `paired_count`; (c) sets the
    `pairing_mode` flag + 30 s deadline + switches radio L to PAIR ch/addr PRX; (d) returns
    `set_id = rf_compute_set_id()` and `paired_count` for the CDC response. The `rf_rx_task`
    loop, when `pairing_mode`, runs the pairing state machine on radio L instead of normal RX.

18. **Dongle hot-switch (spec §3.5, the key ambiguity — resolved here):** when the window
    closes (both halves paired OR 30 s elapsed), the dongle must move BOTH radios to the
    derived per-set address/channel WITHOUT rebooting (USB HID/CDC must stay up). Sequence,
    run in `rf_rx_task` when `pairing_mode` transitions to false:
    - `set_id = rf_compute_set_id()` (or 0 if `paired_count == 0` → stay factory).
    - Rebuild each cfg from `board_rf_left_cfg()`/`board_rf_right_cfg()`, `rf_apply_set_id`
      (slot 0x01 / 0x02), then reprogram the live radios via a new
      `rf_driver_set_rx_address(&s_left, addr5)` + `rf_driver_set_channel(&s_left, ch)` (and
      same for right). `rf_driver_set_rx_address` is added to `rf_driver.{c,h}` (RX-safe, no
      guard): `ce_low`; `REG_RX_ADDR_P0` = addr (5B); `ce_high`. This is the minimal live
      reprogram — channel + RX address are the only per-set fields. The radios stay powered/PRX
      throughout; only CE toggles briefly.
    - Note: radio L was on the PAIR address during the window; the hot-switch reprograms it to
      the derived per-set address, ending pairing cleanly.

19. **Why dongle hot-switch but half reboot (spec §3.5):** the dongle is USB-tethered; a reboot
    would drop the HID/CDC link to the host. The half has no runtime address-switch path in its
    normal data flow and re-derives from NVS at boot via RF-1's wiring — `esp_restart()` is the
    simplest correct apply. The half pairing task does `vTaskDelay(pdMS_TO_TICKS(2000))` then
    `esp_restart()` after writing NVS + receiving the ACK.

20. **clangd false positives:** ignore IDE missing-header errors; trust `idf.py build`.

---

## Packet & State Reference (from spec §3.2, §3.3, §3.4, §3.5)

```
RF_PAIR_CHANNEL = 0x28 (40, 2440 MHz)        below WiFi ch1 and the 80+ data pool
RF_PAIR_ADDR    = { 'K','S','P','R', 0xFF }   KaSe Pairing, 0xFF suffix

PKT_PAIR_REQ (half→dongle, 7 B):
  [0]   0xF0
  [1-6] half_wifi_mac

PKT_PAIR_ACK (dongle→half, 10 B):
  [0]   0xE0
  [1-2] set_id  (big-endian, hi first)
  [3-8] dongle_wifi_mac
  [9]   slot (0x01 left / 0x02 right)

Slot assignment: paired_count 0→0x01, 1→0x02, 2→closed.
Dedup: REQ.mac == stored mac_left → re-ACK slot 0x01 (no count++); == mac_right → slot 0x02.
```

---

## File Structure

### Modified

| Path | Change |
|---|---|
| `main/comm/rf/rf_packet.h` | Add `PKT_TYPE_PAIR_REQ/ACK`, `rf_pair_ack_t`, 4 encode/decode `static inline` helpers (spec §5.5) |
| `main/comm/rf/rf_pairing.h` | Add `RF_PAIR_CHANNEL`/`RF_PAIR_ADDR`, pure `rf_pairing_assign_slot`/`rf_pairing_match_slot`, firmware NVS-write helpers |
| `main/comm/rf/rf_pairing.c` | Implement the new pure + firmware helpers (NVS writes, MAC loads) |
| `main/comm/rf/rf_driver.h` | Declare `rf_driver_oob_tx`, `rf_driver_set_rx_address` (both roles); `rf_driver_set_tx_address`, `rf_driver_pair_listen` (RF_TX only) |
| `main/comm/rf/rf_driver.c` | Implement the four new register/CE helpers |
| `main/comm/rf/rf_rx_task.h` | Declare `rf_rx_pair_start` |
| `main/comm/rf/rf_rx_task.c` | `pairing_mode` flag + state machine in the RX loop; `rf_rx_pair_start`; hot-switch on window close |
| `main/comm/rf/half_scan_task.c` | BOOT-button poll in `heartbeat_timer_cb`; `half_pairing_task` (REQ loop + ACK wait + NVS + reboot) |
| `main/comm/cdc/cdc_binary_protocol.h` | Add `KS_CMD_RF_PAIR_START = 0xB2` to `ks_cmd_id_t` |
| `main/comm/cdc/cdc_binary_cmds.c` | Add `bin_cmd_rf_pair_start` + table entry, guarded `#if CONFIG_KASE_HAS_RF_RX` |
| `test/test_rf_packet.c` | Add PKT_PAIR_REQ/ACK round-trip + negative cases |
| `test/test_rf_pairing.c` | Add `rf_pairing_assign_slot` + `rf_pairing_match_slot` cases |

### Untouched

```
main/comm/rf/rf_pairing.{c,h} RF-1 functions      not modified — only ADDED to
boards/*/board.h, board_rf.h                       factory defaults unchanged
main/sys/nvs_utils.{c,h}                            pairing uses direct nvs_set_* (6B blob / u8 / u16)
sdkconfig                                           rm before each board switch, never commit
```

---

## Task 1: PKT_PAIR_REQ/ACK encode/decode in `rf_packet.h` + host tests (TDD)

**Rationale:** The wire format is the contract between half and dongle. Pin it with host tests
first. `rf_packet.h`'s helpers are `static inline` and already host-tested in
`test_rf_packet.c`.

**Files:**
- Modify: `main/comm/rf/rf_packet.h`
- Modify: `test/test_rf_packet.c`

### Step 1.1 — Read the existing packet helpers and types

```bash
sed -n '8,12p;37,45p;79,86p' /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/rf_packet.h
```

Confirm `PKT_TYPE_KEY/HEARTBEAT/TRACKPAD` are defined and `rf_packet_type()` exists.

### Step 1.2 — Add the type defines + struct + helpers to `rf_packet.h`

After the existing `#define PKT_TYPE_TRACKPAD 0x3` line, add:

```c
#define PKT_TYPE_PAIR_ACK   0xE   /* dongle→half pairing ACK (RF-2) */
#define PKT_TYPE_PAIR_REQ   0xF   /* half→dongle pairing request (RF-2) */
```

After the `rf_trackpad_t` struct, add:

```c
typedef struct {
    uint16_t set_id;              /* host order; encoded big-endian on wire */
    uint8_t  dongle_wifi_mac[6];
    uint8_t  slot;               /* 0x01=left, 0x02=right */
} rf_pair_ack_t;
```

After `rf_encode_trackpad`, add the encoders:

```c
/* PKT_PAIR_REQ: 7 bytes — type 0xF, then the half's 6-byte WiFi STA MAC. */
static inline uint16_t rf_encode_pair_req(uint8_t *buf, const uint8_t mac[6])
{
    buf[0] = (PKT_TYPE_PAIR_REQ << 4);
    memcpy(buf + 1, mac, 6);
    return 7;
}

/* PKT_PAIR_ACK: 10 bytes — type 0xE, set_id big-endian, dongle MAC, slot. */
static inline uint16_t rf_encode_pair_ack(uint8_t *buf, const rf_pair_ack_t *a)
{
    buf[0] = (PKT_TYPE_PAIR_ACK << 4);
    buf[1] = (uint8_t)(a->set_id >> 8);
    buf[2] = (uint8_t)(a->set_id & 0xFF);
    memcpy(buf + 3, a->dongle_wifi_mac, 6);
    buf[9] = a->slot;
    return 10;
}
```

After `rf_decode_trackpad`, add the decoders:

```c
static inline bool rf_decode_pair_req(const uint8_t *buf, uint16_t len, uint8_t mac_out[6])
{
    if (len < 7 || rf_packet_type(buf, len) != PKT_TYPE_PAIR_REQ) return false;
    memcpy(mac_out, buf + 1, 6);
    return true;
}

static inline bool rf_decode_pair_ack(const uint8_t *buf, uint16_t len, rf_pair_ack_t *a)
{
    if (len < 10 || rf_packet_type(buf, len) != PKT_TYPE_PAIR_ACK) return false;
    a->set_id = ((uint16_t)buf[1] << 8) | buf[2];
    memcpy(a->dongle_wifi_mac, buf + 3, 6);
    a->slot = buf[9];
    return true;
}
```

### Step 1.3 — Add host tests to `test/test_rf_packet.c`

Open `test/test_rf_packet.c`, find the `void test_rf_packet(void)` body, and add (before its
closing brace; reuse the existing `TEST_SUITE` already opened in that function):

```c
    /* ── PKT_PAIR_REQ round-trip ──────────────────────────────── */
    {
        const uint8_t mac[6] = {0x24,0x6F,0x28,0xAA,0xBB,0xCC};
        uint8_t buf[7];
        uint16_t n = rf_encode_pair_req(buf, mac);
        TEST_ASSERT_EQ(n, 7,                "pair_req: encodes 7 bytes");
        TEST_ASSERT_EQ(buf[0], 0xF0,        "pair_req: byte0 = 0xF0");
        TEST_ASSERT_EQ(rf_packet_type(buf,n), PKT_TYPE_PAIR_REQ, "pair_req: type 0xF");
        uint8_t out[6] = {0};
        TEST_ASSERT(rf_decode_pair_req(buf, n, out), "pair_req: decode ok");
        TEST_ASSERT_EQ(memcmp(out, mac, 6), 0,       "pair_req: mac round-trips");
    }

    /* ── PKT_PAIR_ACK round-trip (big-endian set_id) ──────────── */
    {
        rf_pair_ack_t a = { .set_id = 0x1234,
                            .dongle_wifi_mac = {0x10,0x20,0x30,0x40,0x50,0x60},
                            .slot = 0x02 };
        uint8_t buf[10];
        uint16_t n = rf_encode_pair_ack(buf, &a);
        TEST_ASSERT_EQ(n, 10,               "pair_ack: encodes 10 bytes");
        TEST_ASSERT_EQ(buf[0], 0xE0,        "pair_ack: byte0 = 0xE0");
        TEST_ASSERT_EQ(buf[1], 0x12,        "pair_ack: set_id hi first (BE)");
        TEST_ASSERT_EQ(buf[2], 0x34,        "pair_ack: set_id lo second");
        TEST_ASSERT_EQ(buf[9], 0x02,        "pair_ack: slot in byte9");
        rf_pair_ack_t d;
        TEST_ASSERT(rf_decode_pair_ack(buf, n, &d), "pair_ack: decode ok");
        TEST_ASSERT_EQ(d.set_id, 0x1234,    "pair_ack: set_id round-trips");
        TEST_ASSERT_EQ(d.slot, 0x02,        "pair_ack: slot round-trips");
        TEST_ASSERT_EQ(memcmp(d.dongle_wifi_mac, a.dongle_wifi_mac, 6), 0,
                                            "pair_ack: dongle mac round-trips");
    }

    /* ── Negative: wrong type / short buffer ──────────────────── */
    {
        uint8_t bad[10] = {0x10};   /* type 0x1 (KEY), not pair */
        uint8_t out[6];
        rf_pair_ack_t d;
        TEST_ASSERT(!rf_decode_pair_req(bad, 7, out), "pair_req: wrong type rejected");
        TEST_ASSERT(!rf_decode_pair_ack(bad, 10, &d), "pair_ack: wrong type rejected");
        uint8_t shortbuf[5] = {0xF0};
        TEST_ASSERT(!rf_decode_pair_req(shortbuf, 5, out), "pair_req: short buf rejected");
        uint8_t shortack[9] = {0xE0};
        TEST_ASSERT(!rf_decode_pair_ack(shortack, 9, &d),  "pair_ack: short buf rejected");
    }
```

### Step 1.4 — Run host tests

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -10
./test_runner 2>&1 | grep -E "rf_packet|FAIL|passed|failed"
```

Expected: new pair cases pass, `0 failed`, count grows.

### Step 1.5 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/rf_packet.h test/test_rf_packet.c
git commit -m "feat(rf): PKT_PAIR_REQ/ACK encode+decode + host tests (TDD)"
```

---

## Task 2: Pure pairing logic in `rf_pairing` (slot assignment + MAC match) + host tests (TDD)

**Files:**
- Modify: `main/comm/rf/rf_pairing.h`
- Modify: `main/comm/rf/rf_pairing.c`
- Modify: `test/test_rf_pairing.c`

### Step 2.1 — Add pairing constants + pure declarations to `rf_pairing.h`

In the HOST-SAFE section (before `#ifndef TEST_HOST`), after the RF-1 pure declarations, add:

```c
/* ── Pairing rendezvous (fixed, immutable — spec §3.2) ─────────── */
#define RF_PAIR_CHANNEL  0x28                          /* 40 dec, 2440 MHz */
#define RF_PAIR_ADDR     { 'K', 'S', 'P', 'R', 0xFF }  /* 5 bytes */

/* Positional slot assignment (spec §3.4). paired_count 0→0x01, 1→0x02.
 * Returns false (window full) when paired_count >= 2. Pure. */
bool rf_pairing_assign_slot(uint8_t paired_count, uint8_t *slot_out);

/* Dedup: if `mac` equals an already-stored peer, return its slot. All-zero
 * stored MACs are treated as empty (no match). Returns false if no match. Pure. */
bool rf_pairing_match_slot(const uint8_t mac[6],
                           const uint8_t mac_left[6], const uint8_t mac_right[6],
                           uint8_t *slot_out);
```

### Step 2.2 — Add the firmware NVS-write declarations to `rf_pairing.h`

In the `#ifndef TEST_HOST` section (after the RF-1 firmware declarations), add:

```c
/* Dongle: persist a paired half's MAC into the slot's NVS key and bump
 * paired_count. slot 0x01 → "mac_left", 0x02 → "mac_right". Returns ESP_OK. */
esp_err_t rf_pairing_save_peer_dongle(uint8_t slot, const uint8_t mac[6],
                                      uint8_t new_paired_count);

/* Dongle: clear mac_left/mac_right/paired_count (re-pair reset). */
esp_err_t rf_pairing_reset_dongle(void);

/* Dongle: load mac_left + mac_right (6B each; zeroed if absent) and paired_count. */
void rf_pairing_load_peers_dongle(uint8_t mac_left[6], uint8_t mac_right[6],
                                  uint8_t *paired_count);

/* Half: persist set_id + slot + dongle MAC after receiving PKT_PAIR_ACK. */
esp_err_t rf_pairing_save_half(uint16_t set_id, uint8_t slot, const uint8_t mac_dongle[6]);
```

`esp_err_t` is from `esp_err.h`, already pulled in via `rf_driver.h` inside the guard.

### Step 2.3 — Implement the pure helpers in `rf_pairing.c` (outside TEST_HOST)

After `rf_derive_wifi_ch` (still OUTSIDE `#ifndef TEST_HOST`), add:

```c
/* ── Pure: positional slot assignment ──────────────────────────── */
bool rf_pairing_assign_slot(uint8_t paired_count, uint8_t *slot_out)
{
    if (paired_count == 0) { *slot_out = 0x01; return true; }
    if (paired_count == 1) { *slot_out = 0x02; return true; }
    return false;   /* window full */
}

/* ── Pure: dedup against already-stored peers ──────────────────── */
static bool mac_is_zero(const uint8_t m[6])
{
    for (int i = 0; i < 6; i++) if (m[i] != 0) return false;
    return true;
}

bool rf_pairing_match_slot(const uint8_t mac[6],
                           const uint8_t mac_left[6], const uint8_t mac_right[6],
                           uint8_t *slot_out)
{
    int i;
    if (!mac_is_zero(mac_left)) {
        for (i = 0; i < 6 && mac[i] == mac_left[i]; i++) {}
        if (i == 6) { *slot_out = 0x01; return true; }
    }
    if (!mac_is_zero(mac_right)) {
        for (i = 0; i < 6 && mac[i] == mac_right[i]; i++) {}
        if (i == 6) { *slot_out = 0x02; return true; }
    }
    return false;
}
```

### Step 2.4 — Implement the firmware NVS helpers in `rf_pairing.c` (inside TEST_HOST guard)

In the `#ifndef TEST_HOST` section (after the RF-1 NVS loaders), add:

```c
esp_err_t rf_pairing_save_peer_dongle(uint8_t slot, const uint8_t mac[6],
                                      uint8_t new_paired_count)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(RF_STORAGE_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    const char *key = (slot == 0x01) ? "mac_left" : "mac_right";
    e = nvs_set_blob(h, key, mac, 6);
    if (e == ESP_OK) e = nvs_set_u8(h, "paired_count", new_paired_count);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved %s, paired_count=%u (err=%d)", key, new_paired_count, e);
    return e;
}

esp_err_t rf_pairing_reset_dongle(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(RF_STORAGE_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_key(h, "mac_left");    /* ESP_ERR_NVS_NOT_FOUND is harmless */
    nvs_erase_key(h, "mac_right");
    nvs_set_u8(h, "paired_count", 0);
    e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "pairing reset (mac_left/right cleared, paired_count=0)");
    return e;
}

void rf_pairing_load_peers_dongle(uint8_t mac_left[6], uint8_t mac_right[6],
                                  uint8_t *paired_count)
{
    memset(mac_left, 0, 6);
    memset(mac_right, 0, 6);
    *paired_count = 0;
    nvs_handle_t h;
    if (nvs_open(RF_STORAGE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = 6; nvs_get_blob(h, "mac_left",  mac_left,  &sz);
    sz = 6;        nvs_get_blob(h, "mac_right", mac_right, &sz);
    nvs_get_u8(h, "paired_count", paired_count);
    nvs_close(h);
}

esp_err_t rf_pairing_save_half(uint16_t set_id, uint8_t slot, const uint8_t mac_dongle[6])
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(RF_STORAGE_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u16(h, "set_id", set_id);
    if (e == ESP_OK) e = nvs_set_u8(h, "slot", slot);
    if (e == ESP_OK) e = nvs_set_blob(h, "mac_dongle", mac_dongle, 6);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "half saved set_id=0x%04X slot=0x%02X (err=%d)", set_id, slot, e);
    return e;
}
```

`memset`/`memcmp` need `<string.h>` — add `#include <string.h>` inside the `#ifndef TEST_HOST`
include block of `rf_pairing.c` if not already present. `mac_is_zero` (pure section) and the
match loop use no string.h. Confirm `string.h` is available to the pure section too if you used
`memcmp` — the implementation above avoids `memcmp` in the pure section (manual loop) so the
pure section needs nothing.

### Step 2.5 — Add host tests to `test/test_rf_pairing.c`

In `void test_rf_pairing(void)` (before its closing brace), add:

```c
    /* ── Slot assignment (positional) ─────────────────────────── */
    uint8_t aslot;
    TEST_ASSERT(rf_pairing_assign_slot(0, &aslot),  "assign: count0 → ok");
    TEST_ASSERT_EQ(aslot, 0x01,                     "assign: count0 → slot 0x01");
    TEST_ASSERT(rf_pairing_assign_slot(1, &aslot),  "assign: count1 → ok");
    TEST_ASSERT_EQ(aslot, 0x02,                     "assign: count1 → slot 0x02");
    TEST_ASSERT(!rf_pairing_assign_slot(2, &aslot), "assign: count2 → window full");

    /* ── MAC dedup ────────────────────────────────────────────── */
    const uint8_t ml[6] = {1,2,3,4,5,6};
    const uint8_t mr[6] = {7,8,9,10,11,12};
    const uint8_t zero[6] = {0};
    uint8_t mslot;
    TEST_ASSERT(rf_pairing_match_slot(ml, ml, mr, &mslot), "match: left mac → ok");
    TEST_ASSERT_EQ(mslot, 0x01,                            "match: left mac → slot 0x01");
    TEST_ASSERT(rf_pairing_match_slot(mr, ml, mr, &mslot), "match: right mac → ok");
    TEST_ASSERT_EQ(mslot, 0x02,                            "match: right mac → slot 0x02");
    const uint8_t mx[6] = {99,99,99,99,99,99};
    TEST_ASSERT(!rf_pairing_match_slot(mx, ml, mr, &mslot),"match: unknown mac → no match");
    /* all-zero stored slots are 'empty' → never match even a zero query */
    TEST_ASSERT(!rf_pairing_match_slot(zero, zero, zero, &mslot),
                                                            "match: empty store → no match");
```

### Step 2.6 — Run host tests

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -10
./test_runner 2>&1 | grep -E "rf_pairing|FAIL|passed|failed"
```

Expected: new slot/match cases pass; `0 failed`.

Common failure modes:
- `undefined reference to rf_pairing_assign_slot` → the pure helpers must be OUTSIDE
  `#ifndef TEST_HOST` in `rf_pairing.c`.

### Step 2.7 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/rf_pairing.h main/comm/rf/rf_pairing.c test/test_rf_pairing.c
git commit -m "feat(rf): pure slot assignment + MAC dedup + pairing NVS helpers (TDD)"
```

---

## Task 3: Driver OOB/PRX helpers for pairing (`rf_driver.{c,h}`)

**Files:**
- Modify: `main/comm/rf/rf_driver.h`
- Modify: `main/comm/rf/rf_driver.c`

### Step 3.1 — Read the existing register/CE helpers and PRX/PTX register sequences

```bash
sed -n '24,45p;77,80p;150,179p;242,326p' /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/rf_driver.c
```

Confirm: `ce_low`/`ce_high`/`csn_low`/`csn_high`/`spi_xfer`/`write_reg_buf` are file-static;
`REG_CONFIG`/`REG_STATUS`/`REG_TX_ADDR`(0x10, only in RF_TX block)/`REG_RX_ADDR_P0`(0x0A);
PRX power-up `CONFIG=0x3F`; PTX power-up `CONFIG=0x3E`; `CMD_FLUSH_TX`=0xE1/`CMD_FLUSH_RX`.

### Step 3.2 — Declare the new functions in `rf_driver.h`

Both-roles helpers (place after `rf_driver_set_channel` declaration, before the
`#if CONFIG_KASE_HAS_RF_TX` block):

```c
/* Reprogram the live PRX pipe-0 RX address (5 bytes) without re-init.
 * ce_low → write REG_RX_ADDR_P0 → ce_high. Used by the dongle pairing hot-switch. */
void rf_driver_set_rx_address(rf_radio_t *r, const uint8_t addr[5]);

/* Out-of-band one-shot PTX for the dongle PKT_PAIR_ACK (spec §5.4).
 * Switches to PTX on ch+addr, transmits payload once (CE pulse, poll TX_DS/MAX_RT),
 * then restores PRX on restore_ch+restore_addr and re-asserts CE high. Returns TX_DS.
 * Compiled in both roles (no KASE_HAS_RF_TX guard). */
bool rf_driver_oob_tx(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                      const uint8_t *payload, uint8_t len,
                      uint8_t restore_ch, const uint8_t restore_addr[5]);
```

Inside the `#if CONFIG_KASE_HAS_RF_TX` block (half-only), add:

```c
/* Reprogram the half's TX address (REG_TX_ADDR + REG_RX_ADDR_P0, 5 bytes, CE low).
 * Used to retarget the half radio to RF_PAIR_ADDR for the PKT_PAIR_REQ burst. */
void rf_driver_set_tx_address(rf_radio_t *r, const uint8_t addr[5]);

/* Pairing-only: switch the half radio to PRX on ch+addr, wait up to timeout_ms for
 * one RX payload (PKT_PAIR_ACK), copy to buf (max maxlen). Returns length (0 on
 * timeout). Leaves radio in PRX — caller restores PTX via set_tx_address. */
uint16_t rf_driver_pair_listen(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                               uint8_t *buf, uint16_t maxlen, uint32_t timeout_ms);
```

### Step 3.3 — Implement `rf_driver_set_rx_address` + `rf_driver_oob_tx` in `rf_driver.c`

Place after `rf_driver_set_channel` (both roles; uses file-static helpers). Note
`CMD_W_TX_PAYLOAD`/`CMD_FLUSH_TX` are currently #define'd only inside the RF_TX block — add
copies above this block (or move them above `rf_driver_init`). Use these local defines if not
already visible:

```c
#ifndef CMD_W_TX_PAYLOAD
#define CMD_W_TX_PAYLOAD 0xA0
#endif
#ifndef CMD_FLUSH_TX
#define CMD_FLUSH_TX     0xE1
#endif
#define REG_TX_ADDR_OOB  0x10   /* avoid clashing with the RF_TX-block REG_TX_ADDR */

void rf_driver_set_rx_address(rf_radio_t *r, const uint8_t addr[5])
{
    ce_low(r);
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);
    ce_high(r);
}

bool rf_driver_oob_tx(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                      const uint8_t *payload, uint8_t len,
                      uint8_t restore_ch, const uint8_t restore_addr[5])
{
    /* ── Enter PTX on the pairing channel/address ── */
    ce_low(r);
    rf_driver_set_channel(r, ch);
    write_reg_buf(r, REG_TX_ADDR_OOB, addr, 5);
    write_reg_buf(r, REG_RX_ADDR_P0,  addr, 5);   /* match TX_ADDR for ESB ACK */
    rf_driver_write_reg(r, REG_CONFIG, 0x3E);     /* PTX power-up, EN_CRC|CRCO */
    vTaskDelay(pdMS_TO_TICKS(2));                  /* Tpd2stby */

    rf_driver_write_reg(r, REG_STATUS, 0x70);     /* clear flags */
    { uint8_t c = CMD_FLUSH_TX, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r); }

    /* ── Write payload + pulse CE ── */
    uint8_t tx[33], rxb[33];
    tx[0] = CMD_W_TX_PAYLOAD;
    memcpy(&tx[1], payload, len);
    csn_low(r); spi_xfer(r, tx, rxb, (size_t)(len + 1)); csn_high(r);
    ce_high(r); esp_rom_delay_us(15); ce_low(r);

    /* ── Poll TX_DS / MAX_RT ── */
    uint32_t deadline = (uint32_t)(esp_timer_get_time() + 5000);
    uint8_t status = 0;
    do {
        status = rf_driver_read_reg(r, REG_STATUS);
        if (status & 0x30) break;
    } while ((uint32_t)esp_timer_get_time() < deadline);
    bool ok = (status & 0x20) != 0;   /* TX_DS = ACK received */
    if (status & 0x10) { uint8_t c = CMD_FLUSH_TX, rx; csn_low(r); spi_xfer(r,&c,&rx,1); csn_high(r); }
    rf_driver_write_reg(r, REG_STATUS, 0x30);

    /* ── Restore PRX on the data address/channel ── */
    ce_low(r);
    rf_driver_set_channel(r, restore_ch);
    write_reg_buf(r, REG_RX_ADDR_P0, restore_addr, 5);
    rf_driver_write_reg(r, REG_CONFIG, 0x3F);     /* PRX power-up */
    vTaskDelay(pdMS_TO_TICKS(2));
    rf_driver_write_reg(r, REG_STATUS, 0x70);
    { uint8_t c = 0xE2 /*FLUSH_RX*/, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r); }
    ce_high(r);                                    /* resume listening */
    return ok;
}
```

Confirm `esp_rom_delay_us` is available — `rf_driver.c` already uses it in `rf_driver_send`
(it includes `esp_rom_sys.h` via the half path; if compiling for the dongle the include must
be unconditional). Add `#include "esp_rom_sys.h"` near the top of `rf_driver.c` if it is
currently only pulled in by the RF_TX section. Likewise `esp_timer_get_time` needs
`esp_timer.h` and `vTaskDelay` needs `freertos/FreeRTOS.h` + `freertos/task.h` — verify these
are included unconditionally (the PRX init already uses `vTaskDelay`, so FreeRTOS is present).

### Step 3.4 — Implement the half-only helpers inside `#if CONFIG_KASE_HAS_RF_TX`

```c
void rf_driver_set_tx_address(rf_radio_t *r, const uint8_t addr[5])
{
    ce_low(r);
    write_reg_buf(r, REG_TX_ADDR,    addr, 5);
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);   /* must match for ESB ACK */
}

uint16_t rf_driver_pair_listen(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                               uint8_t *buf, uint16_t maxlen, uint32_t timeout_ms)
{
    ce_low(r);
    rf_driver_set_channel(r, ch);
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);
    rf_driver_write_reg(r, REG_CONFIG, 0x3F);    /* PRX power-up */
    vTaskDelay(pdMS_TO_TICKS(2));
    rf_driver_write_reg(r, REG_STATUS, 0x70);
    { uint8_t c = 0xE2 /*FLUSH_RX*/, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r); }
    ce_high(r);

    uint16_t got = 0;
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + timeout_ms;
    while ((uint32_t)(esp_timer_get_time() / 1000) < deadline) {
        if (rf_driver_rx_available(r)) {
            got = rf_driver_read_rx(r, buf, maxlen);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ce_low(r);
    return got;
}
```

`REG_TX_ADDR` is already defined inside the RF_TX block — these functions are in that block so
it is in scope.

### Step 3.5 — Build dongle (exercises rf_driver_oob_tx, set_rx_address)

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

Common failure modes:
- `esp_rom_delay_us undeclared` → add `#include "esp_rom_sys.h"` unconditionally.
- `write_reg_buf undeclared` here → `rf_driver_oob_tx`/`set_rx_address` must be placed AFTER
  the static `write_reg_buf` definition (it is defined ~line 69).

### Step 3.6 — Build half_left (exercises set_tx_address, pair_listen)

```bash
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

### Step 3.7 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/rf_driver.h main/comm/rf/rf_driver.c
git commit -m "feat(rf): driver OOB PTX + PRX-listen + live address reprogram for pairing"
```

---

## Task 4: Dongle pairing state machine + hot-switch in `rf_rx_task`

**Files:**
- Modify: `main/comm/rf/rf_rx_task.h`
- Modify: `main/comm/rf/rf_rx_task.c`

### Step 4.1 — Declare `rf_rx_pair_start` in `rf_rx_task.h`

```bash
sed -n '1,40p' /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/rf_rx_task.h
```

Add to `rf_rx_task.h` (before the closing guard):

```c
/* Begin a pairing window (called from the CDC KS_CMD_RF_PAIR_START handler).
 * reset=1 → clear NVS mac_left/mac_right/paired_count first. Switches radio L to
 * the pairing rendezvous (RF_PAIR_ADDR/RF_PAIR_CHANNEL) PRX and starts a 30 s
 * window driven by rf_rx_task. Returns set_id (computed) and current paired_count. */
bool rf_rx_pair_start(uint8_t reset, uint16_t *set_id_out, uint8_t *paired_count_out);
```

### Step 4.2 — Add pairing state + includes to `rf_rx_task.c`

In the include block, add:

```c
#include "rf_pairing.h"   /* rf_compute_set_id, rf_apply_set_id, pairing NVS + pure helpers */
```

After the `static rf_radio_t s_left, s_right;` line, add the pairing working state:

```c
/* ── Pairing window state (driven inside rf_rx_task) ── */
static volatile bool s_pairing_mode = false;
static uint32_t s_pair_deadline_ms = 0;
static uint8_t  s_pair_paired_count = 0;
static uint8_t  s_pair_mac_left[6]  = {0};
static uint8_t  s_pair_mac_right[6] = {0};
#define RF_PAIR_WINDOW_MS 30000
```

### Step 4.3 — Implement `rf_rx_pair_start`

Add (e.g. after `drain_radio`, before `rf_rx_task`):

```c
bool rf_rx_pair_start(uint8_t reset, uint16_t *set_id_out, uint8_t *paired_count_out)
{
    if (!s_left.present) return false;   /* need radio L for the rendezvous */

    if (reset) {
        rf_pairing_reset_dongle();
    }
    rf_pairing_load_peers_dongle(s_pair_mac_left, s_pair_mac_right, &s_pair_paired_count);

    /* Switch radio L to the pairing rendezvous PRX. */
    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;
    rf_driver_set_channel(&s_left, RF_PAIR_CHANNEL);
    rf_driver_set_rx_address(&s_left, pair_addr);

    s_pair_deadline_ms = (uint32_t)(esp_timer_get_time() / 1000) + RF_PAIR_WINDOW_MS;
    s_pairing_mode = true;

    if (set_id_out)       *set_id_out = rf_compute_set_id();
    if (paired_count_out) *paired_count_out = s_pair_paired_count;
    ESP_LOGI(TAG, "pairing window open (reset=%u, paired_count=%u)", reset, s_pair_paired_count);
    return true;
}
```

### Step 4.4 — Add the pairing service + hot-switch (called from `rf_rx_task`)

Add a helper that the task calls when `s_pairing_mode`:

```c
/* Reprogram both radios to the derived per-set address+channel (or factory if
 * paired_count==0). Hot-switch — no reboot (USB stays up). */
static void rf_rx_apply_paired_config(void)
{
    uint16_t set_id = (s_pair_paired_count > 0) ? rf_compute_set_id() : 0;

    rf_radio_cfg_t lcfg = board_rf_left_cfg();
    rf_radio_cfg_t rcfg = board_rf_right_cfg();
    rf_apply_set_id(&lcfg, set_id, 0x01);
    rf_apply_set_id(&rcfg, set_id, 0x02);

    uint8_t laddr[5] = { lcfg.rx_addr[0], lcfg.rx_addr[1], lcfg.rx_addr[2],
                         lcfg.rx_addr[3], lcfg.addr_suffix };
    uint8_t raddr[5] = { rcfg.rx_addr[0], rcfg.rx_addr[1], rcfg.rx_addr[2],
                         rcfg.rx_addr[3], rcfg.addr_suffix };
    rf_driver_set_channel(&s_left,  lcfg.channel);
    rf_driver_set_rx_address(&s_left, laddr);
    if (s_right.present) {
        rf_driver_set_channel(&s_right,  rcfg.channel);
        rf_driver_set_rx_address(&s_right, raddr);
    }
    ESP_LOGI(TAG, "hot-switch: set_id=0x%04X L ch=%u R ch=%u", set_id, lcfg.channel, rcfg.channel);
}

/* Process the pairing rendezvous on radio L. Returns true while still pairing. */
static bool rf_rx_pairing_service(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    /* Window timeout or both halves paired → close + hot-switch. */
    if (now >= s_pair_deadline_ms || s_pair_paired_count >= 2) {
        rf_rx_apply_paired_config();
        s_pairing_mode = false;
        ESP_LOGI(TAG, "pairing window closed (paired_count=%u)", s_pair_paired_count);
        return false;
    }

    /* Drain any PKT_PAIR_REQ on radio L. */
    uint8_t buf[32];
    while (rf_driver_rx_available(&s_left)) {
        uint16_t n = rf_driver_read_rx(&s_left, buf, sizeof(buf));
        if (n == 0) break;
        uint8_t mac[6];
        if (!rf_decode_pair_req(buf, n, mac)) continue;

        uint8_t slot = 0;
        bool is_dup = rf_pairing_match_slot(mac, s_pair_mac_left, s_pair_mac_right, &slot);
        if (!is_dup) {
            if (!rf_pairing_assign_slot(s_pair_paired_count, &slot)) continue; /* full */
        }

        /* Persist (new pairings only bump count). */
        if (!is_dup) {
            uint8_t new_count = s_pair_paired_count + 1;
            rf_pairing_save_peer_dongle(slot, mac, new_count);
            if (slot == 0x01) memcpy(s_pair_mac_left,  mac, 6);
            else              memcpy(s_pair_mac_right, mac, 6);
            s_pair_paired_count = new_count;
        }

        /* Send PKT_PAIR_ACK out-of-band on radio L, then restore PAIR PRX. */
        uint8_t dmac[6];
        esp_read_mac(dmac, ESP_MAC_WIFI_STA);
        rf_pair_ack_t ack = { .set_id = rf_compute_set_id(), .slot = slot };
        memcpy(ack.dongle_wifi_mac, dmac, 6);
        uint8_t ackbuf[10];
        rf_encode_pair_ack(ackbuf, &ack);

        static const uint8_t pair_addr[5] = RF_PAIR_ADDR;
        rf_driver_oob_tx(&s_left, RF_PAIR_CHANNEL, pair_addr, ackbuf, 10,
                         RF_PAIR_CHANNEL, pair_addr);   /* restore to PAIR PRX */
        ESP_LOGI(TAG, "ACK sent slot=0x%02X (dup=%d, paired_count=%u)",
                 slot, is_dup, s_pair_paired_count);
    }
    return true;
}
```

`esp_read_mac`/`ESP_MAC_WIFI_STA` need `#include "esp_mac.h"` in `rf_rx_task.c`.

### Step 4.5 — Hook the pairing service into the `rf_rx_task` loop

At the TOP of the `for (;;)` body in `rf_rx_task`, after the `xSemaphoreTake(...)` line, add:

```c
        if (s_pairing_mode) {
            rf_rx_pairing_service();
            continue;   /* skip normal RX/engine while pairing */
        }
```

This serializes pairing vs normal RX in the same task (spec §5.4). When the service returns
(window closed), `s_pairing_mode` is false and normal RX resumes on the new addresses.

### Step 4.6 — Build dongle

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

Common failure modes:
- `board_rf_left_cfg undeclared` in `rf_rx_apply_paired_config` → `board_rf.h` is already
  included in `rf_rx_task.c` (used by `rf_rx_start`). Confirm.
- `esp_read_mac undeclared` → add `#include "esp_mac.h"`.

### Step 4.7 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/rf_rx_task.h main/comm/rf/rf_rx_task.c
git commit -m "feat(dongle): pairing state machine + ACK TX + per-set hot-switch in rf_rx_task"
```

---

## Task 5: CDC command `KS_CMD_RF_PAIR_START`

**Files:**
- Modify: `main/comm/cdc/cdc_binary_protocol.h`
- Modify: `main/comm/cdc/cdc_binary_cmds.c`

### Step 5.1 — Add the command ID

In `cdc_binary_protocol.h`, in the Diagnostics group (after `KS_CMD_NVS_RESET = 0xB1`), add:

```c
    KS_CMD_RF_PAIR_START    = 0xB2,
```

### Step 5.2 — Add the handler + table entry in `cdc_binary_cmds.c`

At the top of `cdc_binary_cmds.c`, after the existing includes, add a guarded include:

```c
#if CONFIG_KASE_HAS_RF_RX
#include "rf_rx_task.h"   /* rf_rx_pair_start */
#endif
```

Add the handler (guarded — only meaningful on the dongle):

```c
#if CONFIG_KASE_HAS_RF_RX
/* RF_PAIR_START: payload [reset:u8]. Opens a 30 s pairing window asynchronously
 * (the exchange runs in rf_rx_task). Responds immediately with
 * [set_id_hi, set_id_lo, paired_count]. */
static void bin_cmd_rf_pair_start(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    uint8_t reset = (l >= 1) ? p[0] : 0;
    uint16_t set_id = 0;
    uint8_t  paired_count = 0;
    if (!rf_rx_pair_start(reset, &set_id, &paired_count)) {
        ks_respond_err(cmd, KS_STATUS_ERR_BUSY);
        return;
    }
    uint8_t resp[3] = { (uint8_t)(set_id >> 8), (uint8_t)(set_id & 0xFF), paired_count };
    ks_respond(cmd, KS_STATUS_OK, resp, sizeof(resp));
}
#endif /* CONFIG_KASE_HAS_RF_RX */
```

In the `bin_cmd_table[]`, in the Diagnostics section (after `{ KS_CMD_NVS_RESET, ... }`), add a
guarded entry:

```c
#if CONFIG_KASE_HAS_RF_RX
    { KS_CMD_RF_PAIR_START,     bin_cmd_rf_pair_start },
#endif
```

The keyboard builds (no `KASE_HAS_RF_RX`) skip both the include, handler, and table entry — so
`cdc_binary_cmds.c` still links on V1/V2/V2D without `rf_rx_task.h`.

### Step 5.3 — Build dongle

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

### Step 5.4 — Build a keyboard to confirm the guard (cdc_binary_cmds.c shared)

The keyboard build proves the `#if CONFIG_KASE_HAS_RF_RX` guard keeps `cdc_binary_cmds.c`
linking without `rf_rx_task.h`. (kase_v2 has a pre-existing BT Kconfig issue per repo notes;
use kase_v1 which builds clean.)

```bash
rm -f sdkconfig
idf.py -B build_kase_v1 -DBOARD=kase_v1 -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.` (no regression from RF-2 — the guard excludes the RF code).

### Step 5.5 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/cdc/cdc_binary_protocol.h main/comm/cdc/cdc_binary_cmds.c
git commit -m "feat(dongle): KS_CMD_RF_PAIR_START CDC command (0xB2)"
```

---

## Task 6: Half BOOT-button trigger + pairing task (REQ loop, ACK wait, NVS, reboot)

**Files:**
- Modify: `main/comm/rf/half_scan_task.c`

### Step 6.1 — Read the heartbeat callback + task body anchors

```bash
grep -n "heartbeat_timer_cb\|half_scan_task\|s_radio\|half_spi_lock\|#include\|board_nrf_cfg" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/half_scan_task.c | head -30
```

Confirm `rf_radio_t s_radio;` is file-scope non-static, `heartbeat_timer_cb` runs every 100 ms,
and `rf_pairing.h` is already included (added in Plan RF-1 Task 4).

### Step 6.2 — Add includes + pairing state

In the `#ifndef TEST_HOST` include block, ensure `#include "rf_pairing.h"` is present (from
RF-1) and add `#include "esp_mac.h"` and `#include "esp_system.h"` (for `esp_read_mac` /
`esp_restart`). `driver/gpio.h` is already included.

After `static volatile uint8_t s_seq = 0;`, add:

```c
/* ── BOOT-button pairing trigger (GPIO0, active-low) ── */
#define HALF_BOOT_GPIO        GPIO_NUM_0
#define HALF_BOOT_HOLD_TICKS  30      /* 30 × 100 ms heartbeat ticks ≈ 3 s */
static volatile bool s_pairing_active = false;
```

### Step 6.3 — Configure GPIO0 in the task body (once)

In `half_scan_task`, after `half_spi_lock_init();` (and before the matrix init), add:

```c
    /* BOOT button (GPIO0) as input with pull-up — pairing trigger (held 3 s). */
    gpio_config_t boot_cfg = {
        .pin_bit_mask = (1ULL << HALF_BOOT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,   /* polled in heartbeat_timer_cb */
    };
    gpio_config(&boot_cfg);
```

### Step 6.4 — Implement the pairing task

Add (above `heartbeat_timer_cb` so the callback can reference it, or forward-declare):

```c
/* ── Half pairing task: REQ loop on the rendezvous, await PKT_PAIR_ACK ── */
static void half_pairing_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pairing: window open (30 s) — sending PKT_PAIR_REQ");

    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    uint8_t req[7];
    rf_encode_pair_req(req, my_mac);

    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + 30000;
    bool acked = false;
    rf_pair_ack_t ack;

    while (!acked && (uint32_t)(esp_timer_get_time() / 1000) < deadline) {
        /* Burst one REQ as PTX on the rendezvous. */
        half_spi_lock();
        rf_driver_set_tx_address(&s_radio, pair_addr);
        rf_driver_set_channel(&s_radio, RF_PAIR_CHANNEL);
        rf_driver_send(&s_radio, req, 7);
        half_spi_unlock();

        /* Listen ~150 ms for the ACK as PRX on the rendezvous. */
        uint8_t rxb[32];
        half_spi_lock();
        uint16_t n = rf_driver_pair_listen(&s_radio, RF_PAIR_CHANNEL, pair_addr,
                                           rxb, sizeof(rxb), 150);
        half_spi_unlock();
        if (n && rf_decode_pair_ack(rxb, n, &ack)) {
            acked = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));   /* ~200 ms REQ cadence (spec §3.3) */
    }

    if (acked) {
        ESP_LOGI(TAG, "pairing: ACK set_id=0x%04X slot=0x%02X — saving NVS + reboot",
                 ack.set_id, ack.slot);
        rf_pairing_save_half(ack.set_id, ack.slot, ack.dongle_wifi_mac);
        vTaskDelay(pdMS_TO_TICKS(2000));   /* let log/NVS settle, then apply via reboot */
        esp_restart();
    } else {
        ESP_LOGW(TAG, "pairing: timed out (no ACK) — restoring normal TX");
        /* Restore the half's data address/channel from NVS (or factory). */
        uint8_t slot = BOARD_NRF_ADDR_SUFFIX;
        uint16_t set_id = rf_pairing_load_set_id_half(BOARD_NRF_ADDR_SUFFIX, &slot);
        rf_radio_cfg_t cfg = board_nrf_cfg();
        rf_apply_set_id(&cfg, set_id, slot);
        uint8_t addr[5] = { cfg.rx_addr[0], cfg.rx_addr[1], cfg.rx_addr[2],
                            cfg.rx_addr[3], cfg.addr_suffix };
        half_spi_lock();
        rf_driver_set_tx_address(&s_radio, addr);
        rf_driver_set_channel(&s_radio, cfg.channel);
        half_spi_unlock();
        s_pairing_active = false;
    }
    vTaskDelete(NULL);
}
```

`board_nrf_cfg()` is file-static in `half_scan_task.c` — in scope here.

### Step 6.5 — Poll the BOOT button in `heartbeat_timer_cb`

At the END of `heartbeat_timer_cb` (after the existing battery stub block), add:

```c
    /* BOOT-button pairing trigger: spawn the pairing task after a ~3 s hold. */
    static uint8_t s_boot_low_ticks = 0;
    if (!s_pairing_active) {
        if (gpio_get_level(HALF_BOOT_GPIO) == 0) {   /* active-low: pressed */
            if (++s_boot_low_ticks >= HALF_BOOT_HOLD_TICKS) {
                s_boot_low_ticks = 0;
                s_pairing_active = true;
                xTaskCreatePinnedToCore(half_pairing_task, "half_pair",
                                        4096, NULL, 4, NULL, 0);
            }
        } else {
            s_boot_low_ticks = 0;   /* released early — reset hold counter */
        }
    }
```

Note: while `s_pairing_active`, the heartbeat callback still fires but the half's normal TX
targets the rendezvous-retargeted radio. That is acceptable for the pairing window — heartbeats
during pairing will MAX_RT (no dongle on the data address) and are harmless. The half reboots
on success; on timeout the task restores the data address.

### Step 6.6 — Build half_left

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

Common failure modes:
- `half_pairing_task defined but not used` (if the button poll edit was missed) → ensure
  Step 6.5 references it.
- `rf_driver_pair_listen / set_tx_address undeclared` → confirm Task 3 declared them inside
  `#if CONFIG_KASE_HAS_RF_TX` in `rf_driver.h` and the half build has that flag.
- `esp_restart undeclared` → add `#include "esp_system.h"`.

### Step 6.7 — Build half_right

```bash
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

### Step 6.8 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/half_scan_task.c
git commit -m "feat(half): BOOT-button pairing trigger + REQ/ACK task + NVS persist + reboot"
```

---

## Task 7: Final verification — 3 RF boards + 1 keyboard green + host tests green

**Files:** none changed unless a build error forces a fix.

### Step 7.1 — Build dongle (clean)

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

### Step 7.2 — Build half_left + half_right (clean)

```bash
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.` for both.

### Step 7.3 — Build kase_v1 keyboard (clean) — guard regression check

```bash
rm -f sdkconfig
idf.py -B build_kase_v1 -DBOARD=kase_v1 -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.` (proves `#if CONFIG_KASE_HAS_RF_RX` guard in
`cdc_binary_cmds.c` keeps the keyboard build clean — no `rf_rx_task.h` dependency leaks).

### Step 7.4 — Run host tests

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner
```

Expected: `0 failed`; count grew vs the RF-1 baseline (new PKT_PAIR + slot/match cases).

### Step 7.5 — Commit if any files were adjusted

If nothing changed: no commit. If a fix was needed:

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add <fixed-file>
git commit -m "fix(rf): <short description of fix>"
```

---

## Self-review checklist

- [ ] `PKT_TYPE_PAIR_REQ = 0xF`, `PKT_TYPE_PAIR_ACK = 0xE` added to `rf_packet.h`; encode/decode
  `static inline`, byte-exact per spec §3.3 (REQ 7 B `0xF0`+mac; ACK 10 B `0xE0`+BE set_id+mac+slot).
- [ ] PKT_PAIR_* round-trip + negative (wrong type, short buffer) host tests pass.
- [ ] `rf_pairing_assign_slot` (0→0x01, 1→0x02, 2→false) and `rf_pairing_match_slot` (dedup,
  all-zero = empty) are PURE (outside `#ifndef TEST_HOST`) and host-tested.
- [ ] NVS namespace `"rf"`; keys exactly `mac_left`/`mac_right`/`paired_count` (dongle),
  `set_id`/`slot`/`mac_dongle` (half); MACs are 6-byte blobs (Plan 3 reads these).
- [ ] Dongle ACK TX uses `rf_driver_oob_tx` (NOT `rf_driver_send`, which is RF_TX-only); restores
  PAIR PRX afterward.
- [ ] Dongle hot-switch: `rf_rx_apply_paired_config` rebuilds cfgs + `rf_apply_set_id` + live
  reprogram via `rf_driver_set_rx_address` + `rf_driver_set_channel` — NO `esp_restart` on the
  dongle (USB HID/CDC stays up). paired_count==0 → set_id 0 → factory addresses retained.
- [ ] Half applies via reboot: `rf_pairing_save_half` then `esp_restart` (2 s delay); on reboot,
  RF-1's `half_scan_task` wiring re-derives from NVS.
- [ ] Pairing serialized in `rf_rx_task` via `s_pairing_mode` flag (top-of-loop `continue`); no
  new dongle task.
- [ ] Half pairing runs in a dedicated `half_pairing_task` (not in the timer callback); the
  callback only spawns it once (`s_pairing_active` guard) after a 3 s GPIO0-low hold.
- [ ] BOOT button = GPIO0, active-low, internal pull-up, polled at 100 ms × 30 ticks ≈ 3 s.
- [ ] `KS_CMD_RF_PAIR_START = 0xB2`; handler + table entry guarded `#if CONFIG_KASE_HAS_RF_RX`;
  response `[set_id_hi, set_id_lo, paired_count]`; `reset=1` clears NVS.
- [ ] RF-1 function signatures unchanged; RF-2 only ADDED to `rf_pairing.{c,h}`.
- [ ] All driver helpers placed after the file-static `write_reg_buf` definition; both-roles
  helpers have no `KASE_HAS_RF_TX` guard; half-only helpers are inside it.
- [ ] dongle + half_left + half_right + kase_v1 build green; host tests `0 failed`.

---

## Out of scope / deferred to Plan 3 and beyond

| Item | Reason |
|------|--------|
| ESP-NOW peer registration from `mac_left`/`mac_right`/`mac_dongle` | Plan 3 — RF-2 only STORES the MACs |
| `esp_wifi_set_channel` from `rf_derive_wifi_ch` | Plan 3 |
| Live e-ink layer display | Plan 3 |
| `KS_CMD_RF_PAIR_STATUS` poll command (spec §5.6) | Optional follow-up; not required for the core flow |
| Partial re-pair (`KS_CMD_RF_PAIR_RESET_SLOT`) | Spec §3.6 defers it; RF-2 has whole-reset via `reset=1` only |
| Pairing encryption / authentication | Spec §9, §11 — explicitly out of scope |
| Simultaneous-pairing collision mitigation beyond dual-trigger + 30 s window | Spec §3.2 documents it as a known limitation |
| Battery telemetry over ESP-NOW (half heartbeat stub) | Spec §11 — separate work |
| Bench validation (two sets, no cross-talk; e-ink layer push) | Spec §10.2 — after Plan 3 |
