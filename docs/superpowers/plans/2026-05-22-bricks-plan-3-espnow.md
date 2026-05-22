# Plan Bricks-3 — ESP-NOW Info Channel (WiFi sdkconfig + Link + Msg + Half State)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the ESP-NOW info channel across dongle and both halves:
- Enable WiFi in `sdkconfig.defaults.half_left/right` (ESP-NOW prerequisite).
- Host-test the `espnow_msg.h` encode/decode (pure codec, no ESP-IDF deps).
- Create `espnow_link.h/c` (WiFi STA init + esp_now_init + peer management + send + recv dispatch).
- Create `espnow_msg.h` (type IDs + payload structs).
- Create `espnow_info.c` (handlers stubs: battery RX on dongle, layer/state RX on half).
- Define `g_half_state` + mutex in `espnow_info.h` (shared between `espnow_info.c` and `eink.c`).
- Hook `dongle_engine_state.c::layer_changed()` to push EN_INFO_LAYER + EN_INFO_STATE (stub body).
- Add battery TX in `half_scan_task.c` heartbeat timer (stub: sends zeros, 30 s period).
- Align WiFi channel across dongle and halves: ch 6 (2437 MHz) in all defaults files.
- Wire `espnow_link_init()` into `main.c` (dongle) and `half_scan_task.c` (half).
- Add CMake conditional blocks and priv_requires (esp_wifi, esp_event).
- End state: all three targets build green; host tests pass.

**Architecture:**
- `espnow_link.c` handles the ESP-NOW transport: WiFi STA mode (radio only, no IP),
  `esp_now_init`, recv callback that dispatches by type to `espnow_info.c`, and
  `espnow_send()` which prepends the 1-byte type field.
- `espnow_msg.h` is a pure header (structs + constants) with no ESP-IDF function calls —
  it can be included in host tests.
- `espnow_info.c` is compiled on BOTH dongle and half but each `#if` branch is role-specific.
- `g_half_state` + `g_half_state_mutex` are defined in `espnow_info.c` (half role) and
  declared extern in `espnow_info.h`. `eink.c` includes `espnow_info.h` to read state.
- The MAC management is intentionally stubbed: `mac_dongle`/`mac_left`/`mac_right` are
  zero by default (no NVS key yet). Peer add is skipped with a log warning. Battery TX to
  dongle is a no-op in the stub (log only). This is the correct boundary.

**Tech Stack:** ESP-IDF 5.5, `esp_wifi`, `esp_event`, `esp_now`, FreeRTOS, NVS (read-only for
wifi_ch — existing namespace `"rf"`). No new external components.

**Spec reference:** `docs/superpowers/specs/2026-05-22-half-peripherals-espnow-design.md`
Sections 3 (Kconfig), 7 (ESP-NOW), 8 (shared concerns), Annex A/B.

**Depends on:** Plan Bricks-1 (Kconfig has KASE_HAS_ESPNOW extended to HALF). Plan Bricks-2
not strictly required (eink.c reads g_half_state but that is wired here independently).

**Build targets:** All three (half_left, half_right, dongle). Build preamble:
```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f sdkconfig
idf.py -B build_<target> -DBOARD=kase_<target> -DIDF_TARGET=esp32s3 build
```

---

## Learned facts baked in

1. **WiFi must be enabled on the half for ESP-NOW.** Current `sdkconfig.defaults.half_left`
   has `CONFIG_ESP_WIFI_ENABLED=n`. Flipping it to `y` pulls in the WiFi + LwIP stack.
   Per spec §8.4 and 9.2, also set: `CONFIG_ESP_WIFI_NVS_ENABLED=n`,
   `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n`, `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n`.
   LwIP: consult whether `CONFIG_LWIP_ENABLE=n` is safe — if not accepted, set
   `CONFIG_ESP_WIFI_STATIC_TX_BUFFER=n` and accept LwIP but disable DHCP. In practice,
   with mode STA and no connection, LwIP adds ~20 KB heap but is inert. Keep it enabled
   for compatibility; add a comment.

