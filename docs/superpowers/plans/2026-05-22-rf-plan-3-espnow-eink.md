# Plan RF-3 — ESP-NOW Unicast Peers + Live E-ink Layer Display

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the ESP-NOW info channel end-to-end so that layer changes on the dongle appear on both halves' SSD1681 e-ink panels within 2 seconds. Four concrete deliverables: (1) real peer registration from NVS pairing data; (2) sender-MAC filtering in the recv callback; (3) `layer_changed()` unicasts `EN_INFO_LAYER` + `EN_INFO_STATE` to both halves; (4) the e-ink LVGL task wakes on notify and renders the current layer name dynamically. End state: flash dongle + either half; change layer via the controller; the e-ink panel displays the layer name, not the static "KaSe". All three boards build green. Host test baseline stays at 890 pass.

**Architecture overview:**

```
[Dongle — rf_rx_task context]
  run_engine_cycle() → layer_changed()
    ├── lazy-load mac_left / mac_right from NVS "rf" (once, static flag)
    ├── en_layer_t { layer_idx, name[16] }
    ├── en_state_t { modifiers, flags=0 }
    ├── espnow_send(mac_left,  EN_INFO_LAYER, &l, sizeof l)   ← unicast
    ├── espnow_send(mac_left,  EN_INFO_STATE, &s, sizeof s)
    ├── espnow_send(mac_right, EN_INFO_LAYER, &l, sizeof l)
    └── espnow_send(mac_right, EN_INFO_STATE, &s, sizeof s)
         (no-op if MAC is all-zeros → graceful unpaired)

[espnow_link.c — espnow_link_init()]
  derive wifi_ch from set_id (NVS "rf".paired_count > 0)
  esp_wifi_set_channel(wifi_ch, WIFI_SECOND_CHAN_NONE)
  load mac_left/mac_right (dongle) or mac_dongle (half) from NVS "rf"
  esp_now_add_peer() for each non-zero MAC
  store paired peer MACs in static array for recv filter

[espnow_link.c — espnow_recv_cb()]
  is_known_peer(src_addr) → drop if not in paired peer list
  espnow_info_dispatch(mac, buf, len)

[Half — ESP-NOW recv task context]
  espnow_info_dispatch() → on_layer() / on_state()
    half_state_lock()
    g_half_state.layer_idx / layer_name / modifiers / flags
    half_state_unlock()
    xTaskNotify(eink_get_task_handle(), 0x01, eSetBits)

[Half — eink_lvgl_task]
  xTaskNotifyWait(sleep_ms) → notified
    half_state_lock() → copy layer_name → half_state_unlock()
    lv_label_set_text(s_label_layer, name_copy)
    lv_obj_invalidate(s_label_layer)
  lv_timer_handler() → flush_cb → eink_push()
```

**Spec reference:** `docs/superpowers/specs/2026-05-22-rf-pairing-addressing-design.md` §§2.5, 6, 7 — read in full before touching any file.

**Depends on:**
- Plan RF-2 (separate): writes NVS keys `rf.mac_left`, `rf.mac_right` (dongle) and `rf.mac_dongle`, `rf.set_id` (half) during pairing. This plan **reads** those keys. If they are absent (unpaired), the plan degrades gracefully — no peers are added, ESP-NOW sends are skipped, and the e-ink falls back to the static "KaSe" label.
- Plan RF-1 (separate): provides `set_id` derivation logic (`rf_compute_set_id`, `rf_apply_set_id`). Plan RF-3 calls `rf_compute_set_id()` internally for the WiFi channel derivation.
- Plan Bricks-3 (done): `eink_lvgl.c` exists and is hardware-validated with the static "KaSe" + version screen; `half_spi_lock/unlock` contract is in place; `eink_push()` is real.

**Graceful-degrades behavior (unpaired state):**
- `espnow_link_init()` reads `paired_count` from NVS "rf". If absent or 0: use WiFi channel 6 (current default), add no peers, log `LOGW("no paired peers — ESP-NOW send disabled")`.
- `layer_changed()`: both `mac_left` and `mac_right` are all-zeros → neither unicast fires → log `LOGD("layer_changed: no paired halves")`.
- Half `on_layer()`: never reached (no packet arrives). The e-ink continues to show the static "KaSe" text until the first `EN_INFO_LAYER` packet arrives.
- All three code paths are no-ops without asserts or panics. The keyboard continues to function normally.

**Build command (always `rm -f sdkconfig` when switching boards; run from repo root):**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_<target> -DBOARD=kase_<target> -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

**Build targets:**
- `idf.py -B build_dongle     -DBOARD=kase_dongle     -DIDF_TARGET=esp32s3 build`
- `idf.py -B build_half_left  -DBOARD=kase_half_left  -DIDF_TARGET=esp32s3 build`
- `idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build`

