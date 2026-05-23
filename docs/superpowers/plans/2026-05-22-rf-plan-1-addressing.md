# Plan RF-1 — Per-Set NRF Addressing + set_id Derivation + Unpaired Fallback

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new `rf_pairing.{c,h}` module to `main/comm/rf/` that derives a per-set NRF24
address + channel (and WiFi channel) from the dongle's factory WiFi MAC, via two PURE
functions (`rf_compute_set_id`, `rf_apply_set_id`) plus a WiFi-channel derivation
(`rf_derive_wifi_ch`). Host-test the pure derivations FIRST (TDD), including an explicit test
that `set_id = 0` (and `0xFFFF`) leaves an `rf_radio_cfg_t` completely unchanged so an
unpaired set reproduces today's factory `KaSe.01`/`KaSe.02` addresses and channels 76/82.
Then wire `rf_apply_set_id()` into the dongle's two RX cfgs (`rf_rx_start` via
`board_rf_left_cfg`/`board_rf_right_cfg`) and the half's one TX cfg (`half_scan_task` via
`board_nrf_cfg`), reading `set_id`/`slot` from NVS namespace `rf`. An UNPAIRED build (no NVS
record) MUST behave byte-for-byte identically to today. End state: all three RF boards
(dongle, half_left, half_right) build green; host tests stay at 0 failed and grow.

**Architecture:** One new pure-logic module, two hook points (dongle RX, half TX). The
derivation is applied in place to the `rf_radio_cfg_t` AFTER the board factory cfg is built
and BEFORE it is handed to `rf_driver_init()` / `rf_driver_init_tx()`. The radio driver itself
is untouched — it already reads `cfg->rx_addr[0..3]` + `cfg->addr_suffix` + `cfg->channel`.

```
[DONGLE firmware — rf_rx_start()]

  lcfg = board_rf_left_cfg()    →  {rx_addr={K,a,S,e}, addr_suffix=0x01, channel=0x4C}
  rcfg = board_rf_right_cfg()   →  {rx_addr={K,a,S,e}, addr_suffix=0x02, channel=0x52}
       │
       ├── [RF-1 HOOK] set_id = rf_pairing_load_set_id_dongle()   (NVS rf.paired_count gate)
       │       paired_count==0  → set_id = 0  (no-op below → factory defaults preserved)
       │       paired_count>0   → set_id = rf_compute_set_id()    (CRC16 of own WiFi MAC)
       │
       ├── [RF-1 HOOK] rf_apply_set_id(&lcfg, set_id, 0x01)   (patches addr+channel in place)
       ├── [RF-1 HOOK] rf_apply_set_id(&rcfg, set_id, 0x02)
       │
       ├── rf_driver_init(&s_left,  &lcfg)    ← unchanged
       └── rf_driver_init(&s_right, &rcfg)    ← unchanged


[HALF firmware — half_scan_task()]

  nrf_cfg = board_nrf_cfg()     →  {rx_addr={K,a,S,e}, addr_suffix=BOARD_NRF_ADDR_SUFFIX,
       │                            channel=BOARD_NRF_CHANNEL}
       │
       ├── [RF-1 HOOK] set_id = rf_pairing_load_set_id_half(&slot)  (NVS rf.set_id / rf.slot)
       │       key absent or 0 → set_id = 0, slot = BOARD_NRF_ADDR_SUFFIX (factory)
       │       key present     → set_id, slot from NVS
       │
       ├── [RF-1 HOOK] rf_apply_set_id(&nrf_cfg, set_id, slot)
       │
       └── rf_driver_init_tx(&s_radio, &nrf_cfg)    ← unchanged
```

**Tech Stack:** ESP-IDF 5.5, `esp_mac.h` (`esp_read_mac`, in `esp_hw_support`), `nvs.h`
(`nvs_open`/`nvs_get_*`, in `nvs_flash`), existing `rf_driver.h` (`rf_radio_cfg_t`),
FreeRTOS. Pure derivation functions have NO ESP-IDF deps and compile under `-DTEST_HOST`.

**Spec reference:** `docs/superpowers/specs/2026-05-22-rf-pairing-addressing-design.md` —
sections 2.1, 2.2, 2.3, 2.4, 2.5, 4, 5.1, 5.2, 5.3, 10.1. Read in full before touching any
file. This plan is an executor's companion, not a substitute.

**Depends on:** The dongle + half firmware (branch `dongle-firmware`) is build-green:
`rf_rx_start()` builds cfgs from `board_rf_left_cfg`/`board_rf_right_cfg`; `half_scan_task`
builds its cfg from `board_nrf_cfg()`. `rf_radio_cfg_t` has `rx_addr[4]`, `addr_suffix`,
`channel`. All confirmed present.

**Build command (always `rm -f sdkconfig` when switching boards; run from repo root):**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_<target> -DBOARD=kase_<target> -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

`-DIDF_TARGET=esp32s3` is REQUIRED after `rm sdkconfig`. `sdkconfig` is SHARED across boards
(rm it when switching). The shell cwd persists between bash calls — `cd` to the repo root
first. Build dirs: `build_dongle`, `build_half_left`, `build_half_right`.

**Build targets:**
- `idf.py -B build_dongle     -DBOARD=kase_dongle     -DIDF_TARGET=esp32s3 build`
- `idf.py -B build_half_left  -DBOARD=kase_half_left  -DIDF_TARGET=esp32s3 build`
- `idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build`