2. **WiFi channel alignment.** The spec uses ch 6 (2437 MHz) for ESP-NOW (updated from the
   spec's earlier example of ch 11 — spec §7.2 explicitly chose ch 6 for NRF separation).
   Both halves and the dongle must use the same channel. The dongle's
   `sdkconfig.defaults.dongle` may not set a WiFi channel (check). The channel is set via
   `esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE)` in `espnow_link_init()`. If the dongle
   sdkconfig does not enable WiFi, add it there too.

3. **Dongle already has `KASE_HAS_ESPNOW=y` and `esp_wifi`/`esp_event` in priv_requires
   (added in Plan Half-1 MVP CMake).** Verify this before adding duplicates. The dongle
   sdkconfig.defaults should already have `CONFIG_ESP_WIFI_ENABLED=y` — check.

4. **`esp_netif_init()` and `esp_event_loop_create_default()` are safe to call multiple
   times** (they return `ESP_ERR_INVALID_STATE` on re-init, not a panic). Use the pattern:
   ```c
   esp_err_t e = esp_netif_init();
   if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ... }
   ```
   The dongle may already call these from another subsystem. Check `main.c` dongle branch.

5. **MAC peer management stub:** The spec (§7.2 and §9.5) says: if MACs are zero,
   log a warning and skip peer add. This is the correct stub behavior — do not add a peer
   with an all-zero MAC. `esp_now_add_peer` with a zero MAC would fail with an error anyway.
   On the half, `mac_dongle` is missing from NVS — stub: log only, no send.

6. **`espnow_link.c` call in `main.c` (dongle):** The dongle `main.c` currently has a
   `#if CONFIG_KASE_HAS_ESPNOW` block that is empty (from earlier scaffold). Add
   `espnow_link_init()` call there. Read the current `main.c` dongle branch before editing.

7. **`espnow_link.c` call in `half_scan_task.c`:** Add after eink init. ESP-NOW init is
   after SPI init but before the keyboard_button loop. The recv callback runs in the
   ESP-NOW internal task — no conflict with SPI or keyboard tasks.

8. **`espnow_info.c` on dongle vs half:** The same `.c` file is compiled on both roles.
   Use `#if CONFIG_KASE_DEVICE_ROLE_DONGLE` / `#elif CONFIG_KASE_DEVICE_ROLE_HALF`
   guards inside the file to provide role-specific handler bodies. Both roles compile the
   file, but only the relevant handler bodies are included.

9. **`espnow_msg.h` encode/decode is worth host-testing.** The spec wire format (1-byte type
   prepend + packed struct) is pure C with no ESP-IDF deps. A small test validates the type
   byte, struct sizes, and round-trip for all three message types (BATTERY, LAYER, STATE).
   This catches struct padding bugs early. Add to `test/test_espnow_msg.c`.

10. **`layer_changed()` in `dongle_engine_state.c`:** Currently a no-op `void layer_changed(void) { }`.
    The hook body is a stub (does not send — MACs not configured). Wrap in
    `#if CONFIG_KASE_HAS_ESPNOW` to avoid pulling in espnow headers on non-ESPNOW builds
    (though all current dongle builds have ESPNOW=y).

11. **`g_half_state_mutex` init location:** Must be initialized before `espnow_link_init()`
    is called (the recv callback may fire immediately and call `espnow_on_layer`). Initialize
    in `espnow_info_init()` (a new init function called before `espnow_link_init()`), or at
    the top of `espnow_info.c` in a `__attribute__((constructor))` — avoid the constructor
    attribute. Use an explicit `espnow_info_init()` call from `half_scan_task`.

12. **`g_half_state` definition location:** Defined in `espnow_info.c` (compiled only on HALF
    for the definition; the dongle compiles the same file but the definition is inside
    `#if CONFIG_KASE_DEVICE_ROLE_HALF`). Declared extern in `espnow_info.h` for `eink.c`.
    On the dongle, `eink.c` is not compiled (KASE_HAS_EINK=n) — no linker issue.

---

## File Structure

### Created

| Path | Responsibility |
|---|---|
| `main/comm/espnow/espnow_link.h` | Public API: `espnow_link_init()`, `espnow_send()` |
| `main/comm/espnow/espnow_link.c` | WiFi STA init, esp_now_init, peer add (stub), send, recv dispatch |
| `main/comm/espnow/espnow_msg.h` | Type IDs, payload structs, encode/decode helpers (pure) |
| `main/comm/espnow/espnow_info.h` | Handler declarations + `g_half_state` extern + `half_state_t` typedef |
| `main/comm/espnow/espnow_info.c` | Handler stubs (battery/layer/state) + g_half_state definition |
| `test/test_espnow_msg.c` | Host tests: wire format, struct sizes, round-trip encode/decode |

### Modified

| Path | Change |
|---|---|
| `sdkconfig.defaults.half_left` | Enable WiFi; set ch 6 defaults |
| `sdkconfig.defaults.half_right` | Same changes (copy of half_left) |
| `sdkconfig.defaults.dongle` | Verify WiFi enabled + ch 6 alignment (edit if needed) |
| `main/CMakeLists.txt` | Add espnow_link.c + espnow_info.c under KASE_HAS_ESPNOW; add esp_wifi + esp_event priv_requires; add include dir |
| `main/comm/rf/dongle_engine_state.c` | `layer_changed()` → stub push EN_INFO_LAYER + EN_INFO_STATE |
| `main/comm/rf/half_scan_task.c` | Add `espnow_info_init()` + `espnow_link_init()` calls; add battery TX counter in heartbeat_timer_cb |
| `main/main.c` | Dongle branch: add `espnow_link_init()` call in KASE_HAS_ESPNOW block |
| `main/periph/eink/eink.c` | Replace `s_layer_idx_stub` read with `g_half_state` (guarded; safe if Plan Bricks-3 not done yet) |
| `test/CMakeLists.txt` | Add test_espnow_msg.c to test_runner |
| `test/test_main.c` | Add extern + call for test_espnow_msg |

### Untouched

```
main/comm/rf/rf_rx_task.c          PKT_TRACKPAD stub already done (Plan Bricks-1)
main/comm/rf/rf_driver.c           unchanged
main/periph/half_spi.h/c           unchanged
main/periph/trackpad/trackpad.c    unchanged
main/periph/eink/eink.c            small edit to replace stub read with g_half_state
boards/kase_half_left/board.h      unchanged (GPIO defines already added)
```

---

## Task 1: Enable WiFi in sdkconfig.defaults for halves and verify dongle

**Files:**
- Modify: `sdkconfig.defaults.half_left`
- Modify: `sdkconfig.defaults.half_right`
- Read (verify): `sdkconfig.defaults.dongle`

ESP-NOW requires the WiFi radio to be enabled. The halves currently have
`CONFIG_ESP_WIFI_ENABLED=n`. This task flips that and adds related settings.

- [ ] **Step 1: Read current `sdkconfig.defaults.dongle`**

```bash
cat /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig.defaults.dongle
```

Verify: `CONFIG_ESP_WIFI_ENABLED=y` is present and WiFi is configured. If the dongle
already has it, no edit needed. If missing, add the same WiFi block as below.

- [ ] **Step 2: Edit `sdkconfig.defaults.half_left`**

Replace:
```ini
# --- WiFi disabled (ESP-NOW deferred to Phase 2) ---
CONFIG_ESP_WIFI_ENABLED=n
```

With:
```ini
# --- WiFi enabled for ESP-NOW info channel (plan bricks-3) ---
# ESP-NOW requires the WiFi radio. No AP, no IP, no credentials stored.
# Memory impact: +60-80 KB heap (ESP32-S3 has 512 KB SRAM — acceptable).
# Power impact: +10-20 mA (modem sleep pending power-management brick).
# Channel: 6 (2437 MHz) — 29 MHz below NRF24 left at 2476 MHz (strategy A, spec §8.1).
# See docs/superpowers/specs/2026-05-22-half-peripherals-espnow-design.md §8.4.
CONFIG_ESP_WIFI_ENABLED=y
CONFIG_ESP_WIFI_NVS_ENABLED=n
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n
CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n
```

- [ ] **Step 3: Copy the same change to `sdkconfig.defaults.half_right`**

```bash
# Apply the same WiFi block — half_right has identical content to half_left
```

Edit `sdkconfig.defaults.half_right` with the same replacement as Step 2.

- [ ] **Step 4: Verify a half build reconfigures without error (WiFi now enabled)**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_half_left \
    -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware reconfigure 2>&1 | tail -10
```

Expected: reconfigure completes without error. The full build is done in Task 7 (after all
sources are added). This step only checks that sdkconfig is accepted.

- [ ] **Step 5: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add sdkconfig.defaults.half_left sdkconfig.defaults.half_right
git commit -m "feat(bricks): enable WiFi in half sdkconfig.defaults (ESP-NOW prerequisite)"
```

---

## Task 2: Create `espnow_msg.h` + host tests (TDD)

**Files:**
- Create: `main/comm/espnow/espnow_msg.h`
- Create: `test/test_espnow_msg.c`
- Modify: `test/CMakeLists.txt`
- Modify: `test/test_main.c`

`espnow_msg.h` defines the wire format. It is pure C (no ESP-IDF function calls) and can be
compiled in the host test build directly via `#include`.

- [ ] **Step 1: Create directory**

```bash
mkdir -p /home/mae/Documents/GitHub/KaSe_firmware/main/comm/espnow
```

- [ ] **Step 2: Write `main/comm/espnow/espnow_msg.h`**

```c
#pragma once

/*
 * espnow_msg.h — ESP-NOW info-channel message types and payload structs.
 *
 * Wire format:
 *   [type:u8][payload bytes...]
 *   Total ESP-NOW payload = 1 + sizeof(payload struct).
 *   espnow_send() in espnow_link.c prepends the type byte.
 *
 * Type ID ranges:
 *   0x00-0x0F  Info channel (this file — layer, battery, state).
 *   0x10-0x1F  OTA (Plan 5, dongle spec §8 — reserved, not implemented).
 *   0x20-0x2F  Config push (Plan 5 — reserved).
 *   0x30-0x3F  Verbose telemetry (Plan 5 — reserved).
 *
 * All structs are packed. Endianness: all fields ≤ 1 byte (no multi-byte
 * integers in the current info-channel payloads — no endianness issue).
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* ── Info-channel type IDs ────────────────────────────────────── */
#define EN_INFO_BATTERY   0x01   /* half → dongle: battery level (en_battery_t) */
#define EN_INFO_LAYER     0x02   /* dongle → half: current active layer (en_layer_t) */
#define EN_INFO_STATE     0x03   /* dongle → half: modifier + flag state (en_state_t) */

/* ── Reserved type ID ranges (Plan 5 — not implemented) ─────── */
/* EN_OTA_BEGIN = 0x10, EN_OTA_CHUNK = 0x11, EN_OTA_END = 0x12, EN_OTA_ACK = 0x13 */
/* EN_CFG_PUSH = 0x20, EN_CFG_ACK = 0x21                                           */
/* EN_TELEM_REQ = 0x30, EN_TELEM_RSP = 0x31                                        */

/* ── EN_INFO_BATTERY (half → dongle) — 3 bytes ──────────────── */
typedef struct __attribute__((packed)) {
    uint8_t batt_dV;    /* Battery voltage × 10. 0=unknown. Range: 0..83 (0..8.3V). */
    uint8_t soc_pct;    /* State of charge 0..100. 0=unknown (battery brick not done). */
    uint8_t charging;   /* 0=not charging, 1=charging. Source: BMS GPIO46 (stub). */
} en_battery_t;

/* ── EN_INFO_LAYER (dongle → half) — 17 bytes ───────────────── */
typedef struct __attribute__((packed)) {
    uint8_t layer_idx;  /* Active layer index 0..N-1. */
    char    name[16];   /* Layer name string, zero-padded. Not null-terminated if 16 chars. */
} en_layer_t;

/* ── EN_INFO_STATE (dongle → half) — 2 bytes ────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t modifiers;  /* HID modifier byte (same as USB HID Report byte 0). */
    uint8_t flags;      /* bit0=caps_word, bit1=bt_connected, bit2=usb_active, bits3-7=rsvd */
} en_state_t;

/* ── Encode helpers: write [type][payload] into buf, return total bytes written. ── */
/* buf must be at least 1 + sizeof(payload). Returns 0 on error. */

static inline uint8_t en_encode_battery(uint8_t *buf, const en_battery_t *b)
{
    buf[0] = EN_INFO_BATTERY;
    memcpy(buf + 1, b, sizeof(*b));
    return 1 + sizeof(*b);
}

static inline uint8_t en_encode_layer(uint8_t *buf, const en_layer_t *l)
{
    buf[0] = EN_INFO_LAYER;
    memcpy(buf + 1, l, sizeof(*l));
    return 1 + sizeof(*l);
}

static inline uint8_t en_encode_state(uint8_t *buf, const en_state_t *s)
{
    buf[0] = EN_INFO_STATE;
    memcpy(buf + 1, s, sizeof(*s));
    return 1 + sizeof(*s);
}

/* ── Decode helpers: parse from [type][payload] buffer.
 * Returns true if type matches and buf has enough bytes. ────── */

static inline bool en_decode_battery(const uint8_t *buf, uint8_t len, en_battery_t *out)
{
    if (len < 1 + sizeof(*out) || buf[0] != EN_INFO_BATTERY) return false;
    memcpy(out, buf + 1, sizeof(*out));
    return true;
}

static inline bool en_decode_layer(const uint8_t *buf, uint8_t len, en_layer_t *out)
{
    if (len < 1 + sizeof(*out) || buf[0] != EN_INFO_LAYER) return false;
    memcpy(out, buf + 1, sizeof(*out));
    return true;
}

static inline bool en_decode_state(const uint8_t *buf, uint8_t len, en_state_t *out)
{
    if (len < 1 + sizeof(*out) || buf[0] != EN_INFO_STATE) return false;
    memcpy(out, buf + 1, sizeof(*out));
    return true;
}
```

- [ ] **Step 3: Write `test/test_espnow_msg.c`**

```c
#include "test_framework.h"
#include "../main/comm/espnow/espnow_msg.h"
#include <string.h>

void test_espnow_msg(void)
{
    TEST_SUITE("espnow_msg");

    /* ── Struct size verification ────────────────────────────── */
    TEST_ASSERT_EQ(sizeof(en_battery_t), 3, "en_battery_t is 3 bytes (packed)");
    TEST_ASSERT_EQ(sizeof(en_layer_t),  17, "en_layer_t is 17 bytes (1+16, packed)");
    TEST_ASSERT_EQ(sizeof(en_state_t),   2, "en_state_t is 2 bytes (packed)");

    /* ── EN_INFO_BATTERY round-trip ─────────────────────────── */
    {
        en_battery_t b_in  = { .batt_dV = 74, .soc_pct = 85, .charging = 1 };
        en_battery_t b_out = { 0 };
        uint8_t buf[1 + sizeof(en_battery_t)];

        uint8_t n = en_encode_battery(buf, &b_in);
        TEST_ASSERT_EQ(n, 4, "en_encode_battery returns 4 bytes (1+3)");
        TEST_ASSERT_EQ(buf[0], EN_INFO_BATTERY, "type byte is EN_INFO_BATTERY");

        TEST_ASSERT(en_decode_battery(buf, n, &b_out), "en_decode_battery succeeds");
        TEST_ASSERT_EQ(b_out.batt_dV,  74, "batt_dV round-trips");
        TEST_ASSERT_EQ(b_out.soc_pct,  85, "soc_pct round-trips");
        TEST_ASSERT_EQ(b_out.charging,  1, "charging round-trips");

        /* Wrong type byte must fail decode */
        buf[0] = EN_INFO_LAYER;
        TEST_ASSERT(!en_decode_battery(buf, n, &b_out), "wrong type byte fails decode");

        /* Truncated buffer must fail decode */
        buf[0] = EN_INFO_BATTERY;
        TEST_ASSERT(!en_decode_battery(buf, 2, &b_out), "truncated buffer fails decode");
    }

    /* ── EN_INFO_LAYER round-trip ────────────────────────────── */
    {
        en_layer_t l_in = { .layer_idx = 3 };
        strncpy(l_in.name, "Gaming", 16);
        en_layer_t l_out = { 0 };
        uint8_t buf[1 + sizeof(en_layer_t)];

        uint8_t n = en_encode_layer(buf, &l_in);
        TEST_ASSERT_EQ(n, 18, "en_encode_layer returns 18 bytes (1+17)");
        TEST_ASSERT_EQ(buf[0], EN_INFO_LAYER, "type byte is EN_INFO_LAYER");

        TEST_ASSERT(en_decode_layer(buf, n, &l_out), "en_decode_layer succeeds");
        TEST_ASSERT_EQ(l_out.layer_idx, 3, "layer_idx round-trips");
        TEST_ASSERT(memcmp(l_out.name, "Gaming", 6) == 0, "name round-trips");

        /* 16-char name (no null terminator) */
        en_layer_t l16 = { .layer_idx = 0 };
        memset(l16.name, 'A', 16);   /* no null terminator */
        uint8_t buf16[1 + sizeof(en_layer_t)];
        en_encode_layer(buf16, &l16);
        en_layer_t l16_out = { 0 };
        TEST_ASSERT(en_decode_layer(buf16, sizeof(buf16), &l16_out), "16-char name decodes");
        TEST_ASSERT(memcmp(l16_out.name, l16.name, 16) == 0, "16-char name matches exactly");
    }

    /* ── EN_INFO_STATE round-trip ────────────────────────────── */
    {
        en_state_t s_in  = { .modifiers = 0x02, .flags = 0x05 };   /* left shift + caps_word + usb_active */
        en_state_t s_out = { 0 };
        uint8_t buf[1 + sizeof(en_state_t)];

        uint8_t n = en_encode_state(buf, &s_in);
        TEST_ASSERT_EQ(n, 3, "en_encode_state returns 3 bytes (1+2)");
        TEST_ASSERT_EQ(buf[0], EN_INFO_STATE, "type byte is EN_INFO_STATE");

        TEST_ASSERT(en_decode_state(buf, n, &s_out), "en_decode_state succeeds");
        TEST_ASSERT_EQ(s_out.modifiers, 0x02, "modifiers round-trips");
        TEST_ASSERT_EQ(s_out.flags,     0x05, "flags round-trips");
    }

    /* ── Type ID uniqueness ────────────────────────────────────── */
    TEST_ASSERT(EN_INFO_BATTERY != EN_INFO_LAYER,  "BATTERY != LAYER");
    TEST_ASSERT(EN_INFO_BATTERY != EN_INFO_STATE,  "BATTERY != STATE");
    TEST_ASSERT(EN_INFO_LAYER   != EN_INFO_STATE,  "LAYER != STATE");

    /* ── Zero / unknown struct values ────────────────────────── */
    {
        en_battery_t b_zero = { .batt_dV = 0, .soc_pct = 0, .charging = 0 };
        uint8_t buf[4];
        en_encode_battery(buf, &b_zero);
        en_battery_t b_out = { 0xFF, 0xFF, 0xFF };
        en_decode_battery(buf, sizeof(buf), &b_out);
        TEST_ASSERT_EQ(b_out.batt_dV, 0, "batt_dV zero encodes/decodes as 0 (unknown stub value)");
        TEST_ASSERT_EQ(b_out.soc_pct, 0, "soc_pct zero encodes/decodes as 0");
    }
}
```

- [ ] **Step 4: Add to `test/CMakeLists.txt`**

In `add_executable(test_runner ...)`, add `test_espnow_msg.c` to the file list.
No new `.c` firmware files need to be compiled — `espnow_msg.h` is a pure header.

- [ ] **Step 5: Add to `test/test_main.c`**

Add:
```c
extern void test_espnow_msg(void);
```
in the extern declarations block. Add `test_espnow_msg();` call in `main()`.

- [ ] **Step 6: Run host tests**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -10 && ./test_runner 2>&1 | tail -30
```

Expected: all existing tests pass, `test_espnow_msg` passes with no FAILs.

Common failure: `sizeof(en_layer_t) != 17` — indicates padding issue. Verify
`__attribute__((packed))` is present on the struct. On x86 host with GCC, packed structs
may differ from the target if the attribute is missing.

- [ ] **Step 7: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/espnow/espnow_msg.h test/test_espnow_msg.c \
        test/CMakeLists.txt test/test_main.c
git commit -m "feat(bricks): espnow_msg.h wire format + host tests (TDD — all types)"
```

---

## Task 3: Create `espnow_info.h` and `espnow_info.c`

**Files:**
- Create: `main/comm/espnow/espnow_info.h`
- Create: `main/comm/espnow/espnow_info.c`

`espnow_info.h` exposes `half_state_t`, `g_half_state`, `g_half_state_mutex`, the three
handler functions, and `espnow_info_init()`. `espnow_info.c` defines the state and provides
stub handler bodies.

- [ ] **Step 1: Write `main/comm/espnow/espnow_info.h`**

```c
#pragma once

/*
 * espnow_info.h — ESP-NOW info-channel handler API.
 *
 * Shared between espnow_info.c (writer) and eink.c (reader) via g_half_state.
 * Roles:
 *   - Dongle: receives EN_INFO_BATTERY from a half; stub logs it.
 *   - Half:   receives EN_INFO_LAYER + EN_INFO_STATE from dongle; updates g_half_state.
 *
 * g_half_state and g_half_state_mutex are defined in espnow_info.c (half role only).
 * They are declared extern here for eink.c.
 */

#include "espnow_msg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <string.h>

/* ── Half state shared struct ────────────────────────────────── */
typedef struct {
    uint8_t layer_idx;       /* Current active layer index */
    char    layer_name[16];  /* Layer name, zero-padded */
    uint8_t modifiers;       /* HID modifier byte */
    uint8_t flags;           /* bit0=caps_word, bit1=bt_connected, bit2=usb_active */
} half_state_t;

/* Global half state (defined in espnow_info.c, HALF role only).
 * On the dongle, this symbol is not defined — eink.c is not compiled there either. */
extern half_state_t g_half_state;
extern SemaphoreHandle_t g_half_state_mutex;

/* Convenience lock/unlock wrappers */
static inline void half_state_lock(void)   { xSemaphoreTake(g_half_state_mutex, portMAX_DELAY); }
static inline void half_state_unlock(void) { xSemaphoreGive(g_half_state_mutex); }

/* Initialize the info-channel handler module.
 * Creates g_half_state_mutex. Must be called before espnow_link_init(). */
void espnow_info_init(void);

/* Dispatch callback called by espnow_link.c on message receipt.
 * mac: 6-byte sender MAC; buf: raw [type][payload] bytes; len: total length. */
void espnow_info_dispatch(const uint8_t mac[6], const uint8_t *buf, uint8_t len);
```

- [ ] **Step 2: Write `main/comm/espnow/espnow_info.c`**

```c
/*
 * espnow_info.c — ESP-NOW info-channel handler stubs.
 *
 * Skeleton:  dispatch logic, g_half_state update, mutex.
 * Stub:      all handler bodies (log only or no-op).
 *
 * Both dongle and half compile this file. Role-specific code is guarded with
 * CONFIG_KASE_DEVICE_ROLE_DONGLE / CONFIG_KASE_DEVICE_ROLE_HALF.
 */

#include "espnow_info.h"
#include "espnow_msg.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "espnow_info";

/* ── g_half_state + mutex — defined on HALF only ─────────────── */
#if CONFIG_KASE_DEVICE_ROLE_HALF
half_state_t     g_half_state       = { 0 };
SemaphoreHandle_t g_half_state_mutex = NULL;
#endif

void espnow_info_init(void)
{
#if CONFIG_KASE_DEVICE_ROLE_HALF
    g_half_state_mutex = xSemaphoreCreateMutex();
    if (g_half_state_mutex == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed for g_half_state_mutex");
    }
    memset(&g_half_state, 0, sizeof(g_half_state));
    ESP_LOGI(TAG, "espnow_info_init OK (half role)");
#else
    ESP_LOGI(TAG, "espnow_info_init OK (dongle role — no half_state)");
#endif
}

/* ── Dongle: receives EN_INFO_BATTERY from a half ─────────────── */
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
static void on_battery(const uint8_t mac[6], const en_battery_t *b)
{
    ESP_LOGI(TAG, "battery from %02X:%02X:%02X:%02X:%02X:%02X: %u.%uV soc=%u%% chg=%u",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             b->batt_dV / 10, b->batt_dV % 10,
             b->soc_pct, b->charging);
    /* TODO STUB: Forward battery level to controller via CDC push frame.
     *   Suggested: new KR unsolicited frame (KS_CMD_RF_LINK_STATUS extended, or new cmd).
     *   Or: store in a global and return via KS_CMD_DONGLE_STATS.
     *   For now: log only. */
}
#endif /* CONFIG_KASE_DEVICE_ROLE_DONGLE */

/* ── Half: receives EN_INFO_LAYER from dongle ─────────────────── */
#if CONFIG_KASE_DEVICE_ROLE_HALF
static void on_layer(const en_layer_t *l)
{
    ESP_LOGI(TAG, "layer update: idx=%u name='%.16s'", l->layer_idx, l->name);
    /* TODO STUB: Update g_half_state and wake eink_task.
     *   half_state_lock();
     *   g_half_state.layer_idx = l->layer_idx;
     *   memcpy(g_half_state.layer_name, l->name, 16);
     *   half_state_unlock();
     *   xTaskNotify(s_eink_task_handle, 0x01, eSetBits);   // wake eink to refresh
     *   Requires: expose eink task handle (eink_get_task_handle() in eink.h). */
    (void)l;
}

/* ── Half: receives EN_INFO_STATE from dongle ─────────────────── */
static void on_state(const en_state_t *s)
{
    ESP_LOGD(TAG, "state update: mod=0x%02x flags=0x%02x", s->modifiers, s->flags);
    /* TODO STUB: Update g_half_state modifiers/flags and wake eink_task. */
    (void)s;
}
#endif /* CONFIG_KASE_DEVICE_ROLE_HALF */

/* ── Dispatch: called by espnow_link.c recv callback ─────────── */
void espnow_info_dispatch(const uint8_t mac[6], const uint8_t *buf, uint8_t len)
{
    if (len < 1) return;
    uint8_t type = buf[0];

    switch (type) {
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
    case EN_INFO_BATTERY: {
        en_battery_t b;
        if (en_decode_battery(buf, len, &b)) {
            on_battery(mac, &b);
        }
        break;
    }
#endif
#if CONFIG_KASE_DEVICE_ROLE_HALF
    case EN_INFO_LAYER: {
        en_layer_t l;
        if (en_decode_layer(buf, len, &l)) {
            on_layer(&l);
        }
        break;
    }
    case EN_INFO_STATE: {
        en_state_t s;
        if (en_decode_state(buf, len, &s)) {
            on_state(&s);
        }
        break;
    }
#endif
    default:
        ESP_LOGW(TAG, "unknown ESP-NOW type 0x%02x (len=%u) — dropped", type, len);
        break;
    }
}
```

- [ ] **Step 3: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/espnow/espnow_info.h main/comm/espnow/espnow_info.c
git commit -m "feat(bricks): espnow_info — handler stubs (battery/layer/state) + half_state"
```

---

## Task 4: Create `espnow_link.h` and `espnow_link.c`

**Files:**
- Create: `main/comm/espnow/espnow_link.h`
- Create: `main/comm/espnow/espnow_link.c`

- [ ] **Step 1: Write `main/comm/espnow/espnow_link.h`**

```c
#pragma once

/*
 * espnow_link.h — ESP-NOW link layer API.
 *
 * Handles WiFi STA init (radio only, no IP), esp_now_init, peer management,
 * send (with type byte prepend), and recv dispatch to espnow_info_dispatch().
 *
 * Call espnow_info_init() before espnow_link_init() to ensure g_half_state_mutex
 * exists before the recv callback can fire.
 *
 * MAC sources:
 *   - Dongle: loads mac_left / mac_right from NVS namespace "rf" (stub: zeros → skip add).
 *   - Half: loads mac_dongle from NVS namespace "rf" (stub: zeros → battery TX skipped).
 *   - All MACs zero by default until pairing (Plan 4) is implemented.
 */

#include <stdint.h>
#include <stdbool.h>

/* Initialize ESP-NOW link:
 *   - esp_netif_init() + esp_event_loop_create_default() (idempotent)
 *   - WiFi STA mode (no connection, radio only)
 *   - Channel from NVS rf.wifi_ch (default 6 = 2437 MHz)
 *   - esp_now_init()
 *   - Register recv callback → espnow_info_dispatch()
 *   - Add peers from NVS (zeros → log warning, skip add)
 * Returns true on success. */
bool espnow_link_init(void);

/* Send an ESP-NOW message.
 *   mac:     6-byte target peer MAC
 *   type:    message type ID (from espnow_msg.h)
 *   payload: message body (may be NULL if len == 0)
 *   len:     payload length in bytes (uint16_t per spec; ESP-NOW max is 250 bytes)
 * The function prepends [type] before payload in the ESP-NOW frame.
 * Returns true if esp_now_send() accepted the frame (fire-and-forget; no ACK). */
bool espnow_send(const uint8_t mac[6], uint8_t type, const void *payload, uint16_t len);
```

- [ ] **Step 2: Write `main/comm/espnow/espnow_link.c`**

```c
/*
 * espnow_link.c — ESP-NOW link layer skeleton.
 *
 * Skeleton:  WiFi STA init, esp_now_init, recv dispatch, send with type prepend.
 * Stub:      Peer MAC loading from NVS (zeros → skip add + log warning).
 *
 * WiFi channel: 6 (2437 MHz) by default. This gives ~29 MHz separation from the
 * NRF24 left channel (2476 MHz). Strategy A from spec §8.1: tolerate occasional
 * NRF packet loss (heartbeat reconciliation recovers state). Bench measurement of
 * rf.link_q before/after ESP-NOW activation is required to validate the approach.
 */

#include "espnow_link.h"
#include "espnow_info.h"   /* espnow_info_dispatch */
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "espnow_link";

/* ── Recv callback — called from esp_now internal task (not ISR) ── */
/* ESP-IDF signature: data_len is int. Safe cast to uint8_t: ESP-NOW max payload
 * is 250 bytes, so data_len is always in [1..250] when non-negative. */
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int data_len)
{
    if (recv_info == NULL || data == NULL || data_len < 1 || data_len > 250) return;
    espnow_info_dispatch(recv_info->src_addr, data, (uint8_t)data_len);
}