**Host test command (baseline 890 pass, 0 failed — must stay at 0 failed, grow in count):**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -5 && ./test_runner
```

---

## Critical implementation traps — read before touching any file

**Trap 1 — `current_modifiers` is `static` in `hid_report.c`.**
The spec stub in `dongle_engine_state.c` says `extern uint8_t current_modifiers`. This will not link: `current_modifiers` is declared `static` in `hid_report.c` and is not exported in `hid_report.h`. The correct approach: add a small getter `uint8_t hid_report_get_modifiers(void)` to `hid_report.c` and declare it in `hid_report.h`. Do not de-staticize the variable — it is an internal implementation detail.

**Trap 2 — `esp_now_add_peer()` requires the WiFi channel to match the current channel.**
`esp_wifi_set_channel()` MUST be called before `esp_now_add_peer()`. The peer's `.channel` field in `esp_now_peer_info_t` must equal the channel set with `esp_wifi_set_channel()`. If they diverge, `esp_now_add_peer()` returns `ESP_ERR_ESPNOW_ARG`. Call `esp_wifi_set_channel()` first, store `derived_wifi_ch` in a local, use it for both `esp_wifi_set_channel` and every `peer.channel` field.

**Trap 3 — `is_known_peer()` filter: static array initialized once in `espnow_link_init()`.**
The recv callback runs from the esp_now internal task. Accessing the peer-MAC array from the callback requires it to be safe for concurrent read after `espnow_link_init()` completes. Use a `static` array of up to 2 MACs (dongle) or 1 MAC (half), populated in `espnow_link_init()` before `esp_now_register_recv_cb()`. After registration, the array is read-only from the callback — no mutex needed.

**Trap 4 — `xTaskNotify` from the ESP-NOW recv task into the LVGL task: correct usage.**
`xTaskNotify(handle, 0x01, eSetBits)` is safe from any task (not ISR). It sets bit 0 in the task's notification value. `xTaskNotifyWait(0, 0xFFFFFFFF, &val, timeout_ticks)` clears all bits after read. The eink task must call `xTaskNotifyWait` (not `ulTaskNotifyTake`) because it needs the bits to identify the event type. Never call `xTaskNotify` with a NULL handle — always guard with `if (h != NULL)`.

**Trap 5 — LVGL label update must happen inside `eink_lvgl_task` only.**
`lv_label_set_text()` is NOT thread-safe in raw LVGL v8 (no `esp_lvgl_port` mutex here). The ESP-NOW callback task must NOT call `lv_label_set_text()` directly. The pattern: callback → write to `g_half_state` (under mutex) → `xTaskNotify` → `eink_lvgl_task` wakes → reads `g_half_state` (under mutex) → calls `lv_label_set_text()`. This plan follows this pattern throughout.

**Trap 6 — SPI-lock contract: BUSY wait is OUTSIDE `half_spi_lock`.**
`eink_push()` releases `half_spi_lock` before the long BUSY poll (~1–2 s). This contract is already implemented and hardware-validated. Do not add any new `half_spi_lock` calls inside the eink task's notify handler. The label update (LVGL call) happens before the next `lv_timer_handler()` which eventually calls `flush_cb` → `eink_push()`. No lock changes needed.

**Trap 7 — `default_layout_names` bounds: `MAX_LAYOUT_NAME_LENGTH = 15` (NOT 16).**
`en_layer_t.name` is 16 bytes. `default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH]` is 15-byte strings (including NUL). Use `strncpy(l.name, default_layout_names[current_layout], 15)` then `l.name[15] = '\0'` (the 16th byte stays NUL from zero-init). Do not use 16 as the copy length — that would read 1 byte past the NVS-loaded string storage.

**Trap 8 — WiFi channel derivation for unpaired vs paired.**
`wifi_ch = {1, 6, 11}[set_id % 3]` only applies when `paired_count > 0` (dongle) or `set_id != 0` (half). For unpaired, use channel 6 (the current hardcoded default). Derive `set_id` by calling `rf_compute_set_id()` from `rf_pairing.h` (Plan RF-1). If that symbol is not yet available (Plan RF-1 not merged), inline the computation: `esp_read_mac(mac, ESP_MAC_WIFI_STA)` + `crc16_ccitt(mac, 6)`. The plan provides the inline version so RF-3 can land independently.

---

## File Structure

### Modified

| Path | Change |
|------|--------|
| `main/comm/espnow/espnow_link.c` | Replace WiFi-ch NVS read + peer stub with real peer registration + MAC filter static array |
| `main/comm/espnow/espnow_link.h` | Add `is_known_peer()` declaration (or keep it internal — see T1) |
| `main/comm/espnow/espnow_info.c` | Fill `on_layer()` and `on_state()` stubs: update `g_half_state`, notify eink task |
| `main/comm/rf/dongle_engine_state.c` | Fill `layer_changed()` stub: lazy-load MACs, build payloads, unicast ESP-NOW |
| `main/periph/eink/eink_lvgl.c` | Add dynamic `s_label_layer`; convert task loop to `xTaskNotifyWait`; update label on notify |
| `main/periph/eink/eink_lvgl.h` | Add `eink_get_task_handle()` declaration |
| `main/input/hid_report.c` | Add `hid_report_get_modifiers()` getter |
| `main/input/hid_report.h` | Declare `hid_report_get_modifiers()` |

### Created

| Path | Responsibility |
|------|----------------|
| `test/test_espnow_peer_filter.c` | Host tests for the `is_known_peer()` predicate logic (pure function, no FreeRTOS) |

### Modified (test infrastructure)

| Path | Change |
|------|--------|
| `test/CMakeLists.txt` | Add `test_espnow_peer_filter.c` |
| `test/test_main.c` | Add `extern void test_espnow_peer_filter(void)` + call |

### Untouched

```
main/comm/espnow/espnow_msg.h        en_layer_t / en_state_t + encode/decode — already tested
main/comm/espnow/espnow_info.h       half_state_t + lock/unlock — no API changes
main/comm/rf/rf_pairing.h/c          Plan RF-1 — not touched here
main/periph/eink/eink.c              eink_push / eink_init — no changes
boards/kase_*/board.h                GPIO mappings — no changes
partitions.csv                       unchanged
main/idf_component.yml               no new components
```

---

## Task 1: Host TDD — `is_known_peer()` predicate function

**Purpose:** The recv-callback MAC filter is the only new pure-logic component in this plan. Extract it as a testable function before wiring it into the recv callback. All other logic (ESP-NOW init, LVGL label) has ESP-IDF dependencies unsuitable for host tests.

**Files:**
- Modify: `main/comm/espnow/espnow_link.c` (add `is_known_peer()` + `espnow_set_peers()` helper)
- Modify: `main/comm/espnow/espnow_link.h` (declare `espnow_set_peers()` under `TEST_HOST` guard so the test can call it)
- Create: `test/test_espnow_peer_filter.c`
- Modify: `test/CMakeLists.txt`
- Modify: `test/test_main.c`

**Rationale for TDD:** The MAC filter is the isolation mechanism that prevents a neighbouring KaSe set's ESP-NOW frames from reaching this set's halves. An incorrect filter (e.g., wrong byte-comparison length, off-by-one in peer count) silently breaks the isolation guarantee without any build error. Testing it on the host with known MAC vectors catches these bugs before hardware integration.

- [ ] **Step 1: Add `is_known_peer()` and `espnow_set_peers()` to `espnow_link.c`**

Add this block BEFORE any `#ifndef TEST_HOST` guard (both functions are pure logic, no ESP-IDF deps):

```c
/* ── Peer MAC filter — pure logic, host-testable ─────────────────
 *
 * The static array is populated once in espnow_link_init() (before
 * esp_now_register_recv_cb) and then read-only from espnow_recv_cb.
 * No mutex needed: init completes before the callback is registered.
 *
 * Dongle: up to 2 peers (mac_left, mac_right).
 * Half:   up to 1 peer  (mac_dongle).
 * Unpaired (0 peers): all incoming frames are dropped. */
#define ESPNOW_MAX_PEERS 2

static uint8_t s_peer_macs[ESPNOW_MAX_PEERS][6];
static int     s_peer_count = 0;

/* Store up to ESPNOW_MAX_PEERS MAC addresses for recv filtering.
 * Call once in espnow_link_init(), before registering the recv callback.
 * macs: array of `count` MAC addresses (6 bytes each).
 * count: number of valid MACs in `macs` (0 = no peers = drop all). */
void espnow_set_peers(const uint8_t macs[][6], int count)
{
    s_peer_count = 0;
    for (int i = 0; i < count && i < ESPNOW_MAX_PEERS; i++) {
        memcpy(s_peer_macs[i], macs[i], 6);
        s_peer_count++;
    }
}

/* Returns true if `mac` matches any stored peer MAC (exact 6-byte compare).
 * Returns false if no peers configured or MAC is not in the list.
 * Called from espnow_recv_cb — must be fast (linear scan, ≤2 entries). */
bool is_known_peer(const uint8_t mac[6])
{
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(s_peer_macs[i], mac, 6) == 0) return true;
    }
    return false;
}
```

Add to `main/comm/espnow/espnow_link.h`:

```c
/* ── Peer filter API (also used by host tests) ─────────────────── */

/* Populate the static peer-MAC filter table.
 * Must be called before esp_now_register_recv_cb().
 * macs: array of count MAC addresses. count: 0..ESPNOW_MAX_PEERS.
 * All-zero MACs are silently skipped by the caller (espnow_link_init). */
void espnow_set_peers(const uint8_t macs[][6], int count);

/* Returns true if mac matches any registered peer (exact 6-byte compare). */
bool is_known_peer(const uint8_t mac[6]);
```

- [ ] **Step 2: Write `test/test_espnow_peer_filter.c`**

Write the test BEFORE verifying the implementation compiles (TDD).