**Host test command (baseline: 890 pass, 0 failed — must stay at 0 failed, grow in count):**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner
```

---

## Baked-in facts (read before touching any file)

1. **`rf_radio_cfg_t` splits the 5-byte address into `rx_addr[4]` (base) + `addr_suffix`
   (5th byte).** `rf_driver_init` / `rf_driver_init_tx` build the on-air address as
   `{rx_addr[0], rx_addr[1], rx_addr[2], rx_addr[3], addr_suffix}`. So `rf_apply_set_id`
   writes `rx_addr[0..3]` AND `addr_suffix` — exactly the spec §5.1 formula.

2. **Factory defaults today (must be reproduced when set_id==0):**
   - Left:  `rx_addr = {'K','a','S','e'}`, `addr_suffix = 0x01`, `channel = 0x4C` (76).
   - Right: `rx_addr = {'K','a','S','e'}`, `addr_suffix = 0x02`, `channel = 0x52` (82).
   These live in `boards/kase_dongle/board_rf.h` (`board_rf_left_cfg`/`board_rf_right_cfg`,
   `RF_CH_LEFT_DEFAULT=0x4C`, `RF_CH_RIGHT_DEFAULT=0x52`) and in
   `boards/kase_half_left/board.h` (`BOARD_NRF_ADDR_SUFFIX=0x01`, `BOARD_NRF_CHANNEL=0x4C`) /
   `boards/kase_half_right/board.h` (suffix `0x02`, channel `0x52`).

3. **`rf_apply_set_id` MUST be a no-op for `set_id == 0` AND `set_id == 0xFFFF`.** Per spec
   §2.4 the derivation is NOT applied to the sentinel; the caller keeps the board factory
   cfg unchanged. This is THE backwards-compatibility guarantee. The host test in Task 1
   asserts a factory cfg passes through `rf_apply_set_id(&cfg, 0, slot)` BYTE-FOR-BYTE
   unchanged for both left and right.

4. **The pure derivation functions live in `rf_pairing.c` OUTSIDE any `#ifndef TEST_HOST`
   guard.** `rf_compute_set_id()` reads eFuse (`esp_read_mac`) so its BODY is ESP-IDF-only —
   but `rf_apply_set_id()` and `rf_derive_wifi_ch()` are pure C and host-testable. Structure
   `rf_pairing.c` so that `rf_apply_set_id`, `rf_derive_wifi_ch`, and the internal
   `crc16_ccitt` helper are OUTSIDE `#ifndef TEST_HOST`; everything that calls `esp_read_mac`
   / `nvs_*` / `ESP_LOG*` is INSIDE the guard. The crc16 helper is host-testable so that the
   `set_id = crc16(mac)` mapping itself can be checked against a known vector.

5. **`rf_pairing.h` must be host-safe — the cfg-typed parts are TEST_HOST-guarded.**
   `rf_driver.h` is NOT host-clean (it includes `esp_err.h` + `driver/spi_master.h`), so
   `rf_pairing.h` includes `rf_driver.h` ONLY inside `#ifndef TEST_HOST`. The pure declarations
   (`rf_derive_addr`, `rf_derive_wifi_ch`, `crc16_ccitt`) use primitive C types only and live
   OUTSIDE the guard, so the host parse never touches `rf_radio_cfg_t` or any ESP-IDF header.
   `rf_apply_set_id` (which takes `rf_radio_cfg_t *`) is declared INSIDE the guard.

6. **The no-op / factory-fallback guarantee is fully covered by the PURE function.** The host
   test never needs `rf_apply_set_id` (the cfg-typed wrapper). Instead the test exercises
   `rf_derive_addr` directly: it returns `false` for the sentinel (set_id 0 / 0xFFFF), and the
   test asserts that a caller who skips the copy on `false` keeps a factory cfg byte-for-byte
   unchanged (Case 1/2 in Step 1.4). `rf_apply_set_id` is a thin wrapper whose ONLY logic is
   "call `rf_derive_addr`; on true copy three fields; on false do nothing" — fully validated by
   the pure test plus the firmware build of Tasks 3/4. No host-side mirror struct, no shim.

7. **Module split (authoritative — implement exactly this).** Split the logic so the pure part
   does NOT depend on `rf_radio_cfg_t` at all:
   - Add a pure helper `rf_derive_addr(uint16_t set_id, uint8_t slot, uint8_t addr_out[4],
     uint8_t *suffix_out, uint8_t *channel_out)` that returns `false` for the sentinel
     (set_id 0 / 0xFFFF) and `true` otherwise, writing the four address bytes, suffix, and
     channel. This takes ONLY primitive C types → host-testable with zero ESP-IDF deps and no
     `rf_driver.h` include.
   - `rf_apply_set_id(rf_radio_cfg_t *cfg, uint16_t set_id, uint8_t slot)` is a THIN wrapper:
     it calls `rf_derive_addr`, and on `true` copies the results into `cfg->rx_addr[0..3]`,
     `cfg->addr_suffix`, `cfg->channel`; on `false` it leaves `cfg` untouched. The wrapper
     needs `rf_driver.h` so it stays in firmware-only code; the test exercises `rf_derive_addr`
     directly plus a SEPARATE wrapper test that is compiled with `rf_driver.h` available in
     firmware (skip in host if needed — the no-op guarantee is fully covered by
     `rf_derive_addr` returning false for the sentinel, asserted in the host tests).
   - Put `rf_derive_addr`, `rf_derive_wifi_ch`, and `crc16_ccitt` in `rf_pairing.c` OUTSIDE
     `#ifndef TEST_HOST`. Put `rf_apply_set_id`, `rf_compute_set_id`, and all NVS/eFuse
     helpers INSIDE `#ifndef TEST_HOST`. The host test compiles `rf_pairing.c` with
     `-DTEST_HOST` and links `rf_derive_addr` / `rf_derive_wifi_ch` / `crc16_ccitt`.
   - `rf_pairing.h` is host-safe: it declares `rf_derive_addr`, `rf_derive_wifi_ch`,
     `crc16_ccitt` with primitive types (no `rf_driver.h`), and declares `rf_apply_set_id`,
     `rf_compute_set_id`, `rf_pairing_load_set_id_dongle`, `rf_pairing_load_set_id_half`
     guarded by `#ifndef TEST_HOST` (so the host parse never sees `rf_radio_cfg_t`).

8. **set_id sentinel guard (spec §2.1):** if `crc16_ccitt(mac, 6)` returns `0x0000` or
   `0xFFFF`, `rf_compute_set_id()` returns `0x0001` instead. This guard lives in
   `rf_compute_set_id` (firmware-only), NOT in `rf_derive_addr` — `rf_derive_addr` treats 0
   and 0xFFFF as the sentinel (returns false). The two are consistent: a real paired set
   never carries set_id 0 or 0xFFFF because `rf_compute_set_id` already remapped it to 1.

9. **CRC-16/CCITT-FALSE parameters (spec §2.1):** polynomial `0x1021`, init `0xFFFF`, NO
   input reflection, NO output reflection, NO final XOR. Standard table-free bit-by-bit
   implementation. The host test pins a known vector so a future refactor cannot drift the
   algorithm.