bool espnow_link_init(void)
{
    /* ── Network interface init (idempotent) ───────────────────── */
    esp_err_t e;
    e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %d", e);
        return false;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %d", e);
        return false;
    }

    /* ── WiFi STA mode (no connection — radio only for ESP-NOW) ── */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&wifi_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %d", e);
        return false;
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   /* no NVS cred persistence */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* ── WiFi channel from NVS rf.wifi_ch (default 6) ─────────── */
    uint8_t wifi_ch = 6;
    nvs_handle_t nvs_h;
    if (nvs_open("rf", NVS_READONLY, &nvs_h) == ESP_OK) {
        uint8_t ch_nvs = 0;
        if (nvs_get_u8(nvs_h, "wifi_ch", &ch_nvs) == ESP_OK && ch_nvs >= 1 && ch_nvs <= 13) {
            wifi_ch = ch_nvs;
        }
        nvs_close(nvs_h);
    }
    esp_wifi_set_channel(wifi_ch, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG, "WiFi channel: %u (2%03u MHz)", wifi_ch, 412 + wifi_ch * 5);

    /* ── ESP-NOW init + recv callback ─────────────────────────── */
    e = esp_now_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %d", e);
        return false;
    }
    esp_now_register_recv_cb(espnow_recv_cb);

    /* ── Peer add from NVS (stub: MACs are zeros until pairing) ── */
    /* NVS namespace "rf" keys mac_left, mac_right (dongle) / mac_dongle (half).
     * Until Plan 4 (pairing), these keys are absent → MACs are zero → skip add. */
    /* TODO STUB: load peer MACs from NVS namespace "rf":
     *   Dongle: keys "mac_left" (6 bytes), "mac_right" (6 bytes)
     *   Half:   key "mac_dongle" (6 bytes)
     * Then:
     *   esp_now_peer_info_t peer = {
     *     .channel = wifi_ch,
     *     .ifidx   = WIFI_IF_STA,
     *     .encrypt = false,
     *   };
     *   memcpy(peer.peer_addr, mac, 6);
     *   esp_now_add_peer(&peer);
     * For now: log warning and continue (send/recv still works for broadcast tests). */
    ESP_LOGW(TAG, "no peer MACs configured (NVS rf.mac_* not set) — ESP-NOW TX disabled");

    ESP_LOGI(TAG, "ESP-NOW link init OK");
    return true;
}