```c
/*
 * test_espnow_peer_filter.c — Host tests for espnow_link is_known_peer() predicate.
 *
 * Contract:
 *   espnow_set_peers(macs, count): register up to 2 peer MACs.
 *   is_known_peer(mac): exact 6-byte memcmp against each registered MAC.
 *     Returns true on first match; false if count==0 or no match.
 *
 * Isolation guarantee: a foreign MAC (from a different KaSe set) is not in
 * the peer table → is_known_peer returns false → recv dropped.
 */
#define TEST_HOST
#include "test_framework.h"
#include "../main/comm/espnow/espnow_link.h"
#include <string.h>
#include <stdbool.h>

/* Helper: build a MAC from 6 individual bytes */
static void make_mac(uint8_t out[6], uint8_t a, uint8_t b, uint8_t c,
                     uint8_t d, uint8_t e, uint8_t f)
{
    out[0]=a; out[1]=b; out[2]=c; out[3]=d; out[4]=e; out[5]=f;
}

void test_espnow_peer_filter(void)
{
    TEST_SUITE("espnow_peer_filter");

    uint8_t mac_left[6], mac_right[6], mac_foreign[6], mac_zero[6];
    make_mac(mac_left,    0xAA,0xBB,0xCC,0xDD,0xEE,0x01);
    make_mac(mac_right,   0xAA,0xBB,0xCC,0xDD,0xEE,0x02);
    make_mac(mac_foreign, 0x11,0x22,0x33,0x44,0x55,0x66);
    memset(mac_zero, 0x00, 6);

    /* ── Case 1: no peers configured → all MACs rejected ─────── */
    {
        espnow_set_peers(NULL, 0);
        TEST_ASSERT(!is_known_peer(mac_left),    "case1: left rejected (no peers)");
        TEST_ASSERT(!is_known_peer(mac_right),   "case1: right rejected (no peers)");
        TEST_ASSERT(!is_known_peer(mac_foreign), "case1: foreign rejected (no peers)");
    }

    /* ── Case 2: one peer (half role: mac_dongle only) ─────────── */
    {
        const uint8_t peers[1][6] = {{ 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 }};
        espnow_set_peers(peers, 1);
        TEST_ASSERT( is_known_peer(mac_left),    "case2: left accepted (1 peer)");
        TEST_ASSERT(!is_known_peer(mac_right),   "case2: right rejected (1 peer)");
        TEST_ASSERT(!is_known_peer(mac_foreign), "case2: foreign rejected (1 peer)");
    }

    /* ── Case 3: two peers (dongle role: mac_left + mac_right) ──── */
    {
        const uint8_t peers[2][6] = {
            { 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 },
            { 0xAA,0xBB,0xCC,0xDD,0xEE,0x02 },
        };
        espnow_set_peers(peers, 2);
        TEST_ASSERT( is_known_peer(mac_left),    "case3: left accepted  (2 peers)");
        TEST_ASSERT( is_known_peer(mac_right),   "case3: right accepted (2 peers)");
        TEST_ASSERT(!is_known_peer(mac_foreign), "case3: foreign rejected (2 peers)");
    }

    /* ── Case 4: all-zero MAC is not a peer after valid registration ─ */
    {
        const uint8_t peers[1][6] = {{ 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 }};
        espnow_set_peers(peers, 1);
        TEST_ASSERT(!is_known_peer(mac_zero),    "case4: all-zero MAC not a peer");
    }

    /* ── Case 5: MAC with one-byte difference is not a peer ─────── */
    {
        const uint8_t peers[1][6] = {{ 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 }};
        espnow_set_peers(peers, 1);
        uint8_t almost[6];
        make_mac(almost, 0xAA,0xBB,0xCC,0xDD,0xEE,0x00);  /* last byte differs */
        TEST_ASSERT(!is_known_peer(almost),      "case5: off-by-one last byte rejected");
        make_mac(almost, 0xAB,0xBB,0xCC,0xDD,0xEE,0x01);  /* first byte differs */
        TEST_ASSERT(!is_known_peer(almost),      "case5: off-by-one first byte rejected");
    }

    /* ── Case 6: count > ESPNOW_MAX_PEERS is clamped safely ─────── */
    {
        /* Provide 3 MACs but limit is 2 — must not overflow. */
        const uint8_t peers[3][6] = {
            { 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 },
            { 0xAA,0xBB,0xCC,0xDD,0xEE,0x02 },
            { 0x11,0x22,0x33,0x44,0x55,0x66 },   /* overflowing entry */
        };
        espnow_set_peers(peers, 3);
        TEST_ASSERT( is_known_peer(mac_left),    "case6: left accepted (clamped)");
        TEST_ASSERT( is_known_peer(mac_right),   "case6: right accepted (clamped)");
        /* Third entry should NOT be accepted (clamped at ESPNOW_MAX_PEERS=2) */
        TEST_ASSERT(!is_known_peer(mac_foreign), "case6: 3rd entry not accepted (clamped)");
    }

    /* ── Case 7: re-configure peers replaces previous list ───────── */
    {
        /* Register left only, then replace with foreign only. */
        const uint8_t peers_a[1][6] = {{ 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 }};
        espnow_set_peers(peers_a, 1);
        TEST_ASSERT( is_known_peer(mac_left),    "case7a: left accepted before replace");

        const uint8_t peers_b[1][6] = {{ 0x11,0x22,0x33,0x44,0x55,0x66 }};
        espnow_set_peers(peers_b, 1);
        TEST_ASSERT(!is_known_peer(mac_left),    "case7b: left rejected after replace");
        TEST_ASSERT( is_known_peer(mac_foreign), "case7b: foreign accepted after replace");
    }

    /* ── Case 8: exact match all 6 bytes — canonical round-trip ─── */
    {
        uint8_t mac_a[6] = {0xDE,0xAD,0xBE,0xEF,0x00,0x01};
        const uint8_t peers[1][6] = {{0xDE,0xAD,0xBE,0xEF,0x00,0x01}};
        espnow_set_peers(peers, 1);
        TEST_ASSERT( is_known_peer(mac_a),       "case8: canonical 6-byte match accepted");
        mac_a[5] = 0x02;
        TEST_ASSERT(!is_known_peer(mac_a),       "case8: mutated byte rejected");
    }
}
```

- [ ] **Step 3: Add to `test/CMakeLists.txt`**

In the `add_executable(test_runner ...)` source list, add:

```cmake
    test_espnow_peer_filter.c
    ../main/comm/espnow/espnow_link.c
```

In `target_include_directories`, add:

```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/espnow
```

`espnow_link.c` compiled with `-DTEST_HOST`: the `is_known_peer()` and `espnow_set_peers()` functions have no ESP-IDF deps (pure `memcmp`/`memcpy`). All FreeRTOS, WiFi, and ESP-NOW calls in `espnow_link_init()` and `espnow_send()` must be guarded with `#ifndef TEST_HOST`.

- [ ] **Step 4: Guard ESP-IDF-dependent code in `espnow_link.c` for host compilation**

Add `#ifndef TEST_HOST` guards around all includes and functions that use WiFi/ESP-NOW/NVS:

```c
#ifndef TEST_HOST
#include "espnow_link.h"
#include "espnow_info.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#endif /* TEST_HOST */
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ... is_known_peer / espnow_set_peers defined here — no guards (pure) ... */

#ifndef TEST_HOST
static const char *TAG = "espnow_link";

/* ... espnow_recv_cb, espnow_link_init, espnow_send ... */

#endif /* TEST_HOST */
```

Also add `#include "espnow_link.h"` at the top of `espnow_link.h` so the host test can include it standalone without other ESP-IDF headers. The header itself must include only `<stdint.h>` and `<stdbool.h>` at the top; the `esp_now.h` / `esp_wifi.h` includes belong in the `.c` file under `#ifndef TEST_HOST`.

- [ ] **Step 5: Add to `test/test_main.c`**