10. **Channel formula (spec §2.3):** `base_ch = 80 + 2 * (set_id % 20)` (range 80..118, even).
    `ch_left = base_ch` (slot 0x01), `ch_right = base_ch + 1` (slot 0x02). Max `ch_right` =
    118 + 1 = 119 ≤ 125 (NRF24 max). The `slot == 0x01 → base_ch, else base_ch + 1` branch
    means ANY non-left slot value uses `base_ch + 1`; only `0x01` gets `base_ch`.

11. **WiFi channel formula (spec §2.5):** `wifi_ch = {1, 6, 11}[set_id % 3]`. This is
    consumed by Plan 3 (ESP-NOW); RF-1 only ships the pure function + host test. RF-1 does
    NOT call `esp_wifi_set_channel` anywhere.

12. **Dongle NVS gate (spec §4.2):** open `rf` namespace READONLY; read `paired_count` (u8).
    Absent or 0 → return set_id 0 (factory). Else → `rf_compute_set_id()`. The dongle does
    NOT store set_id; it recomputes from its own MAC.

13. **Half NVS gate (spec §4.2):** open `rf` namespace READONLY; read `set_id` (u16). Absent
    or 0 → return set_id 0, slot = `BOARD_NRF_ADDR_SUFFIX` (factory). `0xFFFF` → treat as
    `0x0001`. Else read `slot` (u8); if slot read fails, default to `BOARD_NRF_ADDR_SUFFIX`.

14. **NVS namespace is `rf` (spec §4.1).** Constant `RF_STORAGE_NAMESPACE "rf"` defined in
    `rf_pairing.h` (inside `#ifndef TEST_HOST` is fine — only firmware uses it). Do NOT use
    `STORAGE_NAMESPACE` (`"storage"`); pairing data is a separate namespace.

15. **IDE clangd errors about missing esp-idf headers are false positives.** Trust
    `idf.py build` as ground truth.

16. **CMake: `rf_pairing.c` is compiled for BOTH dongle (RF_RX) and half (RF_TX).** It is not
    role-specific; both roles call into it. Gate it on `CONFIG_KASE_HAS_RF_RX OR
    CONFIG_KASE_HAS_RF_TX`. The keyboard builds (V1/V2/V2D) have neither flag → `rf_pairing.c`
    not compiled there → keyboards unaffected.

---

## Derivation Reference Table (from spec §2.2, §2.3, §2.5)

```
set_id      : uint16_t, CRC-16/CCITT-FALSE of esp_read_mac(ESP_MAC_WIFI_STA), 6 bytes.
              poly=0x1021 init=0xFFFF, no reflect, no final xor. 0/0xFFFF → remap to 0x0001.

addr (5 B)  : { 'K'(0x4B), 'S'(0x53), set_id>>8, set_id&0xFF, slot }
              slot 0x01 = left, 0x02 = right.
              (Note: 'K','S' — NOT 'K','a','S','e'. Paired addr base is 2 ASCII + 2 id bytes.)

base_ch     : 80 + 2 * (set_id % 20)              range 80..118
ch_left     : base_ch          (slot 0x01)        range 80..118  (2480..2518 MHz)
ch_right    : base_ch + 1       (slot != 0x01)    range 81..119  (2481..2519 MHz)

wifi_ch     : {1, 6, 11}[set_id % 3]
```

Worked examples (used as host-test vectors):

```
set_id 0x1234 (=4660): 4660 % 20 = 0  → base_ch = 80; L ch=80, R ch=81
                       addr_L = {0x4B,0x53,0x12,0x34,0x01}
                       addr_R = {0x4B,0x53,0x12,0x34,0x02}
                       4660 % 3 = 1 → wifi_ch = 6
set_id 0x0014 (=20)  : 20 % 20 = 0   → base_ch = 80; L ch=80, R ch=81
set_id 0x0013 (=19)  : 19 % 20 = 19  → base_ch = 118; L ch=118, R ch=119 (≤125 OK)
set_id 0x0001 (=1)   : 1 % 20 = 1    → base_ch = 82; 1 % 3 = 1 → wifi_ch = 6
set_id 0x0000 / 0xFFFF: sentinel → rf_derive_addr returns false, cfg untouched
```

---

## File Structure

### Created

| Path | Responsibility |
|---|---|
| `main/comm/rf/rf_pairing.h` | Public API: pure derivations (host-safe) + firmware NVS/eFuse helpers (guarded) |
| `main/comm/rf/rf_pairing.c` | `crc16_ccitt`, `rf_derive_addr`, `rf_derive_wifi_ch` (pure); `rf_compute_set_id`, `rf_apply_set_id`, `rf_pairing_load_set_id_dongle/half` (firmware) |
| `test/test_rf_pairing.c` | Host tests for `rf_derive_addr` (incl. set_id=0 factory fallback), `rf_derive_wifi_ch`, `crc16_ccitt` |

### Modified

| Path | Change |
|---|---|
| `main/CMakeLists.txt` | Compile `comm/rf/rf_pairing.c` when `CONFIG_KASE_HAS_RF_RX` OR `CONFIG_KASE_HAS_RF_TX` |
| `main/comm/rf/rf_rx_task.c` | After `board_rf_*_cfg()`, load dongle set_id from NVS + `rf_apply_set_id` on both cfgs |
| `main/comm/rf/half_scan_task.c` | After `board_nrf_cfg()`, load half set_id+slot from NVS + `rf_apply_set_id` on the cfg |
| `test/CMakeLists.txt` | Add `test_rf_pairing.c` + `../main/comm/rf/rf_pairing.c` to `add_executable` |
| `test/test_main.c` | Add `extern void test_rf_pairing(void);` + call |

### Untouched

```
main/comm/rf/rf_driver.{c,h}     rf_radio_cfg_t, rf_driver_init/init_tx — read cfg as-is
boards/kase_dongle/board_rf.h    board_rf_left_cfg/right_cfg factory defaults — unchanged
boards/kase_half_left/board.h    BOARD_NRF_* factory defaults — unchanged
boards/kase_half_right/board.h   suffix 0x02, channel 0x52 — unchanged
main/sys/nvs_utils.{c,h}         not used here (we open NVS directly with a 6-byte/u16 schema)
sdkconfig                        rm before each board switch, never commit
```

---

## Task 1: Pure derivations + host tests (TDD) — `rf_derive_addr`, `rf_derive_wifi_ch`, `crc16_ccitt`

**Rationale:** The address/channel/wifi derivation and the set_id=0 fallback are the
load-bearing correctness pieces. Write and pass the host tests first, before wiring anything
into the radios. A green suite (including the factory-fallback assertion) is the prerequisite
for Tasks 2 and 3.