bool espnow_send(const uint8_t mac[6], uint8_t type, const void *payload, uint16_t len)
{
    /* Build [type][payload] frame */
    uint8_t buf[1 + 250];   /* ESP-NOW max payload is 250 bytes */
    if (len > 249) {
        ESP_LOGE(TAG, "espnow_send: payload too large (%u > 249)", (unsigned)len);
        return false;
    }
    buf[0] = type;
    if (payload && len > 0) {
        memcpy(buf + 1, payload, len);
    }
    esp_err_t e = esp_now_send(mac, buf, 1 + len);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send failed: %d (peer not added?)", e);
        return false;
    }
    return true;
}
```

- [ ] **Step 3: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/espnow/espnow_link.h main/comm/espnow/espnow_link.c
git commit -m "feat(bricks): espnow_link — WiFi STA + esp_now_init + recv dispatch + send"
```

---

## Task 5: CMake — add espnow sources + priv_requires + include dir

**Files:**
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Replace the empty ESPNOW block**

Current content of the ESPNOW CMake block:
```cmake
# ESP-NOW cold path — dongle role only (Plan 5 fills these in)
if(CONFIG_KASE_HAS_ESPNOW)
    # Plan 5 will add: comm/espnow/*.c
endif()
```

Replace with:
```cmake
# ESP-NOW info channel — dongle + half roles (KASE_HAS_ESPNOW=y on both)
if(CONFIG_KASE_HAS_ESPNOW)
    list(APPEND srcs
        "comm/espnow/espnow_link.c"
        "comm/espnow/espnow_info.c"
    )
    # Plan 5 will add: EN_OTA_*, EN_CFG_*, EN_TELEM_* handlers in additional .c files.
endif()
```