```c
extern void test_espnow_peer_filter(void);
```

```c
test_espnow_peer_filter();
```

- [ ] **Step 6: Run host tests — must pass before proceeding to Task 2**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -10
./test_runner 2>&1 | grep -E "espnow_peer_filter|FAIL|passed|failed"
```

Expected: all `test_espnow_peer_filter` assertions pass (Cases 1–8). Prior tests stay at 0 failed.

If Case 6 fails: the clamp `i < count && i < ESPNOW_MAX_PEERS` in the for-loop is missing — fix the loop condition in `espnow_set_peers`.

- [ ] **Step 7: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/espnow/espnow_link.c main/comm/espnow/espnow_link.h \
        test/test_espnow_peer_filter.c test/CMakeLists.txt test/test_main.c
git commit -m "test(espnow): is_known_peer + espnow_set_peers — TDD host tests"
```

---

## Task 2: `espnow_link_init()` — real peer registration + WiFi channel derivation

**Files:**
- Modify: `main/comm/espnow/espnow_link.c`

Replace the WiFi-channel NVS lookup and peer-add stub with the real implementation. This task touches only `espnow_link_init()` — not the recv callback or `espnow_send()`.

- [ ] **Step 1: Add the CRC-16/CCITT helper inline to `espnow_link.c`**

This is a local copy of the computation used in `rf_pairing.c` (Plan RF-1). Including it here avoids a cross-component dependency and allows RF-3 to land independently. Add before `espnow_link_init()` inside `#ifndef TEST_HOST`:

```c
/* ── WiFi channel from set_id (spec §2.5) ───────────────────────
 * CRC-16/CCITT: poly=0x1021, init=0xFFFF, no reflect, no final XOR. */
static uint16_t crc16_ccitt_espnow(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Derive WiFi channel from dongle WiFi STA MAC (spec §2.5).
 * Reads eFuse MAC — no WiFi init required.
 * Returns 6 (unpaired default) if NVS paired_count is 0 or absent. */
static uint8_t espnow_derive_wifi_ch(void)
{
    /* Read NVS paired_count to decide whether to apply derivation. */
    uint8_t paired_count = 0;
    nvs_handle_t h;
    if (nvs_open("rf", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "paired_count", &paired_count);
        nvs_close(h);
    }
    if (paired_count == 0) {
        return 6;   /* factory default: 2437 MHz, same as current hardcoded value */
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint16_t set_id = crc16_ccitt_espnow(mac, 6);
    /* Guard reserved sentinels (spec §2.1) */
    if (set_id == 0x0000 || set_id == 0xFFFF) set_id = 0x0001;

    /* {1,6,11}[set_id % 3] — non-overlapping 2.4 GHz WiFi channels (spec §2.5) */
    static const uint8_t ch_table[3] = {1, 6, 11};
    return ch_table[set_id % 3];
}
```

- [ ] **Step 2: Replace the WiFi channel + peer stub block in `espnow_link_init()`**

Remove lines 62–98 of the current `espnow_link_init()` (the NVS `rf.wifi_ch` read block and the `/* TODO STUB */` peer-add block). Replace with:

```c
    /* ── WiFi channel: derived from set_id (paired) or factory default 6 (unpaired).
     * DEPRECATION NOTE: the NVS key "rf.wifi_ch" from the 2026-05-11 dongle spec §6
     * is no longer read. For paired sets, wifi_ch is derived from set_id % 3 → {1,6,11}.
     * For unpaired sets, wifi_ch defaults to 6 (2437 MHz, unchanged from prior behaviour).
     * See rf-pairing-addressing-design.md §2.5 + §6.3 for rationale.
     * IMPORTANT: esp_wifi_set_channel() MUST precede esp_now_add_peer()
     *            (peer.channel must match the current WiFi channel). */
    uint8_t wifi_ch = espnow_derive_wifi_ch();
    esp_wifi_set_channel(wifi_ch, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG, "WiFi channel: %u (2%03u MHz)", wifi_ch, 412 + (uint32_t)wifi_ch * 5);

    /* ── ESP-NOW init + recv callback ─────────────────────────── */
    e = esp_now_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %d", e);
        return false;
    }

    /* ── Load peer MACs from NVS "rf" and register with ESP-NOW ──
     * Dongle role: keys "mac_left" (6B), "mac_right" (6B).
     * Half role:   key "mac_dongle" (6B).
     * If a key is absent or all-zeros: skip that peer — no panic.
     * All-zeros MAC means "not yet paired" (key was written by Plan RF-2 pairing flow). */
    {
        uint8_t tmp_macs[ESPNOW_MAX_PEERS][6];
        int     tmp_count = 0;

        nvs_handle_t nvs_peer;
        if (nvs_open("rf", NVS_READONLY, &nvs_peer) == ESP_OK) {

#if CONFIG_KASE_DEVICE_ROLE_DONGLE
            /* Dongle: load mac_left and mac_right */
            uint8_t mac_l[6] = {0}, mac_r[6] = {0};
            size_t sz = 6;
            nvs_get_blob(nvs_peer, "mac_left",  mac_l, &sz);
            sz = 6;
            nvs_get_blob(nvs_peer, "mac_right", mac_r, &sz);
            if (mac_l[0] || mac_l[1] || mac_l[2] || mac_l[3] || mac_l[4] || mac_l[5]) {
                memcpy(tmp_macs[tmp_count++], mac_l, 6);
            }
            if (mac_r[0] || mac_r[1] || mac_r[2] || mac_r[3] || mac_r[4] || mac_r[5]) {
                memcpy(tmp_macs[tmp_count++], mac_r, 6);
            }
#endif /* CONFIG_KASE_DEVICE_ROLE_DONGLE */

#if CONFIG_KASE_DEVICE_ROLE_HALF
            /* Half: load mac_dongle */
            uint8_t mac_d[6] = {0};
            size_t sz = 6;
            nvs_get_blob(nvs_peer, "mac_dongle", mac_d, &sz);
            if (mac_d[0] || mac_d[1] || mac_d[2] || mac_d[3] || mac_d[4] || mac_d[5]) {
                memcpy(tmp_macs[tmp_count++], mac_d, 6);
            }
#endif /* CONFIG_KASE_DEVICE_ROLE_HALF */

            nvs_close(nvs_peer);
        }

        /* Register with ESP-NOW — must happen after esp_now_init() and esp_wifi_set_channel() */
        esp_now_peer_info_t peer = {
            .channel = wifi_ch,   /* MUST match esp_wifi_set_channel() above */
            .ifidx   = WIFI_IF_STA,
            .encrypt = false,
        };
        for (int i = 0; i < tmp_count; i++) {
            memcpy(peer.peer_addr, tmp_macs[i], 6);
            esp_err_t pe = esp_now_add_peer(&peer);
            if (pe != ESP_OK) {
                ESP_LOGW(TAG, "esp_now_add_peer failed for peer %d: %d", i, pe);
            } else {
                ESP_LOGI(TAG, "ESP-NOW peer %d registered: %02X:%02X:%02X:%02X:%02X:%02X ch=%u",
                         i,
                         tmp_macs[i][0], tmp_macs[i][1], tmp_macs[i][2],
                         tmp_macs[i][3], tmp_macs[i][4], tmp_macs[i][5],
                         wifi_ch);
            }
        }

        /* Populate the recv-callback filter table */
        espnow_set_peers((const uint8_t (*)[6])tmp_macs, tmp_count);

        if (tmp_count == 0) {
            ESP_LOGW(TAG, "no paired peers configured (NVS rf.mac_* absent) — ESP-NOW send disabled");
        }
    }

    /* Register recv callback AFTER filter table is populated */
    esp_now_register_recv_cb(espnow_recv_cb);

    ESP_LOGI(TAG, "ESP-NOW link init OK");
    return true;
```