**Files:**
- Create: `main/comm/rf/rf_pairing.h`
- Create: `main/comm/rf/rf_pairing.c`
- Create: `test/test_rf_pairing.c`
- Modify: `test/CMakeLists.txt`
- Modify: `test/test_main.c`

### Step 1.1 — Read the structs and factory defaults this module depends on

```bash
sed -n '10,18p' /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/rf_driver.h
sed -n '13,43p' /home/mae/Documents/GitHub/KaSe_firmware/boards/kase_dongle/board_rf.h
```

Confirm: `rf_radio_cfg_t` has `uint8_t rx_addr[4]; uint8_t addr_suffix; uint8_t channel;`.
Confirm factory left = `{K,a,S,e}` suffix `0x01` ch `0x4C`; right suffix `0x02` ch `0x52`.

### Step 1.2 — Write `main/comm/rf/rf_pairing.h`

```c
#ifndef RF_PAIRING_H
#define RF_PAIRING_H

/*
 * rf_pairing.h — Per-set NRF24 address/channel derivation (Plan RF-1) and
 * pairing NVS/eFuse helpers (firmware-only).
 *
 * Pure derivations (host-testable, no ESP-IDF deps):
 *   crc16_ccitt, rf_derive_addr, rf_derive_wifi_ch.
 * Firmware-only helpers (NVS + eFuse, guarded by !TEST_HOST):
 *   rf_compute_set_id, rf_apply_set_id, rf_pairing_load_set_id_dongle/half.
 *
 * set_id derivation: CRC-16/CCITT-FALSE of the dongle WiFi STA MAC (eFuse).
 * Unpaired sentinel: set_id 0 or 0xFFFF → derivations are no-ops; the caller
 * keeps the board factory cfg unchanged (reproduces KaSe.01/.02, ch 76/82).
 *
 * See docs/superpowers/specs/2026-05-22-rf-pairing-addressing-design.md §2.
 */

#include <stdint.h>
#include <stdbool.h>

/* ── Pure derivations — host-safe, no ESP-IDF includes ─────────── */

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflect, no final XOR. */
uint16_t crc16_ccitt(const uint8_t *data, uint32_t len);

/* Derive the 5-byte NRF address (4-byte base + suffix) and channel for a
 * given set_id + slot (0x01=left, 0x02=right).
 *   addr_out[4]   ← {'K','S', set_id>>8, set_id&0xFF}
 *   *suffix_out   ← slot
 *   *channel_out  ← (slot == 0x01) ? base_ch : base_ch + 1
 *                   where base_ch = 80 + 2*(set_id % 20)
 * Returns false (and writes nothing) for the unpaired sentinel
 * (set_id == 0 || set_id == 0xFFFF). Returns true otherwise. */
bool rf_derive_addr(uint16_t set_id, uint8_t slot,
                    uint8_t addr_out[4], uint8_t *suffix_out,
                    uint8_t *channel_out);

/* Derive the ESP-NOW WiFi channel from set_id: {1,6,11}[set_id % 3].
 * For the unpaired sentinel (set_id 0 / 0xFFFF) returns 6 (factory default,
 * matching espnow_link.c). Plan 3 consumes this. */
uint8_t rf_derive_wifi_ch(uint16_t set_id);

#ifndef TEST_HOST

#include "rf_driver.h"   /* rf_radio_cfg_t — firmware only */

/* NVS namespace for pairing data (spec §4.1). Distinct from STORAGE_NAMESPACE. */
#define RF_STORAGE_NAMESPACE "rf"

/* Compute set_id from the dongle's own WiFi STA MAC (eFuse, no WiFi init).
 * Applies the 0/0xFFFF → 0x0001 guard (spec §2.1). */
uint16_t rf_compute_set_id(void);

/* Apply per-set address+channel to a cfg in place. No-op for the sentinel
 * (set_id 0 / 0xFFFF) → cfg keeps its factory defaults. */
void rf_apply_set_id(rf_radio_cfg_t *cfg, uint16_t set_id, uint8_t slot);

/* Dongle: read NVS rf.paired_count. 0/absent → return 0 (factory).
 * Else → rf_compute_set_id(). */
uint16_t rf_pairing_load_set_id_dongle(void);

/* Half: read NVS rf.set_id (u16) + rf.slot (u8). Absent/0 → return 0 and set
 * *slot_out = fallback_slot (the board factory suffix). 0xFFFF → 0x0001. */
uint16_t rf_pairing_load_set_id_half(uint8_t fallback_slot, uint8_t *slot_out);

#endif /* TEST_HOST */

#endif /* RF_PAIRING_H */
```

### Step 1.3 — Write `main/comm/rf/rf_pairing.c`