- [ ] **Step 2: Verify priv_requires for esp_wifi and esp_event**

Check whether the existing CMakeLists.txt already has:
```cmake
if(CONFIG_KASE_HAS_ESPNOW)
    list(APPEND priv_requires esp_wifi esp_event)
endif()
```

If this block exists (from Plan Half-1), do not add a duplicate.
If it is missing, add it in the priv_requires section.

- [ ] **Step 3: Add `"comm/espnow"` to INCLUDE_DIRS**

In `idf_component_register(SRCS ... INCLUDE_DIRS ...)`, add `"comm/espnow"`.

- [ ] **Step 4: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/CMakeLists.txt
git commit -m "chore(bricks): CMake — espnow sources + comm/espnow include dir"
```

---

## Task 6: Wire espnow init into `half_scan_task.c` and `main.c` (dongle)

**Files:**
- Modify: `main/comm/rf/half_scan_task.c`
- Modify: `main/main.c`
- Modify: `main/comm/rf/dongle_engine_state.c`
- Modify: `main/comm/rf/half_scan_task.c` (add battery TX in heartbeat)

### 6a — half_scan_task.c

- [ ] **Step 1: Add espnow includes and init calls to `half_scan_task.c`**

In the `#ifndef TEST_HOST` include block, add:
```c
#if CONFIG_KASE_HAS_ESPNOW
#include "espnow_info.h"
#include "espnow_link.h"
#endif
```