- [ ] **Step 3: Add MAC filter to `espnow_recv_cb()`**

Replace the existing `espnow_recv_cb` body:

```c
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int data_len)
{
    if (recv_info == NULL || data == NULL || data_len < 1 || data_len > 250) return;

    /* Drop frames from unknown senders (isolation: reject foreign KaSe sets).
     * If no peers configured (unpaired), all frames are dropped here. */
    if (!is_known_peer(recv_info->src_addr)) {
        ESP_LOGD(TAG, "ESP-NOW recv from unknown MAC %02X:%02X:%02X:%02X:%02X:%02X — dropped",
                 recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
                 recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);
        return;
    }

    espnow_info_dispatch(recv_info->src_addr, data, (uint8_t)data_len);
}
```

- [ ] **Step 4: Build dongle — verify green**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

Common failures:
- `implicit declaration of is_known_peer` → `espnow_link.h` declaration missing (check Step 1 header edit).
- `'sz' may be used uninitialized` in the `#if DONGLE` / `#if HALF` block — each `sz = 6` reset is needed before every `nvs_get_blob` call, as shown above.
- `unused variable 'sz'` if only one role block compiles — silence with `(void)sz` after the block inside `#if`.

- [ ] **Step 5: Build half_left + half_right — verify green**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.` for both.

- [ ] **Step 6: Run host tests — baseline must stay green**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner 2>&1 | tail -5
```

Expected: all existing tests pass + the new `test_espnow_peer_filter` cases from Task 1.

- [ ] **Step 7: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/espnow/espnow_link.c main/comm/espnow/espnow_link.h
git commit -m "feat(espnow): real peer registration from NVS + MAC filter in recv_cb"
```

---

## Task 3: `hid_report_get_modifiers()` getter + `layer_changed()` dongle push

**Files:**
- Modify: `main/input/hid_report.c` (add getter)
- Modify: `main/input/hid_report.h` (declare getter)
- Modify: `main/comm/rf/dongle_engine_state.c` (fill `layer_changed()` stub)

**Rationale for Task 3 ordering:** `layer_changed()` needs `current_modifiers` from `hid_report.c`. That variable is `static` and not exported. Adding a getter before implementing `layer_changed()` avoids a linker error.

- [ ] **Step 1: Add `hid_report_get_modifiers()` to `hid_report.c`**

Immediately after the `current_modifiers = modifier;` line (line ~125), or at the end of the file, add:

```c
/* Returns the current HID modifier byte.
 * Used by layer_changed() (dongle) to build EN_INFO_STATE payloads.
 * current_modifiers is updated by send_hid_key() / send_hid_kb_mouse(). */
uint8_t hid_report_get_modifiers(void)
{
    return current_modifiers;
}
```

- [ ] **Step 2: Declare `hid_report_get_modifiers()` in `hid_report.h`**

Add after the `keyboard_get_usb_bl_state()` declaration:

```c
/* Returns the current HID modifier byte (left/right shift/ctrl/alt/GUI).
 * Same encoding as USB HID boot protocol modifier byte. */
uint8_t hid_report_get_modifiers(void);
```

- [ ] **Step 3: Fill `layer_changed()` in `dongle_engine_state.c`**

Replace the entire `layer_changed()` body with:

```c
void layer_changed(void)
{
#if CONFIG_KASE_HAS_ESPNOW
    /* Lazy-load paired half MACs from NVS "rf" on first call.
     * Safe: layer_changed() is always called from rf_rx_task (single task).
     * Static variables are task-local in effect (no concurrent access). */
    static uint8_t mac_left[6]  = {0};
    static uint8_t mac_right[6] = {0};
    static bool    macs_loaded  = false;

    if (!macs_loaded) {
        nvs_handle_t h;
        if (nvs_open("rf", NVS_READONLY, &h) == ESP_OK) {
            size_t sz = 6;
            nvs_get_blob(h, "mac_left",  mac_left,  &sz);
            sz = 6;
            nvs_get_blob(h, "mac_right", mac_right, &sz);
            nvs_close(h);
        }
        macs_loaded = true;
    }

    bool has_left  = mac_left[0]  || mac_left[1]  || mac_left[2]  ||
                     mac_left[3]  || mac_left[4]  || mac_left[5];
    bool has_right = mac_right[0] || mac_right[1] || mac_right[2] ||
                     mac_right[3] || mac_right[4] || mac_right[5];

    if (!has_left && !has_right) {
        ESP_LOGD("dongle_engine", "layer_changed: no paired halves — ESP-NOW skip");
        return;
    }

    /* ── Build EN_INFO_LAYER payload ────────────────────────────
     * default_layout_names: char[LAYERS][MAX_LAYOUT_NAME_LENGTH] (15 bytes each).
     * en_layer_t.name:  char[16]. Copy 15 bytes max; 16th byte stays NUL. */
    en_layer_t l;
    memset(&l, 0, sizeof(l));
    l.layer_idx = current_layout;
    if (current_layout < LAYERS) {
        strncpy(l.name, default_layout_names[current_layout], 15);
        /* l.name[15] is already 0 from memset */
    }

    /* ── Build EN_INFO_STATE payload ────────────────────────────
     * espnow_send() is called from rf_rx_task context — esp_now_send()
     * is internally queued and thread-safe (spec §7.1 rationale). */
    en_state_t s;
    memset(&s, 0, sizeof(s));
    s.modifiers = hid_report_get_modifiers();
    s.flags     = 0;   /* caps_word / bt flags: future expansion (spec §7.1) */

    /* ── Unicast to each paired half — fire-and-forget ──────────
     * Low rate (one per layer change, typically seconds apart).
     * NRF is NOT muted: info channel duty cycle < 0.1% (spec §7.5).
     * espnow_send() returns false gracefully if peer was not added. */
    if (has_left) {
        espnow_send(mac_left, EN_INFO_LAYER, &l, sizeof(l));
        espnow_send(mac_left, EN_INFO_STATE, &s, sizeof(s));
    }
    if (has_right) {
        espnow_send(mac_right, EN_INFO_LAYER, &l, sizeof(l));
        espnow_send(mac_right, EN_INFO_STATE, &s, sizeof(s));
    }
#endif /* CONFIG_KASE_HAS_ESPNOW */
}
```

Also add these includes at the top of `dongle_engine_state.c` (after existing includes):

```c
#if CONFIG_KASE_HAS_ESPNOW
#include "nvs.h"                    /* nvs_open, nvs_get_blob, nvs_close */
#include "hid_report.h"             /* hid_report_get_modifiers() */
#include "keymap.h"                 /* default_layout_names */
#include "keyboard_config.h"        /* LAYERS */
#endif
```

Remove the old partial includes that were only needed for the stub comment:

```c
/* Remove these if present — they were part of the stub comment block:
 *   #include "espnow_link.h"
 *   #include "espnow_msg.h"
 * Replace with the more complete include block above. */
```

The includes `espnow_link.h` and `espnow_msg.h` are still needed — keep them in the `#if CONFIG_KASE_HAS_ESPNOW` block alongside the new ones.

- [ ] **Step 4: Build dongle — verify green**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