```c
/*
 * rf_pairing.c — Per-set NRF24 derivation + pairing NVS/eFuse helpers.
 * See rf_pairing.h and spec §2, §4, §5.
 *
 * Layout: pure derivations are OUTSIDE #ifndef TEST_HOST (host-testable).
 * eFuse/NVS/log helpers are INSIDE the guard (firmware only).
 */

#include "rf_pairing.h"

/* ── Pure: CRC-16/CCITT-FALSE ──────────────────────────────────── */
uint16_t crc16_ccitt(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
            else              crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ── Pure: NRF address + channel derivation ────────────────────── */
bool rf_derive_addr(uint16_t set_id, uint8_t slot,
                    uint8_t addr_out[4], uint8_t *suffix_out,
                    uint8_t *channel_out)
{
    if (set_id == 0x0000 || set_id == 0xFFFF) {
        return false;   /* unpaired sentinel — caller keeps factory cfg */
    }
    addr_out[0] = 'K';   /* 0x4B */
    addr_out[1] = 'S';   /* 0x53 */
    addr_out[2] = (uint8_t)(set_id >> 8);
    addr_out[3] = (uint8_t)(set_id & 0xFF);
    *suffix_out = slot;
    uint8_t base_ch = (uint8_t)(80 + 2 * (set_id % 20));   /* 80..118 even */
    *channel_out = (slot == 0x01) ? base_ch : (uint8_t)(base_ch + 1);
    return true;
}

/* ── Pure: WiFi channel derivation (Plan 3 consumer) ───────────── */
uint8_t rf_derive_wifi_ch(uint16_t set_id)
{
    if (set_id == 0x0000 || set_id == 0xFFFF) {
        return 6;   /* factory default ESP-NOW channel (espnow_link.c) */
    }
    static const uint8_t wifi_chans[3] = { 1, 6, 11 };
    return wifi_chans[set_id % 3];
}

#ifndef TEST_HOST

#include "esp_mac.h"     /* esp_read_mac, ESP_MAC_WIFI_STA */
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "rf_pair";

uint16_t rf_compute_set_id(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   /* eFuse read; no WiFi init required */
    uint16_t id = crc16_ccitt(mac, 6);
    if (id == 0x0000 || id == 0xFFFF) {
        id = 0x0001;   /* spec §2.1 guard: never use the reserved sentinels */
    }
    return id;
}

void rf_apply_set_id(rf_radio_cfg_t *cfg, uint16_t set_id, uint8_t slot)
{
    uint8_t addr[4];
    uint8_t suffix;
    uint8_t channel;
    if (!rf_derive_addr(set_id, slot, addr, &suffix, &channel)) {
        return;   /* sentinel → leave cfg at factory defaults */
    }
    cfg->rx_addr[0] = addr[0];
    cfg->rx_addr[1] = addr[1];
    cfg->rx_addr[2] = addr[2];
    cfg->rx_addr[3] = addr[3];
    cfg->addr_suffix = suffix;
    cfg->channel = channel;
}

uint16_t rf_pairing_load_set_id_dongle(void)
{
    nvs_handle_t h;
    if (nvs_open(RF_STORAGE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no rf NVS namespace — factory defaults");
        return 0;
    }
    uint8_t paired_count = 0;
    esp_err_t e = nvs_get_u8(h, "paired_count", &paired_count);
    nvs_close(h);
    if (e != ESP_OK || paired_count == 0) {
        ESP_LOGI(TAG, "paired_count=%u — factory defaults", (unsigned)paired_count);
        return 0;
    }
    uint16_t id = rf_compute_set_id();
    ESP_LOGI(TAG, "paired_count=%u set_id=0x%04X", (unsigned)paired_count, id);
    return id;
}

uint16_t rf_pairing_load_set_id_half(uint8_t fallback_slot, uint8_t *slot_out)
{
    *slot_out = fallback_slot;   /* default to the board factory suffix */

    nvs_handle_t h;
    if (nvs_open(RF_STORAGE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no rf NVS namespace — factory defaults");
        return 0;
    }
    uint16_t set_id = 0;
    esp_err_t e = nvs_get_u16(h, "set_id", &set_id);
    if (e != ESP_OK || set_id == 0) {
        nvs_close(h);
        ESP_LOGI(TAG, "no set_id in NVS — factory defaults");
        return 0;
    }
    if (set_id == 0xFFFF) set_id = 0x0001;   /* guard, same as dongle */

    uint8_t slot = 0;
    if (nvs_get_u8(h, "slot", &slot) == ESP_OK && (slot == 0x01 || slot == 0x02)) {
        *slot_out = slot;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "set_id=0x%04X slot=0x%02X", set_id, *slot_out);
    return set_id;
}

#endif /* TEST_HOST */
```

### Step 1.4 — Write `test/test_rf_pairing.c`

Write the test BEFORE confirming the implementation compiles (TDD discipline). It includes
`rf_pairing.h` only (which under `-DTEST_HOST` exposes ONLY the pure functions — no
`rf_driver.h`). The factory-fallback test exercises `rf_derive_addr` returning false for the
sentinel and asserts a caller's factory bytes remain unchanged.