In `half_scan_task` (task body), after eink init, add:
```c
#if CONFIG_KASE_HAS_ESPNOW
    /* Initialize info-channel state BEFORE ESP-NOW link (mutex must exist before recv fires) */
    espnow_info_init();
    bool espnow_ok = espnow_link_init();
    if (!espnow_ok) {
        ESP_LOGW(TAG, "ESP-NOW init failed — info channel disabled (NRF TX continues)");
    }
#endif /* CONFIG_KASE_HAS_ESPNOW */
```

- [ ] **Step 2: Add battery TX in `heartbeat_timer_cb`**

At the end of `heartbeat_timer_cb`, add:
```c
#if CONFIG_KASE_HAS_ESPNOW
    /* Battery TX: every ~30 s (300 × 100 ms ticks) */
    static uint32_t s_batt_ticks = 0;
    if (++s_batt_ticks >= 300) {
        s_batt_ticks = 0;
        en_battery_t b = {
            .batt_dV  = 0,   /* TODO STUB: read ADC GPIO15 (battery brick, Phase 2+) */
            .soc_pct  = 0,   /* TODO STUB: derive SoC from batt_dV */
            .charging = 0,   /* TODO STUB: gpio_get_level(GPIO46) BMS status */
        };
        /* TODO STUB: load mac_dongle from NVS rf.mac_dongle and call espnow_send().
         * Until MAC is configured, log only — do not send to zero MAC. */
        ESP_LOGD(TAG, "battery stub: dV=%u soc=%u%% chg=%u (not sent — mac_dongle not configured)",
                 b.batt_dV, b.soc_pct, b.charging);
        (void)b;   /* suppress unused warning */
    }
#endif /* CONFIG_KASE_HAS_ESPNOW */
```