Common failures:
- `implicit declaration of hid_report_get_modifiers` → `hid_report.h` not included in `dongle_engine_state.c` under `CONFIG_KASE_HAS_ESPNOW`.
- `implicit declaration of default_layout_names` → `keymap.h` not included.
- `LAYERS undeclared` → `keyboard_config.h` not included.
- `undefined reference to espnow_send` → the existing `espnow_link.h` include must remain.

- [ ] **Step 5: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/input/hid_report.c main/input/hid_report.h \
        main/comm/rf/dongle_engine_state.c
git commit -m "feat(dongle): layer_changed() unicasts EN_INFO_LAYER+STATE to paired halves"
```

---

## Task 4: Half — `on_layer()` / `on_state()` fill stubs + `eink_get_task_handle()`

**Files:**
- Modify: `main/periph/eink/eink_lvgl.c` (save task handle; expose getter)
- Modify: `main/periph/eink/eink_lvgl.h` (declare `eink_get_task_handle()`)
- Modify: `main/comm/espnow/espnow_info.c` (fill `on_layer()`, `on_state()`)

This task implements the receiving side on the half: state update + eink-task wake notification.

- [ ] **Step 1: Save the task handle in `eink_lvgl.c` and expose `eink_get_task_handle()`**

Add a static task-handle variable and its getter. In `eink_lvgl.c`, just before `eink_lvgl_start()`:

```c
/* ── Task handle — exposed for xTaskNotify from espnow_info.c ───
 * Set in eink_lvgl_start() when the task is created.
 * NULL until eink_lvgl_start() is called (checked before every notify). */
static TaskHandle_t s_eink_task_handle = NULL;

TaskHandle_t eink_get_task_handle(void)
{
    return s_eink_task_handle;
}
```

Modify `eink_lvgl_start()` to save the handle:

```c
void eink_lvgl_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        eink_lvgl_task, "eink_lvgl",
        4096,
        NULL,
        3,
        &s_eink_task_handle,   /* save handle here — was NULL before */
        0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed for eink_lvgl_task");
        s_eink_task_handle = NULL;
    }
}
```

- [ ] **Step 2: Declare `eink_get_task_handle()` in `eink_lvgl.h`**

Add after `eink_lvgl_start()`:

```c
/* Returns the FreeRTOS task handle of the eink_lvgl_task, or NULL if not started.
 * Used by espnow_info.c to wake the task when layer/state changes.
 * Call xTaskNotify(eink_get_task_handle(), 0x01, eSetBits) — bit 0 = layer/state event.
 * Always guard with: if (h != NULL) before calling xTaskNotify. */
TaskHandle_t eink_get_task_handle(void);
```

- [ ] **Step 3: Fill `on_layer()` and `on_state()` in `espnow_info.c`**

Replace the stub bodies (the `(void)l;` / `(void)s;` no-ops) with:

```c
#if CONFIG_KASE_DEVICE_ROLE_HALF

/* Add this include inside the HALF role block if not already present */
#include "eink_lvgl.h"    /* eink_get_task_handle() */
#include "freertos/task.h"

static void on_layer(const en_layer_t *l)
{
    ESP_LOGI(TAG, "layer update: idx=%u name='%.16s'", l->layer_idx, l->name);

    /* Update g_half_state under mutex — ESP-NOW recv task context.
     * Timeout 10 ms: if the mutex is held by eink_lvgl_task during label update,
     * we skip this event (next layer change will retry). Acceptable at low rate. */
    if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_half_state.layer_idx = l->layer_idx;
        memcpy(g_half_state.layer_name, l->name, 16);
        xSemaphoreGive(g_half_state_mutex);
    } else {
        ESP_LOGW(TAG, "on_layer: mutex timeout — layer update skipped");
        return;
    }

    /* Wake eink_lvgl_task — bit 0 = layer/state event.
     * Non-blocking (eSetBits): safe from any task context (not ISR). */
    TaskHandle_t h = eink_get_task_handle();
    if (h != NULL) {
        xTaskNotify(h, 0x01, eSetBits);
    }
}

static void on_state(const en_state_t *s)
{
    ESP_LOGD(TAG, "state update: mod=0x%02x flags=0x%02x", s->modifiers, s->flags);

    if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_half_state.modifiers = s->modifiers;
        g_half_state.flags     = s->flags;
        xSemaphoreGive(g_half_state_mutex);
    } else {
        ESP_LOGW(TAG, "on_state: mutex timeout — state update skipped");
        return;
    }

    /* Wake eink_lvgl_task — same bit 0; the task reads both layer_name and modifiers. */
    TaskHandle_t h = eink_get_task_handle();
    if (h != NULL) {
        xTaskNotify(h, 0x01, eSetBits);
    }
}

#endif /* CONFIG_KASE_DEVICE_ROLE_HALF */
```

Remove the old `(void)l;` and `(void)s;` suppressors — they are no longer needed.

- [ ] **Step 4: Build half_left + half_right — verify green**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.` for both.

Common failures:
- `undefined reference to eink_get_task_handle` → `eink_lvgl.h` not included in `espnow_info.c` under `CONFIG_KASE_DEVICE_ROLE_HALF`. Also verify `eink_lvgl.h` is in an include path reachable from `espnow_info.c` — add `main/periph/eink` to the REQUIRES/priv_requires of `main/CMakeLists.txt` if needed (likely already present via `KASE_HAS_EINK`).
- `freertos/task.h not found` → add `freertos/task.h` include inside the `CONFIG_KASE_DEVICE_ROLE_HALF` guard; the header is available in all ESP-IDF 5.x builds.
- `implicit declaration of xTaskNotify` → same, `task.h` missing.

- [ ] **Step 5: Verify dongle build still green**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.` (dongle does not compile `eink_lvgl.c` and does not define `CONFIG_KASE_DEVICE_ROLE_HALF` — no impact).

- [ ] **Step 6: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink_lvgl.c main/periph/eink/eink_lvgl.h \
        main/comm/espnow/espnow_info.c
git commit -m "feat(half): on_layer/on_state fill stubs — update g_half_state + wake eink"
```

---

## Task 5: Live e-ink — dynamic layer label + `xTaskNotifyWait` loop

**Files:**
- Modify: `main/periph/eink/eink_lvgl.c`

This task wires the final link: the eink task wakes on notification, reads the layer name from `g_half_state`, updates the LVGL label, and lets `lv_timer_handler()` trigger a refresh.

**Key invariants to preserve (Trap 6):**
- The existing `flush_cb → eink_push()` render path is unchanged.
- `half_spi_lock` / `half_spi_unlock` ordering in `eink_push()` is not touched.
- `full_refresh = 0` and `set_px_cb`-based packing are not touched.

- [ ] **Step 1: Add `s_label_layer` static pointer and include `espnow_info.h`**

At the top of `eink_lvgl.c`, add the include (inside an existing `#if CONFIG_KASE_HAS_ESPNOW` guard or unconditionally — `espnow_info.h` is safe to include on all half builds):

```c
#include "espnow_info.h"   /* g_half_state, half_state_lock/unlock */
```

Add a static label pointer alongside the existing `s_tick_timer`:

```c
/* ── Dynamic layer-name label ────────────────────────────────────
 * Initialized to "KaSe" in eink_lvgl_init().
 * Updated to g_half_state.layer_name when bit 0 notify fires.
 * Falls back to "KaSe" if layer_name[0] == '\0' (unpaired / no packet yet). */
static lv_obj_t *s_label_layer = NULL;
```

- [ ] **Step 2: Replace the static "KaSe" label in `eink_lvgl_init()` with the dynamic label**

In `eink_lvgl_init()`, find:

```c
    lv_obj_t *label_name = lv_label_create(scr);
    lv_label_set_text(label_name, "KaSe");
    lv_obj_set_style_text_color(label_name, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(label_name, 60, 80);
```

Replace with:

```c
    /* Dynamic layer-name label.
     * Initial text "KaSe" is shown until the first EN_INFO_LAYER packet arrives.
     * Unpaired fallback: layer_name stays empty → "KaSe" is displayed indefinitely.
     * On receipt of EN_INFO_LAYER: on_layer() updates g_half_state, notifies this task,
     * and eink_lvgl_task calls lv_label_set_text() here (inside this task — thread-safe). */
    s_label_layer = lv_label_create(scr);
    lv_label_set_text(s_label_layer, "KaSe");
    lv_obj_set_style_text_color(s_label_layer, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(s_label_layer, 60, 80);
```

The firmware version label (currently at y=100) is preserved unchanged.

- [ ] **Step 3: Convert `eink_lvgl_task()` to `xTaskNotifyWait` loop**

Replace the current `eink_lvgl_task()` body with:

```c
static void eink_lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "eink_lvgl_task started");

    for (;;) {
        /* Run LVGL timers first (drives any pending invalidations from previous notify). */
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms == 0 || sleep_ms > 50) sleep_ms = 50;

        /* Wait for LVGL timer OR notify from on_layer()/on_state() (bit 0 = event). */
        uint32_t notify_val = 0;
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &notify_val,
                                              pdMS_TO_TICKS(sleep_ms));

        if (notified == pdTRUE && (notify_val & 0x01)) {
            /* Layer or state changed — read layer_name under mutex. */
            char name_copy[17] = {0};
            if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                memcpy(name_copy, g_half_state.layer_name, 16);
                xSemaphoreGive(g_half_state_mutex);
            }
            name_copy[16] = '\0';

            /* Fallback: empty name (unpaired or no packet yet) → show "KaSe" */
            if (name_copy[0] == '\0') {
                memcpy(name_copy, "KaSe", 5);
            }

            /* Update LVGL label — safe: we are inside eink_lvgl_task (single LVGL task).
             * lv_obj_is_valid() guards against a display_clear_screen() race (spec pattern). */
            if (s_label_layer != NULL && lv_obj_is_valid(s_label_layer)) {
                lv_label_set_text(s_label_layer, name_copy);
                lv_obj_invalidate(s_label_layer);
                ESP_LOGI(TAG, "eink: layer label updated to '%s'", name_copy);
            }
            /* lv_timer_handler() at the top of the next loop iteration will
             * detect the invalidated region and trigger flush_cb → eink_push(). */
        }

        /* Stack headroom check — log once after first render completes */
        static bool s_stack_checked = false;
        if (!s_stack_checked) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "eink_lvgl_task stack HWM: %u words free", (unsigned)hwm);
            if (hwm < 128) {
                ESP_LOGW(TAG, "STACK LOW — bump eink_lvgl_task stack to 6144");
            }
            s_stack_checked = true;
        }
    }
}
```

**Why this loop structure is correct:**
- `lv_timer_handler()` is called at the START of each iteration. This means any `lv_obj_invalidate()` set in the PREVIOUS iteration is processed immediately on the next wake — not after another `sleep_ms` delay. The label update → invalidate → next-loop `lv_timer_handler()` → flush_cb path completes within one extra loop cycle (~50 ms).
- `xTaskNotifyWait` clears all notification bits on read (`ulBitsToClearOnExit = 0xFFFFFFFF`). Multiple back-to-back layer changes within 50 ms are coalesced — the eink task renders the final state only.
- The `vTaskDelay` that was previously in the loop is removed — `xTaskNotifyWait` with a timeout performs the same blocking.

- [ ] **Step 4: Build half_right (panel target) — verify green**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