```c
/*
 * test_rf_pairing.c — Host tests for the pure per-set derivations (Plan RF-1).
 *
 * Contract under test (spec §2.2, §2.3, §2.4, §2.5, §10.1):
 *   - rf_derive_addr(set_id, slot, ...) → {'K','S',hi,lo}, suffix=slot,
 *     channel = 80 + 2*(set_id%20) for slot 0x01, +1 for slot 0x02.
 *   - rf_derive_addr returns false for set_id 0 and 0xFFFF (sentinel) and the
 *     caller's factory cfg is left UNCHANGED (backwards-compat invariant).
 *   - rf_derive_wifi_ch → {1,6,11}[set_id%3]; sentinel → 6.
 *   - crc16_ccitt pinned to a known vector.
 *
 * Run: cd test/build && cmake .. && make && ./test_runner
 */
#include "test_framework.h"
#include "rf_pairing.h"

/* Mirror of the firmware factory defaults (board_rf.h / board.h) used to prove
 * the set_id=0 fallback leaves a cfg byte-for-byte at factory values. */
typedef struct {
    uint8_t rx_addr[4];
    uint8_t addr_suffix;
    uint8_t channel;
} fake_cfg_t;

/* Local re-implementation of the rf_apply_set_id no-op contract using the pure
 * rf_derive_addr (the firmware rf_apply_set_id wraps exactly this). */
static void apply(fake_cfg_t *c, uint16_t set_id, uint8_t slot)
{
    uint8_t addr[4], suffix, ch;
    if (!rf_derive_addr(set_id, slot, addr, &suffix, &ch)) return; /* sentinel no-op */
    c->rx_addr[0] = addr[0]; c->rx_addr[1] = addr[1];
    c->rx_addr[2] = addr[2]; c->rx_addr[3] = addr[3];
    c->addr_suffix = suffix;
    c->channel = ch;
}

void test_rf_pairing(void)
{
    TEST_SUITE("rf_pairing");

    /* ── Case 1: set_id=0 → no-op; factory LEFT cfg unchanged ───── */
    fake_cfg_t l = { {'K','a','S','e'}, 0x01, 0x4C };
    apply(&l, 0x0000, 0x01);
    TEST_ASSERT_EQ(l.rx_addr[0], 'K',  "set_id0 L: rx_addr[0] unchanged");
    TEST_ASSERT_EQ(l.rx_addr[1], 'a',  "set_id0 L: rx_addr[1] unchanged");
    TEST_ASSERT_EQ(l.rx_addr[2], 'S',  "set_id0 L: rx_addr[2] unchanged");
    TEST_ASSERT_EQ(l.rx_addr[3], 'e',  "set_id0 L: rx_addr[3] unchanged");
    TEST_ASSERT_EQ(l.addr_suffix, 0x01, "set_id0 L: suffix stays 0x01");
    TEST_ASSERT_EQ(l.channel, 0x4C,    "set_id0 L: channel stays 76");

    /* ── Case 2: set_id=0 → no-op; factory RIGHT cfg unchanged ──── */
    fake_cfg_t r = { {'K','a','S','e'}, 0x02, 0x52 };
    apply(&r, 0x0000, 0x02);
    TEST_ASSERT_EQ(r.rx_addr[3], 'e',  "set_id0 R: rx_addr[3] unchanged");
    TEST_ASSERT_EQ(r.addr_suffix, 0x02, "set_id0 R: suffix stays 0x02");
    TEST_ASSERT_EQ(r.channel, 0x52,    "set_id0 R: channel stays 82");

    /* ── Case 3: set_id=0xFFFF → also a no-op (same sentinel) ───── */
    fake_cfg_t s = { {'K','a','S','e'}, 0x01, 0x4C };
    apply(&s, 0xFFFF, 0x01);
    TEST_ASSERT_EQ(s.channel, 0x4C,    "set_id 0xFFFF: channel unchanged");
    TEST_ASSERT_EQ(s.addr_suffix, 0x01, "set_id 0xFFFF: suffix unchanged");

    /* ── Case 4: known set_id 0x1234 LEFT → addr + channel 80 ──── */
    uint8_t addr[4], suf, ch;
    bool ok = rf_derive_addr(0x1234, 0x01, addr, &suf, &ch);
    TEST_ASSERT(ok,                    "0x1234 L: derive returns true");
    TEST_ASSERT_EQ(addr[0], 0x4B,      "0x1234 L: addr[0]='K'");
    TEST_ASSERT_EQ(addr[1], 0x53,      "0x1234 L: addr[1]='S'");
    TEST_ASSERT_EQ(addr[2], 0x12,      "0x1234 L: addr[2]=set_id hi");
    TEST_ASSERT_EQ(addr[3], 0x34,      "0x1234 L: addr[3]=set_id lo");
    TEST_ASSERT_EQ(suf, 0x01,          "0x1234 L: suffix=0x01");
    TEST_ASSERT_EQ(ch, 80,             "0x1234 L: channel=80+2*(4660%20)=80");

    /* ── Case 5: known set_id 0x1234 RIGHT → channel base+1 ─────── */
    ok = rf_derive_addr(0x1234, 0x02, addr, &suf, &ch);
    TEST_ASSERT(ok,                    "0x1234 R: derive returns true");
    TEST_ASSERT_EQ(suf, 0x02,          "0x1234 R: suffix=0x02");
    TEST_ASSERT_EQ(ch, 81,             "0x1234 R: channel=81");

    /* ── Case 6: set_id%20==19 → base_ch=118, R=119 ≤ 125 ──────── */
    ok = rf_derive_addr(0x0013 /*19*/, 0x02, addr, &suf, &ch);
    TEST_ASSERT(ok,                    "id19 R: derive returns true");
    TEST_ASSERT_EQ(ch, 119,            "id19 R: channel=118+1=119");
    TEST_ASSERT(ch <= 125,             "id19 R: channel within NRF24 max 125");

    /* ── Case 7: set_id%20==0 (id=20) → base_ch=80 ─────────────── */
    ok = rf_derive_addr(0x0014 /*20*/, 0x01, addr, &suf, &ch);
    TEST_ASSERT_EQ(ch, 80,             "id20 L: channel=80");

    /* ── Case 8: WiFi channel derivation {1,6,11}[id%3] ────────── */
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0x0003), 1,  "wifi: id%3==0 → 1");
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0x0001), 6,  "wifi: id%3==1 → 6");
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0x0002), 11, "wifi: id%3==2 → 11");
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0x1234), 6,  "wifi: 4660%3==1 → 6");
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0x0000), 6,  "wifi: sentinel 0 → 6");
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0xFFFF), 6,  "wifi: sentinel 0xFFFF → 6");

    /* ── Case 9: crc16_ccitt pinned vector ("123456789" → 0x29B1) ─ */
    /* Standard CRC-16/CCITT-FALSE check value per the algorithm catalogue. */
    const uint8_t check[9] = {'1','2','3','4','5','6','7','8','9'};
    TEST_ASSERT_EQ(crc16_ccitt(check, 9), 0x29B1, "crc16: check vector 0x29B1");

    /* ── Case 10: crc16 of a sample 6-byte MAC is deterministic ── */
    const uint8_t mac[6] = {0x24,0x6F,0x28,0x01,0x02,0x03};
    uint16_t a = crc16_ccitt(mac, 6);
    uint16_t b = crc16_ccitt(mac, 6);
    TEST_ASSERT_EQ(a, b,               "crc16: deterministic for same MAC");
}
```

### Step 1.5 — Add to `test/CMakeLists.txt`

In the `add_executable(test_runner ...)` source list, add (after `test_trackpad_map.c` /
`../main/periph/trackpad/trackpad.c`):

```cmake
    test_rf_pairing.c
    ../main/comm/rf/rf_pairing.c
```

`../main/comm/rf` is already in `target_include_directories` (used by `test_rf_packet.c`), so
`#include "rf_pairing.h"` resolves with no further change.

### Step 1.6 — Add to `test/test_main.c`

```c
/* Add declaration with the other externs: */
extern void test_rf_pairing(void);

/* Add call after test_trackpad_map(): */
test_rf_pairing();
```

### Step 1.7 — Run host tests — must pass before Tasks 2 & 3

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -15
./test_runner 2>&1 | grep -E "rf_pairing|FAIL|passed|failed"
```

Expected: all `rf_pairing` cases pass; results line shows `0 failed`, count ≥ 900 (890
baseline + new cases).

Common failure modes:
- `rf_pairing.h: rf_driver.h: No such file` → confirm the `#include "rf_driver.h"` is INSIDE
  `#ifndef TEST_HOST` in `rf_pairing.h`. Under `-DTEST_HOST` the host build must never see it.
- `undefined reference to rf_derive_addr` → confirm `rf_derive_addr` is OUTSIDE
  `#ifndef TEST_HOST` in `rf_pairing.c`, and `rf_pairing.c` is in the CMake source list.
- `crc16: check vector` fails → the CRC variant drifted. The pinned value 0x29B1 is
  CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no final xor). Re-check the loop.

Do not proceed if any case fails. Fix `rf_pairing.c` (not the test).

### Step 1.8 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/rf_pairing.h main/comm/rf/rf_pairing.c \
        test/test_rf_pairing.c test/CMakeLists.txt test/test_main.c