Add at the top of the `#ifndef TEST_HOST` includes (if not already there):
```c
#if CONFIG_KASE_HAS_ESPNOW
#include "espnow_msg.h"   /* en_battery_t */
#endif
```

### 6b — main.c (dongle)

- [ ] **Step 3: Read the dongle branch of `main.c`**

```bash
grep -n "CONFIG_KASE_HAS_ESPNOW\|espnow" /home/mae/Documents/GitHub/KaSe_firmware/main/main.c
```

Locate the `#if CONFIG_KASE_HAS_ESPNOW` block in the dongle branch. It may already exist
as a scaffold comment. Add `espnow_link_init()` call there.

- [ ] **Step 4: Edit `main.c` dongle branch**

In the dongle role `#elif CONFIG_KASE_DEVICE_ROLE_DONGLE` block, find or add:
```c
#if CONFIG_KASE_HAS_ESPNOW
    #include "espnow_info.h"
    #include "espnow_link.h"
#endif
```
(at the top of the file, near other role-specific includes)

And in the dongle boot sequence:
```c
#if CONFIG_KASE_HAS_ESPNOW
    espnow_info_init();
    espnow_link_init();   /* ESP-NOW info channel: layer push to halves, battery RX */
#endif
```

### 6c — dongle_engine_state.c: layer_changed hook

- [ ] **Step 5: Edit `dongle_engine_state.c`**

Replace:
```c
/* On keyboards layer_changed() flags the display task; no display on dongle. */
void layer_changed(void) { }
```

With:
```c
/*
 * On keyboards layer_changed() flags the display task.
 * On the dongle: push EN_INFO_LAYER + EN_INFO_STATE to both halves via ESP-NOW.
 */
#if CONFIG_KASE_HAS_ESPNOW
#include "espnow_link.h"
#include "espnow_msg.h"
#endif

void layer_changed(void)
{
#if CONFIG_KASE_HAS_ESPNOW
    /* TODO STUB: push EN_INFO_LAYER + EN_INFO_STATE to halves.
     *
     * When MAC pairing is implemented (Plan 4), load mac_left / mac_right from
     * NVS rf.mac_left / rf.mac_right and call espnow_send() for each.
     *
     * en_layer_t l = {
     *     .layer_idx = current_layout,
     * };
     * strncpy(l.name, get_layer_name(current_layout), 16);
     * espnow_send(mac_left,  EN_INFO_LAYER, &l, sizeof(l));
     * espnow_send(mac_right, EN_INFO_LAYER, &l, sizeof(l));
     *
     * en_state_t s = {
     *     .modifiers = current_modifiers,   // from key_processor.c state
     *     .flags     = 0,                   // TODO: caps_word, bt_connected, usb_active
     * };
     * espnow_send(mac_left,  EN_INFO_STATE, &s, sizeof(s));
     * espnow_send(mac_right, EN_INFO_STATE, &s, sizeof(s));
     *
     * For now: no-op (MACs not configured, espnow_send would fail gracefully). */
#endif /* CONFIG_KASE_HAS_ESPNOW */
}
```