Common failures:
- `undefined reference to g_half_state` → `espnow_info.h` not included or its `extern` declaration is missing (the header declares `extern half_state_t g_half_state` — ensure it's included).
- `undefined reference to g_half_state_mutex` → same.
- `implicit declaration of xTaskNotifyWait` → `freertos/task.h` missing (already included in the file via `freertos/FreeRTOS.h` + `freertos/task.h` — verify both are present).
- `implicit declaration of lv_obj_is_valid` → include `lvgl.h` (already present).

- [ ] **Step 5: Build half_left + dongle — verify green**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.` for both.

- [ ] **Step 6: Run host tests — verify all green**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner 2>&1 | tail -5
```

Expected: 0 failed. `eink_lvgl.c` is not compiled in the test runner (LVGL + FreeRTOS deps). The host-testable logic is isolated in Task 1. No new host tests are needed for Tasks 2–5.

- [ ] **Step 7: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink_lvgl.c main/periph/eink/eink_lvgl.h
git commit -m "feat(eink): dynamic layer-name label — xTaskNotifyWait + lv_label_set_text"
```

---

## Task 6: Build-green checkpoint — all three boards

Full rebuild from clean state. This checkpoint must be green before bench validation.

- [ ] **Step 1: Rebuild dongle from clean state**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

- [ ] **Step 2: Rebuild half_left from clean state**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

- [ ] **Step 3: Rebuild half_right from clean state**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

- [ ] **Step 4: Run full host test suite**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner
```

Expected: count ≥ 898 (890 baseline + 8 new cases from `test_espnow_peer_filter`), 0 failed.

---

## Task 7: Bench validation — flash + pair + layer change + e-ink visual check

**This task is hardware-only. No code changes permitted except a stack-size bump if HWM < 512 bytes (identical to Plan Bricks-3 T6 Step 4).**

Hardware required: one dongle + one half (either half_left or half_right with the SSD1681 panel). If pairing data (Plan RF-2) is not yet flashed in NVS, the bench validates the graceful-unpaired path only (steps 1–4); the full end-to-end layer push (steps 5–7) requires Plan RF-2 to have been executed first.

- [ ] **Step 1: Flash dongle**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build && \
  idf.py -B build_dongle -p /dev/ttyUSB0 flash'
```

- [ ] **Step 2: Flash half_right (or half_left — whichever has the panel)**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build && \
  idf.py -B build_half_right -p /dev/ttyUSB1 flash monitor'
```

- [ ] **Step 3: Verify boot log — unpaired path (no NVS pairing data)**

Expected half console output:

```
I espnow_info: espnow_info_init OK (half role)
I espnow_link: WiFi channel: 6 (2437 MHz)      ← factory default, not derived
W espnow_link: no paired peers configured (NVS rf.mac_* absent) — ESP-NOW send disabled
I espnow_link: ESP-NOW link init OK
I eink: SSD1681 e-ink panel detected — init OK
I eink_lvgl: LVGL init OK, display registered (200x200, 1bpp, set_px_cb)
I eink_lvgl: eink_lvgl_task started
I eink_lvgl: eink_lvgl_task stack HWM: <N> words free
```

Expected dongle console output:

```
I espnow_link: WiFi channel: 6 (2437 MHz)      ← factory default
W espnow_link: no paired peers configured (NVS rf.mac_* absent) — ESP-NOW send disabled
I espnow_link: ESP-NOW link init OK
```

- [ ] **Step 4: Verify unpaired e-ink fallback — panel shows "KaSe"**

After boot (~3 s for first SSD1681 refresh): the panel must show "KaSe" (not blank, not garbled). The static firmware version label must also be visible below the "KaSe" label. No layer updates will arrive (no peers). The panel stays static indefinitely.

If the panel is blank: the `xTaskNotifyWait` loop may have stalled before the first `lv_timer_handler()` call. Verify that `lv_timer_handler()` is called before the first `xTaskNotifyWait` in the new loop body (Step 3 of Task 5 places it there).

- [ ] **Step 5: Flash with pairing data pre-written (Plan RF-2 prerequisite)**

If Plan RF-2 has been executed and NVS pairing data exists (run after pairing):

```bash
# Verify NVS pairing keys exist — run from idf.py monitor:
# nvs_flash dump (or read via CDC KS_CMD_DONGLE_STATS if implemented)
```

Alternatively, use `esptool.py` to read the NVS partition and confirm `rf.mac_left` / `rf.mac_right` are non-zero on the dongle and `rf.mac_dongle` is non-zero on the half.

- [ ] **Step 6: End-to-end layer push — verify e-ink displays layer name**

With pairing data in NVS:
1. Reboot both dongle and half (to apply derived WiFi channel).
2. Verify dongle log: `I espnow_link: WiFi channel: <N> (2<NNN> MHz)` where N ∈ {1, 6, 11} (not the default 6 unless set_id % 3 == 1).
3. Verify dongle log: `I espnow_link: ESP-NOW peer 0 registered: <MAC> ch=<N>`.
4. Verify half log: `I espnow_link: ESP-NOW peer 0 registered: <dongle MAC> ch=<N>`.
5. From the KaSe controller software, switch the active layer (e.g., layer 1 "Nav").
6. Verify dongle log: `D dongle_engine: layer_changed: mac_left=<...> mac_right=<...>` (or the LOGD if enabled).
7. Verify half log: `I espnow_info: layer update: idx=1 name='Nav'`.
8. Verify half log: `I eink_lvgl: eink: layer label updated to 'Nav'`.
9. After ~2 s (SSD1681 full refresh): the panel displays "Nav" instead of "KaSe".
10. Switch back to layer 0 ("Base" or similar). Panel updates again within ~2 s.

- [ ] **Step 7: Isolation verification — 2nd keyboard (if available)**

With a second KaSe set present (different dongle):
1. Both sets must have been paired independently (different `set_id` → different WiFi channels unless set_id % 3 collision).
2. Trigger a layer change on Set B's dongle.
3. Verify that Set A's half e-ink panel does NOT update.
4. Expected half log for Set A: `D espnow_link: ESP-NOW recv from unknown MAC — dropped` (if LOGD enabled).

If isolation cannot be verified immediately (second set not available): document as "pending hardware test". The `is_known_peer()` logic is validated by the host tests in Task 1 (Cases 1–8 cover the isolation guarantee).

- [ ] **Step 8: Stack bump if needed**

If `eink_lvgl_task stack HWM: N words free` shows N < 128 (< 512 bytes):

```bash
# In eink_lvgl_start(), change 4096 → 6144.
git add main/periph/eink/eink_lvgl.c
git commit -m "fix(eink): bump eink_lvgl_task stack to 6144 (HWM < 512 bytes)"
```

Otherwise no commit for Task 7.

---

## Self-review checklist

- [ ] `is_known_peer()` and `espnow_set_peers()` are outside any `#ifndef TEST_HOST` guard in `espnow_link.c` — they must compile in both firmware and test builds.
- [ ] `espnow_link.h` includes only `<stdint.h>` and `<stdbool.h>` at file scope — no `esp_now.h` / `esp_wifi.h` (ESP-IDF headers leak into host test compilation).
- [ ] `esp_wifi_set_channel()` is called BEFORE `esp_now_add_peer()` — peer `.channel` matches the current WiFi channel (Trap 2).
- [ ] `esp_now_register_recv_cb()` is called AFTER `espnow_set_peers()` — filter table is populated before the first callback fires (Trap 3).
- [ ] All-zero MAC check in `espnow_link_init()`: `if (mac_l[0] || mac_l[1] || ...)` — does not add unpaired (zeroed) MACs as peers.
- [ ] `hid_report_get_modifiers()` is declared in `hid_report.h` and defined in `hid_report.c` — `current_modifiers` remains `static` (not de-staticized).
- [ ] `strncpy(l.name, default_layout_names[current_layout], 15)` — length 15, not 16; `en_layer_t.name[15]` stays NUL from `memset(&l, 0, sizeof(l))`.
- [ ] `espnow_send()` calls are inside `if (has_left)` / `if (has_right)` guards — no send to all-zeros MAC.
- [ ] `layer_changed()` checks both MACs independently — partial pairing (one half only) sends to the paired half only.
- [ ] `on_layer()` and `on_state()` use a 10 ms mutex timeout — not `portMAX_DELAY` (avoid blocking the ESP-NOW recv task indefinitely).
- [ ] `eink_get_task_handle()` declared in `eink_lvgl.h`; returns NULL until `eink_lvgl_start()` has been called; guarded with `if (h != NULL)` at every call site.
- [ ] `xTaskNotify(h, 0x01, eSetBits)` — not `xTaskNotifyGive` (which increments, not sets bits); `xTaskNotifyWait` clears all bits on exit.
- [ ] `lv_timer_handler()` called at the TOP of `eink_lvgl_task` loop, before `xTaskNotifyWait` — ensures pending LVGL invalidations are processed on the iteration following the notify.
- [ ] `lv_obj_is_valid(s_label_layer)` checked before `lv_label_set_text()` — safe if `display_clear_screen()` was ever called.
- [ ] `full_refresh = 0` and `set_px_cb`-based pixel packing in `eink_lvgl.c` are not touched (hardware-validated in Plan Bricks-3).
- [ ] `half_spi_lock` / `half_spi_unlock` ordering in `eink_push()` not touched — BUSY wait remains outside the lock (Trap 6).
- [ ] `espnow_link.c` compiled with `-DTEST_HOST` in `test/CMakeLists.txt`: only `is_known_peer` and `espnow_set_peers` compile; all WiFi/ESP-NOW/NVS code is inside `#ifndef TEST_HOST`.
- [ ] Host test count grows from 890 to ≥ 898 (8 new cases from `test_espnow_peer_filter`); 0 failed.
- [ ] Three builds all green: `build_dongle`, `build_half_left`, `build_half_right`.
- [ ] Graceful-degrades: unpaired (no NVS keys) → no peers added → ESP-NOW sends skipped → e-ink shows "KaSe" → keyboard functions normally. No panics, no asserts.
- [ ] `rf.wifi_ch` NVS key is no longer read (deprecated per spec §2.5 divergence note). Code comment at the replacement site references the spec section.

---

## Out of scope

| Feature | Deferred to |
|---------|-------------|
| Battery telemetry (`on_battery()` stub fill, CDC push) | Future CDC extension, not this plan |
| ESP-NOW encryption (ECDH, per-packet nonce) | Spec §9 — out of scope by design |
| OTA push via ESP-NOW (Plan 5) | Separate plan; NRF muting strategy unchanged |
| Partial e-ink refresh | Plan Bricks-4+ (custom LUT, ghosting management) |
| `KS_CMD_RF_PAIR_STATUS` CDC polling | Plan RF-2 (pairing flow) |
| Half boot button pairing trigger | Plan RF-2 |
| `current_layout` display on dongle OLED | Dongle has no OLED (no KASE_HAS_EINK on dongle) |
| `flags` field in `en_state_t` (caps_word, bt_connected) | Stubs remain `s.flags = 0`; expand when source signals are wired |
| Isolation validation with 2nd physical keyboard | Bench test step 7 — requires a second set; host test Cases 1–8 provide logical coverage |