git commit -m "feat(rf): rf_pairing pure derivations + host tests (set_id=0 fallback, TDD)"
```

---

## Task 2: Compile `rf_pairing.c` in firmware (dongle + half) — CMake gate, build green

**Rationale:** Before wiring the calls, ensure the new translation unit links cleanly into
both RF builds. This isolates a pure-compile failure from a wiring failure.

**Files:**
- Modify: `main/CMakeLists.txt`

### Step 2.1 — Inspect the existing RF source gating

```bash
grep -n "KASE_HAS_RF_RX\|KASE_HAS_RF_TX\|rf_rx_task.c\|half_scan_task.c\|comm/rf" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/CMakeLists.txt
```

Identify the `if(CONFIG_KASE_HAS_RF_RX)` block (dongle: `rf_rx_task.c`, `rf_driver.c`,
`dongle_engine_state.c`, etc.) and the `if(CONFIG_KASE_HAS_RF_TX)` block (half:
`half_scan_task.c`, `half_spi.c`).

### Step 2.2 — Add `rf_pairing.c` for both RF roles

`rf_pairing.c` is needed by BOTH the dongle (RF_RX) and the half (RF_TX). Add a single block
(place it near the RF blocks; the disjunction avoids a double `list(APPEND)` if both flags
were ever set, which they are not):

```cmake
# Per-set NRF addressing + pairing (Plan RF-1/RF-2) — dongle (RX) and half (TX)
if(CONFIG_KASE_HAS_RF_RX OR CONFIG_KASE_HAS_RF_TX)
    list(APPEND srcs "comm/rf/rf_pairing.c")
endif()
```

`comm/rf` is already in `INCLUDE_DIRS` (it holds `rf_driver.h`, `rf_packet.h`). No include-dir
change needed. `esp_mac.h` is part of `esp_hw_support`, and `nvs.h` from `nvs_flash` — both
are already transitive requirements of the dongle/half builds (NVS is used elsewhere). If a
link error names `esp_read_mac`, add `esp_hw_support` to `priv_requires`; if `nvs_open` is
missing, add `nvs_flash`.

### Step 2.3 — Build dongle (compiles rf_pairing.c, no callers yet)

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

### Step 2.4 — Build half_left

```bash
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

Common failure modes:
- `undefined reference to esp_read_mac` → add `esp_hw_support` to `priv_requires`.
- `undefined reference to nvs_open` → add `nvs_flash` to `priv_requires`.
- `rf_driver.h not found` when compiling `rf_pairing.c` → `comm/rf` missing from INCLUDE_DIRS
  (it should already be present for the existing RF sources).

### Step 2.5 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/CMakeLists.txt
git commit -m "build(rf): compile rf_pairing.c for dongle (RX) and half (TX)"
```

---

## Task 3: Wire `rf_apply_set_id` into the dongle RX cfgs (`rf_rx_start`)

**Files:**
- Modify: `main/comm/rf/rf_rx_task.c`

### Step 3.1 — Confirm the cfg construction site

```bash
grep -n "board_rf_left_cfg\|board_rf_right_cfg\|rf_driver_init\|#include" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/rf_rx_task.c | head -30
```

Confirm `rf_rx_start()` has, in order:
```c
rf_radio_cfg_t lcfg = board_rf_left_cfg();
rf_radio_cfg_t rcfg = board_rf_right_cfg();
rf_driver_init(&s_left, &lcfg);
rf_driver_init(&s_right, &rcfg);
```

### Step 3.2 — Add the include

In the include block at the top of `rf_rx_task.c` (after `#include "board_rf.h"`), add:

```c
#include "rf_pairing.h"   /* rf_pairing_load_set_id_dongle, rf_apply_set_id */
```

### Step 3.3 — Patch the cfgs between construction and init

Replace:

```c
    rf_radio_cfg_t lcfg = board_rf_left_cfg();
    rf_radio_cfg_t rcfg = board_rf_right_cfg();
    rf_driver_init(&s_left, &lcfg);
    rf_driver_init(&s_right, &rcfg);
```

With:

```c
    rf_radio_cfg_t lcfg = board_rf_left_cfg();
    rf_radio_cfg_t rcfg = board_rf_right_cfg();

    /* Per-set addressing (Plan RF-1): if this dongle is paired (NVS rf.paired_count
     * > 0), derive a unique address+channel from its own WiFi MAC. If unpaired,
     * rf_pairing_load_set_id_dongle() returns 0 and rf_apply_set_id is a no-op,
     * so lcfg/rcfg keep the board factory defaults (KaSe.01/.02, ch 76/82). */
    uint16_t set_id = rf_pairing_load_set_id_dongle();
    rf_apply_set_id(&lcfg, set_id, 0x01);   /* left  → slot 0x01 */
    rf_apply_set_id(&rcfg, set_id, 0x02);   /* right → slot 0x02 */

    rf_driver_init(&s_left, &lcfg);
    rf_driver_init(&s_right, &rcfg);
```

No other change. The IRQ-attach block below still uses `lcfg.pin_irq` / `rcfg.pin_irq` (pins
are untouched by `rf_apply_set_id`).

### Step 3.4 — Build dongle

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

### Step 3.5 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/rf_rx_task.c
git commit -m "feat(dongle): apply per-set NRF address+channel in rf_rx_start (NVS-gated)"
```

---

## Task 4: Wire `rf_apply_set_id` into the half TX cfg (`half_scan_task`)

**Files:**
- Modify: `main/comm/rf/half_scan_task.c`

### Step 4.1 — Confirm the cfg construction site

```bash
grep -n "board_nrf_cfg\|rf_driver_init_tx\|BOARD_NRF_ADDR_SUFFIX\|half_spi.h\|#include" \
    /home/mae/Documents/GitHub/KaSe_firmware/main/comm/rf/half_scan_task.c | head -30
```

Confirm the task body has:
```c
rf_radio_cfg_t nrf_cfg = board_nrf_cfg();
esp_err_t err = rf_driver_init_tx(&s_radio, &nrf_cfg);
```
and that `board_nrf_cfg()` seeds `addr_suffix = BOARD_NRF_ADDR_SUFFIX`.

### Step 4.2 — Add the include

In the `#ifndef TEST_HOST` include block (after `#include "half_spi.h"`), add:

```c
#include "rf_pairing.h"      /* rf_pairing_load_set_id_half / rf_apply_set_id */
```