- [ ] **Step 6: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/half_scan_task.c main/main.c \
        main/comm/rf/dongle_engine_state.c
git commit -m "feat(bricks): wire espnow_link_init + layer_changed hook + battery TX stub"
```

---

## Task 7: Build verification — all three targets

- [ ] **Step 1: Build dongle (clean)**

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_dongle \
    -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -20
```

Expected: `Project build complete.`

Common failure modes:
- `undefined reference to 'espnow_link_init'` → check CMake ESPNOW block added `espnow_link.c`.
- `undefined reference to 'g_half_state'` → on dongle this should not be referenced — only
  `eink.c` reads it and `eink.c` is not compiled on the dongle (KASE_HAS_EINK=n).
- `esp_now_recv_info_t` unknown type → verify `esp_now.h` is included; requires esp_wifi
  component in priv_requires.
- `esp_netif_create_default_wifi_sta()` missing → remove this line from `espnow_link.c`
  (it is not needed for ESP-NOW STA mode). The netif for STA is created by `esp_wifi_set_mode`
  + `esp_wifi_start` internally in recent ESP-IDF 5.x.

- [ ] **Step 2: Build half_left (clean)**

```bash
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_half_left \
    -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -20
```

Expected: `Project build complete.`

WiFi is now enabled → binary will be larger (~200-250 KB app vs ~100 KB before). Verify
flash partition table accommodates the binary (factory partition is 2 MB — well within bounds).

Common failure:
- `g_half_state_mutex` not defined → verify `espnow_info.c` defines it inside
  `#if CONFIG_KASE_DEVICE_ROLE_HALF`.
- `en_battery_t` undefined in `half_scan_task.c` → add `#include "espnow_msg.h"` inside
  the KASE_HAS_ESPNOW guard.

- [ ] **Step 3: Build half_right (clean)**

```bash
rm -f /home/mae/Documents/GitHub/KaSe_firmware/sdkconfig
idf.py -B /home/mae/Documents/GitHub/KaSe_firmware/build_half_right \
    -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 \
    -C /home/mae/Documents/GitHub/KaSe_firmware build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 4: Run host tests (espnow_msg + all prior tests)**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -5 && ./test_runner
```

Expected: all tests pass, including `test_espnow_msg`.

---

## Self-review checklist

- [ ] `espnow_info_init()` is called BEFORE `espnow_link_init()` in both `half_scan_task.c`
  and `main.c` (dongle). The recv callback can fire immediately on init; the mutex must
  exist first.
- [ ] `g_half_state` and `g_half_state_mutex` are defined only under
  `#if CONFIG_KASE_DEVICE_ROLE_HALF` in `espnow_info.c`. Dongle build must not define them.
- [ ] `eink.c` includes `espnow_info.h` to access `g_half_state` — but `eink.c` is compiled
  only when `KASE_HAS_EINK=y` (which implies HALF role) → no linker conflict on dongle.
- [ ] Battery TX stub in `heartbeat_timer_cb` is inside `#if CONFIG_KASE_HAS_ESPNOW` — no
  ESP-NOW symbols referenced without the flag.
- [ ] `layer_changed()` in `dongle_engine_state.c` guards `espnow_link.h` and `espnow_msg.h`
  includes with `#if CONFIG_KASE_HAS_ESPNOW` — no WiFi symbols in non-ESPNOW builds.
- [ ] `espnow_link.c` does not call `esp_netif_create_default_wifi_sta()` — this creates an
  unneeded netif that conflicts if called twice. STA mode is set via `esp_wifi_set_mode`.
- [ ] WiFi ch 6 (2437 MHz) is set consistently in sdkconfig.defaults.half_left/right and in
  `espnow_link_init()` default. Dongle must use the same channel (verified in Task 1).
- [ ] All three targets build green. Host tests pass.
- [ ] No peer with zero MAC is added to ESP-NOW — `esp_now_add_peer` with a zero MAC would
  fail; the stub logs a warning and skips the call.
- [ ] `espnow_send()` to a zero MAC is not called — battery TX stub logs only.
- [ ] `espnow_msg.h` structs are `__attribute__((packed))` — verified by host tests checking
  sizeof (battery=3, layer=17, state=2).

---

## Out of scope / stubs left for the implementer

| Stub | Location | What remains |
|------|----------|--------------|
| Peer MAC loading | `espnow_link.c` TODO block | Add NVS keys mac_left/right/dongle; call esp_now_add_peer. Requires Plan 4 (pairing) or manual NVS write for dev. |
| Battery TX enable | `heartbeat_timer_cb` TODO | Load mac_dongle from NVS, call espnow_send(). Depends on mac_dongle NVS key. |
| Battery ADC reading | `heartbeat_timer_cb` batt_dV/soc_pct | Battery ADC brick (GPIO15/16) — separate Phase 2+ plan |
| BMS charging status | `heartbeat_timer_cb` charging | gpio_get_level(GPIO46) — straightforward once GPIO46 configured |
| Battery CDC push on dongle | `espnow_info.c::on_battery` | New KR push frame or KS_CMD_DONGLE_STATS extension |
| `layer_changed` push body | `dongle_engine_state.c` | Load mac_left/right, call espnow_send(). Depends on MACs and get_layer_name(). |
| `on_layer` / `on_state` eink wake | `espnow_info.c` | Expose eink task handle (eink_get_task_handle()); call xTaskNotify |
| `g_half_state` read in eink_task | `eink.c::eink_task` | Replace s_layer_idx_stub with half_state_lock(); copy = g_half_state; half_state_unlock() |
| Plan 5 ESP-NOW OTA/config | `espnow_link.c` recv dispatch | Additional case in espnow_info_dispatch or a new espnow_ota.c; type IDs 0x10-0x2F reserved |
| WiFi modem sleep + power mgmt | sdkconfig | CONFIG_ESP_WIFI_MODEM_SLEEP_DEFAULT=y conjoint with battery brick and CONFIG_PM_ENABLE=y |
| WiFi channel bench validation | bench | Measure rf.link_q MAX_RT count before/after ESP-NOW activation; switch to ch 1 if degraded |