This is correctly inside `#ifndef TEST_HOST`, so the host build of `half_scan_task.c` (which
is in the test CMake list for `half_diff_emit`) never sees `rf_pairing.h`'s firmware section.

### Step 4.3 — Patch the cfg between construction and init

Replace:

```c
    /* Initialize NRF24L01+ in PTX mode */
    rf_radio_cfg_t nrf_cfg = board_nrf_cfg();
    esp_err_t err = rf_driver_init_tx(&s_radio, &nrf_cfg);
```

With:

```c
    /* Initialize NRF24L01+ in PTX mode */
    rf_radio_cfg_t nrf_cfg = board_nrf_cfg();

    /* Per-set addressing (Plan RF-1): if this half is paired (NVS rf.set_id set),
     * derive its address+channel from the stored set_id and slot. If unpaired,
     * rf_pairing_load_set_id_half() returns 0 with *slot = BOARD_NRF_ADDR_SUFFIX,
     * and rf_apply_set_id is a no-op → nrf_cfg keeps the board factory defaults
     * (KaSe.<suffix>, ch from BOARD_NRF_CHANNEL). */
    uint8_t slot = BOARD_NRF_ADDR_SUFFIX;
    uint16_t set_id = rf_pairing_load_set_id_half(BOARD_NRF_ADDR_SUFFIX, &slot);
    rf_apply_set_id(&nrf_cfg, set_id, slot);

    esp_err_t err = rf_driver_init_tx(&s_radio, &nrf_cfg);
```

No other change. `board_nrf_cfg()` remains the source of all other cfg fields (SPI pins, etc.).

### Step 4.4 — Build half_left

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

### Step 4.5 — Build half_right

```bash
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.` (right inherits `BOARD_NRF_ADDR_SUFFIX = 0x02`, channel
`0x52` — the fallback slot is 0x02, so an unpaired right half keeps `KaSe.02` / ch 82.)

### Step 4.6 — Commit

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/half_scan_task.c
git commit -m "feat(half): apply per-set NRF address+channel in half_scan_task (NVS-gated)"
```

---

## Task 5: Final verification — 3 RF boards green + host tests green

**Files:** none changed unless a build error forces a fix.

### Step 5.1 — Build dongle (clean)

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

### Step 5.2 — Build half_left (clean)

```bash
rm -f sdkconfig
idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

### Step 5.3 — Build half_right (clean)

```bash
rm -f sdkconfig
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
    | grep -iE "Project build complete|error:" | head
```

Expected: `Project build complete.`

### Step 5.4 — Run host tests

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner
```

Expected: `0 failed`, count ≥ 900 (890 baseline + new `rf_pairing` cases). If the count
dropped below 890, a test was silently removed — investigate before proceeding.

### Step 5.5 — Commit if any files were adjusted during Task 5

If nothing changed (everything already committed in T1–T4): no commit needed. If a build
error forced a fix:

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add <fixed-file>
git commit -m "fix(rf): <short description of fix>"
```

---

## Self-review checklist

- [ ] `rf_derive_addr`, `rf_derive_wifi_ch`, `crc16_ccitt` are OUTSIDE `#ifndef TEST_HOST` in
  `rf_pairing.c` — host tests link them.
- [ ] `rf_apply_set_id`, `rf_compute_set_id`, the NVS loaders, AND `#include "rf_driver.h"`
  are INSIDE `#ifndef TEST_HOST` — the host parse never sees `rf_radio_cfg_t` / `esp_err.h`.
- [ ] `rf_apply_set_id(cfg, 0, slot)` is a no-op (delegates to `rf_derive_addr` returning
  false); the host test asserts factory LEFT and RIGHT cfgs are byte-for-byte unchanged.
- [ ] `set_id == 0xFFFF` is ALSO a no-op (sentinel) — separate host case present.
- [ ] Channel formula `80 + 2*(set_id%20)`, slot 0x01 → base_ch, else base_ch+1; max channel
  (id%20==19, right) = 119 ≤ 125 — asserted in a host case.
- [ ] Address base is `{'K','S', hi, lo}` (0x4B, 0x53) for paired sets — NOT `{'K','a','S','e'}`.
  The factory `{'K','a','S','e'}` is only preserved via the set_id=0 no-op.
- [ ] WiFi channel `{1,6,11}[set_id%3]`, sentinel → 6 — asserted.
- [ ] `crc16_ccitt` pinned to 0x29B1 ("123456789") — guards against algorithm drift.
- [ ] `rf_compute_set_id` applies the 0/0xFFFF → 0x0001 guard (spec §2.1).
- [ ] Dongle: `rf_pairing_load_set_id_dongle()` gates on NVS `rf.paired_count`; returns 0 when
  absent/0 → factory defaults preserved.
- [ ] Half: `rf_pairing_load_set_id_half()` gates on NVS `rf.set_id`; `*slot_out` defaults to
  `BOARD_NRF_ADDR_SUFFIX`; returns 0 when absent/0 → factory defaults preserved.
- [ ] NVS namespace is `"rf"` (`RF_STORAGE_NAMESPACE`), NOT `"storage"`.
- [ ] `rf_pairing.c` compiled for both RF_RX and RF_TX; keyboards V1/V2/V2D (neither flag)
  do NOT compile it — keyboards unaffected.
- [ ] All three RF boards (dongle, half_left, half_right) build green with `Project build
  complete.`
- [ ] Host tests `0 failed`, count ≥ 900.

---

## Out of scope / deferred to Plan RF-2 and Plan 3

| Item | Reason |
|------|--------|
| Pairing flow (PKT_PAIR_REQ/ACK, KS_CMD_RF_PAIR_START, BOOT button) | Plan RF-2 — RF-1 only ships derivation + NVS read |
| Writing NVS `rf.set_id`/`rf.slot`/`rf.paired_count`/`mac_*` | Plan RF-2 — RF-1 only READS NVS |
| Runtime address hot-switch (`rf_driver_set_address`) | Plan RF-2 — RF-1 applies once at init |
| `rf_derive_wifi_ch` consumption (`esp_wifi_set_channel`, peer registration) | Plan 3 (ESP-NOW) — RF-1 only ships the pure function + test |
| Live e-ink layer display | Plan 3 |
| CRC-16 collision handling beyond the 0/0xFFFF guard | Spec §9 accepts 1/65536; address filtering is the primary isolation |
| Bench validation (two sets side by side, no cross-talk) | Spec §10.2 — after RF-2 lands the full flow |
