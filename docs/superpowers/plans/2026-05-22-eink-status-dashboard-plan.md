# Plan — E-Ink Status Dashboard (Dongle → Half via ESP-NOW)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a live status dashboard to the KaSe half e-ink panel: RF link state (up/down per half), per-half signal quality (0..4 bars), and USB active flag. The dongle pushes a new `EN_INFO_STATUS` ESP-NOW message every 5 s; each half updates three new LVGL labels on-change only; a 15 s timeout degrades the display to "?" when the dongle link is lost. End state: flash dongle + half-right (paired set_id=0x9044); the e-ink shows L/R link dots, signal bars, USB indicator, and layer name, all updating ~every 5 s. Unplug a half → its link dot changes within 5–15 s.

**Architecture overview:**

```
[Dongle — esp_timer task, every 5 s]
  status_push_cb()
    ├── rf_rx_get_status(&st)                  ← thread-safe (internal mutex)
    ├── rf_signal_bars(link_up, hb_age_ms, link_q)  → 0..4 per half
    ├── tud_ready()                             → USB active flag
    ├── en_encode_status(buf, &msg)             → 4 bytes
    ├── espnow_send(mac_left,  buf, 4)          ← unicast (lazy-loaded from NVS)
    └── espnow_send(mac_right, buf, 4)

[rf_rx_task — on PKT_HEARTBEAT ingestion]
  rf_decode_heartbeat() → s_status.link_q_left / link_q_right updated

[espnow_link.c — espnow_recv_cb → espnow_info_dispatch()]
  case EN_INFO_STATUS → on_status(&st)

[Half — ESP-NOW recv task context]
  on_status()
    half_state_lock()
    compare 5 fields (link_left/right, sig_left/right, usb_active)
    if changed → update g_half_state + set changed=true
    always update g_half_state.last_status_ms
    half_state_unlock()
    if changed → xTaskNotify(eink_get_task_handle(), 0x02, eSetBits)

[Half — eink_lvgl_task]
  xTaskNotifyWait() → notified (bit 0 = layer, bit 1 = status, both possible)
  check dongle_alive = (now - g_half_state.last_status_ms) < 15000
  if bit 1 or timeout change:
    build_link_label(buf, 'L', dongle_alive, link_left, sig_left)
    build_link_label(buf, 'R', dongle_alive, link_right, sig_right)
    build_usb_label(buf, dongle_alive, usb_active)
    lv_label_set_text(s_label_link_l/r/usb, ...)
    lv_obj_invalidate(...)
  lv_timer_handler() → flush_cb → eink_push()
```

**Spec reference:** `docs/superpowers/specs/2026-05-22-eink-status-dashboard-design.md` — read in full before touching any file.

**Depends on:**
- Plan RF-3 (done): `layer_changed()` unicasts `EN_INFO_LAYER`/`EN_INFO_STATE`; `on_layer()`/`on_state()` fill stubs; `eink_lvgl_task` uses `xTaskNotifyWait` bit 0; `eink_get_task_handle()` is live. This plan mirrors that exact pattern for bit 1.
- Plan Bricks-3 (done): `eink_lvgl.c` hardware-validated; `set_px_cb→s_fb→flush_cb→eink_push()` render path is proven. **Do not touch it.**
- NVS pairing data written by Plan RF-2: `rf.mac_left`, `rf.mac_right` on dongle.

**Graceful-degrades (unpaired / no pairing data):**
- `status_push_cb`: both MACs are all-zeros → neither `espnow_send` fires → log `LOGD`.
- Half: `on_status()` never reached → `last_status_ms` stays 0 → after 15 s, `dongle_alive = false` → labels show `"? [....]"`. Keyboard continues to function normally.

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
- `idf.py -B build_kase_v2    -DBOARD=kase_v2         -DIDF_TARGET=esp32s3 build`  ← must stay green

**Host test command (baseline 972 pass, 0 failed — must stay at 0 failed, grow in count):**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -5 && ./test_runner
```

---

## Critical implementation traps — read before touching any file

**Trap 1 — `esp_timer` callback context is NOT `rf_rx_task`.**
`status_push_cb` runs in the `esp_timer` task, not `rf_rx_task`. It must NOT call any NRF24 SPI functions (`rf_driver_*`). It may call `rf_rx_get_status()` (thread-safe by internal copy under no lock — the struct fields are written atomically as 32-bit values on Xtensa and copied into a local out struct) and `espnow_send()` (thread-safe per ESP-NOW spec). Do not add any mutex acquisition inside the timer callback.

**Trap 2 — `espnow_send()` sends a pre-encoded `[type][payload]` buffer.**
Looking at Plan RF-3's `layer_changed()`: `espnow_send(mac, EN_INFO_LAYER, &l, sizeof(l))`. The function signature in `espnow_link.c` is `bool espnow_send(const uint8_t mac[6], uint8_t type, const void *payload, uint8_t len)` — it prepends the type byte internally. **Do NOT call `en_encode_status()` and pass the encoded buffer to `espnow_send`**; call `espnow_send(mac, EN_INFO_STATUS, &msg, sizeof(msg))` directly, passing the raw `en_status_t` struct as payload (not the 4-byte encoded buffer). Verify this matches the actual `espnow_send` signature in `main/comm/espnow/espnow_link.h` before writing code.

**Trap 3 — `rf_link_status_t.link_q_left/right` fields do not exist yet.**
The struct in `rf_rx_task.h` currently has 8 fields (link_left, link_right, hb_age_left_ms, hb_age_right_ms, pkt_rx_left, pkt_rx_right, pkt_dup_left, pkt_dup_right). Task 1 adds `link_q_left` and `link_q_right` as appended fields. `rf_rx_get_status()` already copies `s_hb_left.link_up` and `s_hb_right.link_up` — the new fields copy `s_hb_left.last_link_q` (a new `uint8_t` added to `hb_half_state_t`... or, simpler: a module-static `s_link_q_left/right` in `rf_rx_task.c` updated at heartbeat ingestion in `drain_radio()`). Check `heartbeat.h` — `hb_half_state_t` does not currently hold `link_q`; store it as a module-static in `rf_rx_task.c` alongside `s_hb_left/s_hb_right`.

**Trap 4 — `rf_signal_bars()` must be host-testable: no ESP-IDF deps.**
Place it as a `static` function inside `rf_rx_task.c` initially, then extract it under a `TEST_HOST` guard so `test/test_rf_signal_bars.c` can include `rf_rx_task.c` compiled with `-DTEST_HOST`. Pattern is identical to `is_known_peer()` in `espnow_link.c`. Alternatively, put it in a small new `rf_signal.c` with only `<stdint.h>/<stdbool.h>`. Either way: no `esp_log.h`, no FreeRTOS, no `esp_timer.h` in the function body.

**Trap 5 — `on_status()` must update `last_status_ms` even if `changed == false`.**
The dongle-link timeout resets on every received packet regardless of whether the payload changed. If `last_status_ms` is only updated when `changed == true`, a stable-good system degrades after 15 s. The field must be updated before the `if (changed)` branch, always, even when the comparison shows no change.

**Trap 6 — `s_showing_degraded` guard in `eink_lvgl_task` is mandatory.**
Without it, when the dongle is persistently absent, the task checks `dongle_alive = false` on every loop iteration (every 50 ms) and calls `lv_obj_invalidate()` each time → continuous 1.5 s full refreshes of the e-ink every ~50 ms = panic. Track a `static bool s_showing_degraded` and only update + invalidate when the degraded state changes.

**Trap 7 — `xTaskNotify` bit assignment: bit 0 = layer (existing), bit 1 = status (new).**
`on_layer()` sends `xTaskNotify(h, 0x01, eSetBits)`. `on_status()` must send `xTaskNotify(h, 0x02, eSetBits)`. Both bits can fire in one `xTaskNotifyWait` result. The task must check `notify_val & 0x01` AND `notify_val & 0x02` independently (both in the same wakeup). Do not change the existing bit-0 path.

**Trap 8 — Label position change for `label_ver`.**
The existing `label_ver` is at `lv_obj_set_pos(60, 100)`. The spec moves it to `(60, 185)` to make room for the new rows. This is a one-line change in `eink_lvgl_init()`. Do not move it to y=100 in any new code.

**Trap 9 — ASCII glyphs only. No Unicode in label strings.**
Montserrat bitmap font in this project does NOT contain `●` (U+25CF), `○` (U+25CB), or `▮`. Use `*` (link up), `-` (link down), `?` (unknown), `|` (filled bar), `.` (empty bar slot). See spec §8.2. Confirmed default; do not add any UTF-8 multibyte sequences unless a lv_conf.h audit explicitly confirms coverage.

---

## File Structure

### Modified

| Path | Change |
|------|--------|
| `main/comm/espnow/espnow_msg.h` | Add `EN_INFO_STATUS 0x04`, `en_status_t`, `en_encode_status()`, `en_decode_status()` |
| `main/comm/rf/rf_rx_task.h` | Append `link_q_left`, `link_q_right` to `rf_link_status_t` |
| `main/comm/rf/rf_rx_task.c` | Add `s_link_q_left/right` module-statics; update on heartbeat; fill `link_q_*` in `rf_rx_get_status()`; add `rf_signal_bars()` (TEST_HOST-guarded); add `status_push_cb` + periodic esp_timer created in `rf_rx_start()` |
| `main/comm/espnow/espnow_info.h` | Add 6 fields to `half_state_t` (link_left, link_right, sig_left, sig_right, usb_active, last_status_ms); update lock wrappers |
| `main/comm/espnow/espnow_info.c` | Add `on_status()` under `CONFIG_KASE_DEVICE_ROLE_HALF`; add `case EN_INFO_STATUS:` in `espnow_info_dispatch()` |
| `main/periph/eink/eink_lvgl.c` | Move `label_ver` to y=185; add `s_label_link_l`, `s_label_link_r`, `s_label_usb`, `s_label_sep_top`, `s_label_sep_bot` static labels; add `build_link_label()`, `build_usb_label()` helpers; extend `eink_lvgl_task` loop with bit-1 handling + degraded-state guard; add dongle-alive timeout check |

### Created

| Path | Responsibility |
|------|----------------|
| `test/test_en_status.c` | Host tests for `en_encode_status`/`en_decode_status` codec round-trip and `build_link_label()`/`build_usb_label()` formatting |
| `test/test_rf_signal_bars.c` | Host tests for `rf_signal_bars()` all threshold cases |

### Modified (test infrastructure)

| Path | Change |
|------|--------|
| `test/CMakeLists.txt` | Add `test_en_status.c`, `test_rf_signal_bars.c`, and `rf_rx_task.c` (compiled with `-DTEST_HOST`) to the executable |
| `test/test_main.c` | Add `extern void test_en_status(void)` + `extern void test_rf_signal_bars(void)` + calls |

### Untouched

```
main/periph/eink/eink.c                 eink_push / eink_fb_set_px — not touched
main/periph/eink/eink.h                 no changes
main/periph/eink/eink_lvgl.h            no changes (eink_get_task_handle already declared)
main/comm/espnow/espnow_link.c/h        no changes
main/comm/rf/dongle_engine_state.c      no changes (layer_changed untouched)
main/comm/rf/rf_packet.h                rf_heartbeat_t.link_q already present — not changed
boards/*/board.h                        no changes
partitions.csv                          no changes
main/idf_component.yml                  no new components
```

---

## Task 1: TDD — `en_status` codec + `build_link_label`/`build_usb_label` formatters

**Purpose:** `en_encode_status`/`en_decode_status` are the wire contract between the dongle and the half. Formatting helpers are pure string logic. TDD these before any integration code exists.

**Files:**
- Modify: `main/comm/espnow/espnow_msg.h`
- Create: `test/test_en_status.c`
- Modify: `test/CMakeLists.txt`
- Modify: `test/test_main.c`

**Rationale:** A wrong type byte or wrong byte order in the codec silently breaks all status updates with no build error. The formatting helpers use a 16-byte-capped buffer; off-by-one overflows stack silently on embedded. Host tests catch both before any embedded flash.

- [ ] **Step 1: Add `EN_INFO_STATUS`, `en_status_t`, encode/decode to `espnow_msg.h`**

After the `EN_INFO_STATE = 0x03` define and before the reserved range comment:

```c
#define EN_INFO_STATUS  0x04   /* dongle → half: link + signal + USB status */

/* ── EN_INFO_STATUS (dongle → half) — 3 bytes ─────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t flags;      /* bit0=link_left_up, bit1=link_right_up,
                           bit2=usb_active, bits3-7=rsvd (must be 0) */
    uint8_t sig_left;   /* signal quality 0..4, 0=link_down or no data */
    uint8_t sig_right;  /* signal quality 0..4, 0=link_down or no data */
} en_status_t;

static inline uint8_t en_encode_status(uint8_t *buf, const en_status_t *st)
{
    buf[0] = EN_INFO_STATUS;
    memcpy(buf + 1, st, sizeof(*st));
    return 1 + sizeof(*st);   /* 4 */
}

static inline bool en_decode_status(const uint8_t *buf, uint8_t len,
                                     en_status_t *out)
{
    if (len < 1 + (uint8_t)sizeof(*out) || buf[0] != EN_INFO_STATUS) return false;
    memcpy(out, buf + 1, sizeof(*out));
    return true;
}
```

The `en_encode_status` / `en_decode_status` pair mirrors the existing `en_encode_layer` / `en_decode_layer` style exactly. No new includes needed — `<stdint.h>`, `<string.h>`, `<stdbool.h>` are already at the top.

- [ ] **Step 2: Write `test/test_en_status.c`**

Write the test file BEFORE verifying compilation (TDD):

```c
/*
 * test_en_status.c — Host tests for:
 *   1. en_encode_status / en_decode_status (codec round-trip, wire byte layout)
 *   2. build_link_label() formatting (ASCII-only glyphs, clamp, sides)
 *   3. build_usb_label() formatting
 *
 * All tested functions are pure (no I/O, no FreeRTOS).
 */
#include "test_framework.h"
#include "../main/comm/espnow/espnow_msg.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ── build_link_label and build_usb_label are defined in eink_lvgl.c.
 * For host testing, reproduce them here as static helpers with the EXACT
 * same logic as the implementation. If the implementation changes, update
 * both. The test validates the specification, not just the current impl.
 *
 * Alternative: extract them to a tiny eink_label_fmt.c with no LVGL deps.
 * The plan defaults to the inline-reproduction approach (no new .c file). */

static void build_link_label(char *buf, char side,
                              bool dongle_alive, bool link_up, uint8_t bars)
{
    const char dot = dongle_alive ? (link_up ? '*' : '-') : '?';
    bars = (bars > 4) ? 4 : bars;
    char bstr[7];
    bstr[0] = '[';
    for (int i = 0; i < 4; i++) bstr[1 + i] = (i < (int)bars) ? '|' : '.';
    bstr[5] = ']';
    bstr[6] = '\0';
    snprintf(buf, 16, "%c %c %s", side, dot, bstr);
}

static void build_usb_label(char *buf, bool dongle_alive, bool usb_active)
{
    const char dot = dongle_alive ? (usb_active ? '*' : '-') : '?';
    snprintf(buf, 10, "USB %c", dot);
}

void test_en_status(void)
{
    TEST_SUITE("en_status_codec");

    /* ── Codec: encode then decode round-trip ────────────────────── */
    {
        en_status_t orig = { .flags = 0x05, .sig_left = 3, .sig_right = 1 };
        uint8_t buf[8];
        uint8_t n = en_encode_status(buf, &orig);
        TEST_ASSERT_EQ(n, 4, "encode returns 4 bytes");
        TEST_ASSERT_EQ(buf[0], 0x04, "type byte is EN_INFO_STATUS");
        TEST_ASSERT_EQ(buf[1], 0x05, "flags byte");
        TEST_ASSERT_EQ(buf[2], 0x03, "sig_left byte");
        TEST_ASSERT_EQ(buf[3], 0x01, "sig_right byte");

        en_status_t dec;
        TEST_ASSERT(en_decode_status(buf, 4, &dec), "decode succeeds");
        TEST_ASSERT_EQ(dec.flags,     0x05, "decoded flags");
        TEST_ASSERT_EQ(dec.sig_left,  3,    "decoded sig_left");
        TEST_ASSERT_EQ(dec.sig_right, 1,    "decoded sig_right");
    }

    /* ── Codec: wrong type byte must reject ──────────────────────── */
    {
        uint8_t buf[4] = { 0x99, 0x05, 0x03, 0x01 };
        en_status_t dec;
        TEST_ASSERT(!en_decode_status(buf, 4, &dec), "wrong type rejected");
    }

    /* ── Codec: short buffer must reject ─────────────────────────── */
    {
        uint8_t buf[4] = { 0x04, 0x05, 0x03, 0x01 };
        en_status_t dec;
        TEST_ASSERT(!en_decode_status(buf, 3, &dec), "len=3 rejected (need 4)");
        TEST_ASSERT(!en_decode_status(buf, 0, &dec), "len=0 rejected");
    }

    /* ── Codec: all-zeros flags round-trip ───────────────────────── */
    {
        en_status_t orig = { .flags = 0x00, .sig_left = 0, .sig_right = 0 };
        uint8_t buf[8];
        en_encode_status(buf, &orig);
        en_status_t dec;
        TEST_ASSERT(en_decode_status(buf, 4, &dec), "all-zeros decode succeeds");
        TEST_ASSERT_EQ(dec.flags, 0x00, "all-zeros flags");
        TEST_ASSERT_EQ(dec.sig_left,  0, "all-zeros sig_left");
        TEST_ASSERT_EQ(dec.sig_right, 0, "all-zeros sig_right");
    }

    /* ── Codec: all bits set ─────────────────────────────────────── */
    {
        en_status_t orig = { .flags = 0x07, .sig_left = 4, .sig_right = 4 };
        uint8_t buf[8];
        en_encode_status(buf, &orig);
        en_status_t dec;
        TEST_ASSERT(en_decode_status(buf, 4, &dec), "max decode succeeds");
        TEST_ASSERT_EQ(dec.flags, 0x07, "all flags set");
        TEST_ASSERT_EQ(dec.sig_left,  4, "sig_left=4");
        TEST_ASSERT_EQ(dec.sig_right, 4, "sig_right=4");
    }

    TEST_SUITE("build_link_label");

    char buf[16];

    /* ── Link up, 4 bars ──────────────────────────────────────────── */
    build_link_label(buf, 'L', true, true, 4);
    TEST_ASSERT(strcmp(buf, "L * [||||]") == 0, "L up 4 bars");

    /* ── Link up, 0 bars ──────────────────────────────────────────── */
    build_link_label(buf, 'R', true, true, 0);
    TEST_ASSERT(strcmp(buf, "R * [....]") == 0, "R up 0 bars");

    /* ── Link up, 3 bars ──────────────────────────────────────────── */
    build_link_label(buf, 'L', true, true, 3);
    TEST_ASSERT(strcmp(buf, "L * [|||.]") == 0, "L up 3 bars");

    /* ── Link up, 2 bars ──────────────────────────────────────────── */
    build_link_label(buf, 'R', true, true, 2);
    TEST_ASSERT(strcmp(buf, "R * [||..]") == 0, "R up 2 bars");

    /* ── Link up, 1 bar ───────────────────────────────────────────── */
    build_link_label(buf, 'L', true, true, 1);
    TEST_ASSERT(strcmp(buf, "L * [|...]") == 0, "L up 1 bar");

    /* ── Link down ────────────────────────────────────────────────── */
    build_link_label(buf, 'L', true, false, 0);
    TEST_ASSERT(strcmp(buf, "L - [....]") == 0, "L link down");

    /* ── Dongle timeout: '?' override regardless of link_up ──────── */
    build_link_label(buf, 'R', false, true,  3);
    TEST_ASSERT(strcmp(buf, "R ? [|||.]") == 0, "R timeout up 3 bars → '?'");
    build_link_label(buf, 'L', false, false, 0);
    TEST_ASSERT(strcmp(buf, "L ? [....]") == 0, "L timeout down → '?'");

    /* ── Clamp bars > 4 → 4 ──────────────────────────────────────── */
    build_link_label(buf, 'L', true, true, 5);
    TEST_ASSERT(strcmp(buf, "L * [||||]") == 0, "bars=5 clamped to 4");
    build_link_label(buf, 'R', true, true, 255);
    TEST_ASSERT(strcmp(buf, "R * [||||]") == 0, "bars=255 clamped to 4");

    TEST_SUITE("build_usb_label");

    char ubuf[10];

    build_usb_label(ubuf, true, true);
    TEST_ASSERT(strcmp(ubuf, "USB *") == 0, "USB active");

    build_usb_label(ubuf, true, false);
    TEST_ASSERT(strcmp(ubuf, "USB -") == 0, "USB inactive");

    build_usb_label(ubuf, false, true);
    TEST_ASSERT(strcmp(ubuf, "USB ?") == 0, "USB timeout (active but unknown)");

    build_usb_label(ubuf, false, false);
    TEST_ASSERT(strcmp(ubuf, "USB ?") == 0, "USB timeout (inactive but unknown)");
}
```

- [ ] **Step 3: Add to `test/CMakeLists.txt`**

In `add_executable(test_runner ...)` source list, add:

```cmake
    test_en_status.c
```

`espnow_msg.h` is header-only (inline functions). No new `.c` file needed for the codec. The `build_link_label`/`build_usb_label` are reproduced inline in the test (see comment in Step 2). No additional includes in CMakeLists for this task.

- [ ] **Step 4: Add to `test/test_main.c`**

```c
extern void test_en_status(void);
```

```c
test_en_status();
```

- [ ] **Step 5: Run host tests — must pass before proceeding to Task 2**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -5
./test_runner 2>&1 | grep -E "en_status|build_link_label|build_usb|FAIL|passed|failed"
```

Expected: all `test_en_status` suites pass. Prior 972 tests still 0 failed.

Common failure: `TEST_ASSERT_EQ(n, 4, ...)` fails if `sizeof(en_status_t) != 3` (struct alignment padded). Verify `__attribute__((packed))` is present. If the assert shows `n=5` → packed attribute missing.

- [ ] **Step 6: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/espnow/espnow_msg.h \
        test/test_en_status.c test/CMakeLists.txt test/test_main.c
git commit -m "test(espnow): EN_INFO_STATUS codec + label formatter TDD"
```

---

## Task 2: TDD — `rf_signal_bars()` pure function

**Purpose:** `rf_signal_bars()` is the threshold logic for the signal quality bars. Wrong thresholds produce wrong bars silently on hardware. TDD the exact spec thresholds before the function is wired into the dongle timer.

**Files:**
- Modify: `main/comm/rf/rf_rx_task.c` (add `rf_signal_bars()` with TEST_HOST guard)
- Modify: `main/comm/rf/rf_rx_task.h` (add declaration under TEST_HOST guard)
- Create: `test/test_rf_signal_bars.c`
- Modify: `test/CMakeLists.txt`
- Modify: `test/test_main.c`

- [ ] **Step 1: Add `rf_signal_bars()` to `rf_rx_task.c`**

Add this block BEFORE any `#ifndef TEST_HOST` guard (pure function, no ESP-IDF deps). Place it near the top of the file, after includes but before `s_left/s_right` declarations:

```c
/*
 * rf_signal_bars() — derive 0..4 signal quality bars for one half.
 *
 * Inputs:
 *   link_up   : true if rf_link_status_t.link_left/right is true
 *   hb_age_ms : ms since last heartbeat (rf_link_status_t.hb_age_*)
 *   link_q    : half-reported MAX_RT cumulative retries (rf_heartbeat_t.link_q)
 *               0 = no retries = perfect; higher = worse RF.
 *
 * Returns: 0..4 (0 = link down or very bad; 4 = excellent)
 *
 * Pure function: no globals, no I/O. Host-testable.
 * Thresholds are normative — match spec §4.2 exactly.
 */
uint8_t rf_signal_bars(bool link_up, uint32_t hb_age_ms, uint8_t link_q)
{
    /* Link is considered down if rf_rx_task flagged it, OR if the
     * heartbeat age exceeds 3× the nominal heartbeat interval (500 ms
     * → 1500 ms threshold). 3× gives margin for two missed HBs. */
    if (!link_up || hb_age_ms >= 1500u) return 0;

    /* Age score: lower age = better freshness */
    uint8_t age_score;
    if      (hb_age_ms <  200u) age_score = 4;
    else if (hb_age_ms <  400u) age_score = 3;
    else if (hb_age_ms <  700u) age_score = 2;
    else if (hb_age_ms < 1200u) age_score = 1;
    else                         age_score = 0;

    /* Retry score: lower link_q = fewer retries = better RF */
    uint8_t retry_score;
    if      (link_q == 0)  retry_score = 4;
    else if (link_q <= 2)  retry_score = 3;
    else if (link_q <= 5)  retry_score = 2;
    else if (link_q <= 10) retry_score = 1;
    else                    retry_score = 0;

    /* Minimum of both scores: both dimensions must be good to show 4 bars. */
    return (age_score < retry_score) ? age_score : retry_score;
}
```

Add to `main/comm/rf/rf_rx_task.h` (inside a guard so it is usable from the test without linking the full task):

```c
/* Signal quality derivation — pure function, host-testable.
 * Returns 0..4 signal bars. 0 = link down. See rf_rx_task.c for thresholds. */
uint8_t rf_signal_bars(bool link_up, uint32_t hb_age_ms, uint8_t link_q);
```

Guard the ESP-IDF-specific sections of `rf_rx_task.c` with `#ifndef TEST_HOST`:

```c
#ifndef TEST_HOST
/* all existing includes + globals + task functions */
#include "rf_rx_task.h"
#include "rf_driver.h"
/* ... */
static rf_radio_t s_left, s_right;
/* ... all existing functions ... */
#endif /* TEST_HOST */
```

The `rf_signal_bars` function itself stays OUTSIDE the `#ifndef TEST_HOST` guard.

- [ ] **Step 2: Write `test/test_rf_signal_bars.c`**

```c
/*
 * test_rf_signal_bars.c — Host tests for rf_signal_bars().
 *
 * Validates all threshold boundaries from spec §4.2 (normative).
 */
#define TEST_HOST
#include "test_framework.h"
#include "../main/comm/rf/rf_rx_task.h"
#include <stdbool.h>
#include <stdint.h>

void test_rf_signal_bars(void)
{
    TEST_SUITE("rf_signal_bars");

    /* ── Link down → always 0 ──────────────────────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(false,    0, 0), 0, "link_down age=0 q=0 → 0");
    TEST_ASSERT_EQ(rf_signal_bars(false, 5000, 0), 0, "link_down age=5000 → 0");
    TEST_ASSERT_EQ(rf_signal_bars(false,  100, 0), 0, "link_down perfect age → 0");

    /* ── Age timeout → always 0 ─────────────────────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 1500, 0), 0, "age=1500 (boundary) → 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 2000, 0), 0, "age=2000 → 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1500, 5), 0, "age=1500 q=5 → 0");

    /* ── Excellent: age < 200, q == 0 → min(4,4) = 4 ───────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true,   0, 0), 4, "age=0 q=0 → 4");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 0), 4, "age=100 q=0 → 4");
    TEST_ASSERT_EQ(rf_signal_bars(true, 199, 0), 4, "age=199 q=0 → 4");

    /* ── Good: age < 200, q 1..2 → min(4,3) = 3 ────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 1), 3, "age=100 q=1 → 3");
    TEST_ASSERT_EQ(rf_signal_bars(true, 150, 2), 3, "age=150 q=2 → 3");

    /* ── age < 200, q 3..5 → min(4,2) = 2 ──────────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 3), 2, "age=100 q=3 → 2");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 5), 2, "age=100 q=5 → 2");

    /* ── age < 200, q 6..10 → min(4,1) = 1 ─────────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 6),  1, "age=100 q=6 → 1");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 10), 1, "age=100 q=10 → 1");

    /* ── age < 200, q > 10 → min(4,0) = 0 ──────────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 11), 0, "age=100 q=11 → 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 15), 0, "age=100 q=15 → 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 100, 255),0, "age=100 q=255 → 0");

    /* ── age 200..399, q == 0 → min(3,4) = 3 ───────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 200, 0), 3, "age=200 q=0 → 3");
    TEST_ASSERT_EQ(rf_signal_bars(true, 300, 0), 3, "age=300 q=0 → 3");
    TEST_ASSERT_EQ(rf_signal_bars(true, 399, 0), 3, "age=399 q=0 → 3");

    /* ── age 400..699, q == 0 → min(2,4) = 2 ───────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 400, 0), 2, "age=400 q=0 → 2");
    TEST_ASSERT_EQ(rf_signal_bars(true, 500, 0), 2, "age=500 q=0 → 2");
    TEST_ASSERT_EQ(rf_signal_bars(true, 699, 0), 2, "age=699 q=0 → 2");

    /* ── age 400..699, q 3..5 → min(2,2) = 2 ───────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true, 500, 4), 2, "age=500 q=4 → 2");

    /* ── age 700..1199, q == 0 → min(1,4) = 1 ──────────────────── */
    TEST_ASSERT_EQ(rf_signal_bars(true,  700, 0), 1, "age=700 q=0 → 1");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1000, 0), 1, "age=1000 q=0 → 1");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1199, 0), 1, "age=1199 q=0 → 1");

    /* ── age boundary: 1499 → age_score=1 (< 1200 is false, so else=0)
     *    Wait — 1200 <= 1499 < 1500 → age_score=0 (last else branch)
     *    But link is still up (hb_age < 1500), so overall = min(0, retry_score) = 0 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 1200, 0), 0, "age=1200 → age_score=0 → 0");
    TEST_ASSERT_EQ(rf_signal_bars(true, 1499, 0), 0, "age=1499 → age_score=0 → 0");

    /* ── Minimum rule: both must be good ────────────────────────── */
    /* age=300 (age_score=3), q=5 (retry_score=2) → min=2 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 300, 5), 2, "age=300 q=5 → min(3,2)=2");
    /* age=400 (age_score=2), q=0 (retry_score=4) → min=2 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 400, 0), 2, "age=400 q=0 → min(2,4)=2");
    /* age=200 (age_score=3), q=10 (retry_score=1) → min=1 */
    TEST_ASSERT_EQ(rf_signal_bars(true, 200, 10), 1, "age=200 q=10 → min(3,1)=1");
}
```

- [ ] **Step 3: Add to `test/CMakeLists.txt`**

In the `add_executable(test_runner ...)` source list, add:

```cmake
    test_rf_signal_bars.c
    ../main/comm/rf/rf_rx_task.c
```

In `target_compile_definitions` or `target_compile_options`, add `-DTEST_HOST` for `rf_rx_task.c`. Since all other guarded files use the same approach, add to the existing test compile definitions:

```cmake
target_compile_definitions(test_runner PRIVATE TEST_HOST)
```

In `target_include_directories`, add:

```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/rf
```

- [ ] **Step 4: Guard ESP-IDF-dependent code in `rf_rx_task.c`**

Wrap all includes and globals that use FreeRTOS/ESP-IDF in `#ifndef TEST_HOST`:

```c
/* rf_signal_bars() is pure — no guard, compiles in both firmware and test. */

#ifndef TEST_HOST
#include "rf_driver.h"
#include "rf_packet.h"
#include "heartbeat.h"
/* ... all other ESP-IDF includes ... */
#include "esp_log.h"
#include "esp_timer.h"
/* ... */

static rf_radio_t s_left, s_right;
/* ... all task-local statics, functions, etc. ... */

#endif /* TEST_HOST */
```

The `rf_signal_bars` function body is OUTSIDE the guard. It only needs `<stdint.h>` and `<stdbool.h>` which are in the header.

- [ ] **Step 5: Add to `test/test_main.c`**

```c
extern void test_rf_signal_bars(void);
```

```c
test_rf_signal_bars();
```

- [ ] **Step 6: Run host tests — must pass before proceeding**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make 2>&1 | tail -10
./test_runner 2>&1 | grep -E "rf_signal_bars|FAIL|passed|failed"
```

Expected: all `test_rf_signal_bars` assertions pass. Prior tests still 0 failed.

Common failures:
- `undefined reference to rf_signal_bars` → `rf_rx_task.c` not in CMakeLists sources, or function body is inside `#ifndef TEST_HOST` guard — move it out.
- `age=1499 q=0 → 1` fails → boundary case: `hb_age_ms < 1200` is false for 1499, so age_score=0 (else branch), overall=0. Test expects 0. If implementation returns 1, check the `else if (hb_age_ms < 1200)` branch boundary — must be strict `<`.

- [ ] **Step 7: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/rf_rx_task.c main/comm/rf/rf_rx_task.h \
        test/test_rf_signal_bars.c test/CMakeLists.txt test/test_main.c
git commit -m "test(rf): rf_signal_bars() TDD — all threshold boundaries"
```

---

## Task 3: Dongle — `rf_link_status_t` link_q fields + 5 s status push timer

**Purpose:** Wire the dongle side: append `link_q_left/right` to the status struct, update them on heartbeat ingestion in `drain_radio()`, and create the 5 s `esp_timer` in `rf_rx_start()` that sends `EN_INFO_STATUS` to both paired halves.

**Files:**
- Modify: `main/comm/rf/rf_rx_task.h` (append fields to `rf_link_status_t`)
- Modify: `main/comm/rf/rf_rx_task.c` (module-statics, heartbeat update, timer callback, timer create)

- [ ] **Step 1: Append `link_q_left/right` to `rf_link_status_t` in `rf_rx_task.h`**

After the existing `pkt_dup_left, pkt_dup_right` fields:

```c
typedef struct {
    bool link_left, link_right;
    uint32_t hb_age_left_ms, hb_age_right_ms;
    uint32_t pkt_rx_left, pkt_rx_right;
    uint32_t pkt_dup_left, pkt_dup_right;
    /* New: last link_q reported by each half in PKT_HEARTBEAT.
     * 0 if no heartbeat received yet (conservative = best retry score). */
    uint8_t link_q_left;
    uint8_t link_q_right;
} rf_link_status_t;
```

Appending fields preserves backward compatibility for any existing callers of `rf_rx_get_status()`.

- [ ] **Step 2: Add module-statics and update heartbeat ingestion in `rf_rx_task.c`**

Inside `#ifndef TEST_HOST`, after the existing `s_hb_left`, `s_hb_right` declarations:

```c
/* Last link_q received from each half (PKT_HEARTBEAT.link_q).
 * Read by rf_rx_get_status() → status_push_cb → rf_signal_bars(). */
static uint8_t s_link_q_left  = 0;
static uint8_t s_link_q_right = 0;
```

In `drain_radio()`, inside the `else if (type == PKT_TYPE_HEARTBEAT)` branch, AFTER `hb_reconcile(...)`:

```c
        } else if (type == PKT_TYPE_HEARTBEAT) {
            rf_heartbeat_t h;
            if (rf_decode_heartbeat(buf, n, &h)) {
                uint32_t now = esp_timer_get_time() / 1000;
                hb_reconcile(hb, half, &h, &s_cb, now);
                /* Update last link_q for signal quality derivation */
                if (half == HB_HALF_LEFT)  s_link_q_left  = h.link_q;
                else                        s_link_q_right = h.link_q;
                changed = true;
            }
        }
```

In `rf_rx_get_status()`, add the new field fills:

```c
void rf_rx_get_status(rf_link_status_t *out)
{
    uint32_t now = esp_timer_get_time() / 1000;
    out->link_left        = s_hb_left.link_up;
    out->link_right       = s_hb_right.link_up;
    out->hb_age_left_ms   = now - s_hb_left.last_hb_ms;
    out->hb_age_right_ms  = now - s_hb_right.last_hb_ms;
    out->pkt_rx_left      = s_left.pkt_rx;
    out->pkt_rx_right     = s_right.pkt_rx;
    out->pkt_dup_left     = s_left.pkt_dup;
    out->pkt_dup_right    = s_right.pkt_dup;
    out->link_q_left      = s_link_q_left;
    out->link_q_right     = s_link_q_right;
}
```

- [ ] **Step 3: Add `status_push_cb` and create the timer in `rf_rx_start()`**

Inside `#ifndef TEST_HOST`, add the timer handle and callback. Add the handle near other module-statics:

```c
static esp_timer_handle_t s_status_push_timer = NULL;
```

Add the callback (requires the includes already present: `esp_timer.h`, `esp_now` via `espnow_link.h`, `nvs.h`, `tusb.h`). Add after `rf_rx_get_status()`:

```c
/*
 * status_push_cb — periodic esp_timer callback (5 s period).
 *
 * Context: esp_timer task (NOT rf_rx_task).
 * Safe to call: rf_rx_get_status() (no SPI, copies atomically),
 *               espnow_send() (thread-safe per ESP-NOW spec),
 *               tud_ready() (TinyUSB, thread-safe read).
 * Must NOT call: any rf_driver_* (SPI), xSemaphoreTake with portMAX_DELAY.
 */
static void status_push_cb(void *arg)
{
    (void)arg;

    /* Lazy-load paired half MACs once from NVS "rf" (same pattern as layer_changed()). */
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

    bool has_left  = mac_left[0]  | mac_left[1]  | mac_left[2]  |
                     mac_left[3]  | mac_left[4]  | mac_left[5];
    bool has_right = mac_right[0] | mac_right[1] | mac_right[2] |
                     mac_right[3] | mac_right[4] | mac_right[5];

    if (!has_left && !has_right) {
        ESP_LOGD(TAG, "status_push_cb: no paired halves — skip");
        return;
    }

    /* Get current RF link diagnostics */
    rf_link_status_t st;
    rf_rx_get_status(&st);

    /* Build EN_INFO_STATUS payload */
    en_status_t msg;
    msg.sig_left  = rf_signal_bars(st.link_left,  st.hb_age_left_ms,  st.link_q_left);
    msg.sig_right = rf_signal_bars(st.link_right, st.hb_age_right_ms, st.link_q_right);
    msg.flags = 0;
    if (st.link_left)  msg.flags |= (1u << 0);
    if (st.link_right) msg.flags |= (1u << 1);
    if (tud_ready())   msg.flags |= (1u << 2);   /* USB active */

    ESP_LOGD(TAG, "status_push: flags=0x%02x sig_l=%u sig_r=%u",
             msg.flags, msg.sig_left, msg.sig_right);

    /* Unicast to each paired half */
    if (has_left)  espnow_send(mac_left,  EN_INFO_STATUS, &msg, sizeof(msg));
    if (has_right) espnow_send(mac_right, EN_INFO_STATUS, &msg, sizeof(msg));
}
```

Add required includes inside the `#ifndef TEST_HOST` block (after the existing includes):

```c
#include "espnow_link.h"    /* espnow_send() */
#include "espnow_msg.h"     /* en_status_t, EN_INFO_STATUS */
#include "nvs.h"            /* nvs_open, nvs_get_blob, nvs_close */
#include "tusb.h"           /* tud_ready() */
```

At the end of `rf_rx_start()`, after `xTaskCreatePinnedToCore(rf_rx_task, ...)` and before `return true`:

```c
    /* ── 5 s periodic status push to paired halves ───────────────
     * Created AFTER ESP-NOW is initialized (called from main.c after
     * espnow_link_init()). The timer is created here (in rf_rx_start) because
     * rf_rx_task owns the RF status and calling rf_rx_get_status() from the
     * timer avoids cross-module coupling. The callback is safe from the
     * esp_timer task context (see status_push_cb comment). */
    const esp_timer_create_args_t status_args = {
        .callback = status_push_cb,
        .name     = "status_push_tick",
    };
    if (esp_timer_create(&status_args, &s_status_push_timer) == ESP_OK) {
        esp_timer_start_periodic(s_status_push_timer, 5 * 1000 * 1000ULL);  /* 5 s in µs */
        ESP_LOGI(TAG, "status push timer started (5 s period)");
    } else {
        ESP_LOGW(TAG, "status push timer create failed — status push disabled");
    }
```

- [ ] **Step 4: Build dongle — verify green**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

Common failures:
- `implicit declaration of espnow_send` → `espnow_link.h` include missing inside `#ifndef TEST_HOST`.
- `implicit declaration of tud_ready` → `tusb.h` include missing; on the dongle `CONFIG_USB_OTG_SUPPORTED=y` and `tusb.h` is in the TinyUSB include path.
- `'en_status_t' undeclared` → `espnow_msg.h` include missing.
- `rf_signal_bars` inside `#ifndef TEST_HOST` → move the function body outside the guard.

- [ ] **Step 5: Run host tests — baseline must stay green**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner 2>&1 | tail -5
```

Expected: 0 failed. `rf_rx_task.c` compiled with `TEST_HOST` compiles only `rf_signal_bars`; all FreeRTOS/ESP-IDF paths are guarded.

- [ ] **Step 6: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/rf/rf_rx_task.h main/comm/rf/rf_rx_task.c
git commit -m "feat(dongle): 5s status push timer — rf_signal_bars + EN_INFO_STATUS to halves"
```

---

## Task 4: Half — `half_state_t` additions + `on_status()` + dispatch wiring

**Purpose:** Extend the half's shared state struct with the 6 new fields, implement `on_status()` following the `on_layer()` pattern exactly, and wire `EN_INFO_STATUS` into `espnow_info_dispatch()`.

**Files:**
- Modify: `main/comm/espnow/espnow_info.h` (6 new fields in `half_state_t`)
- Modify: `main/comm/espnow/espnow_info.c` (`on_status()` + dispatch case)

- [ ] **Step 1: Add 6 fields to `half_state_t` in `espnow_info.h`**

After the existing `uint8_t flags` field:

```c
typedef struct {
    /* Existing fields (unchanged) */
    uint8_t layer_idx;
    char    layer_name[16];
    uint8_t modifiers;
    uint8_t flags;           /* bit0=caps_word, bit1=bt_connected, bit2=usb_active (on_state) */

    /* New: status dashboard fields driven by EN_INFO_STATUS ─────── */
    bool     link_left;       /* Left half RF link up (set by on_status) */
    bool     link_right;      /* Right half RF link up */
    uint8_t  sig_left;        /* 0..4 signal bars, left */
    uint8_t  sig_right;       /* 0..4 signal bars, right */
    bool     usb_active;      /* Dongle USB active (tud_ready()) — use this for display */
    uint32_t last_status_ms;  /* esp_timer_get_time()/1000 at last EN_INFO_STATUS recv */
} half_state_t;
```

Note: `usb_active` in `half_state_t` (set by `on_status`) is distinct from `flags.bit2` (set by `on_state`). The e-ink task uses `half_state_t.usb_active` exclusively for the USB label. The `flags.bit2` field remains for backward compatibility.

The `half_state_lock()` / `half_state_unlock()` inline wrappers use `portMAX_DELAY` — **change them to 10 ms timeout** to be consistent with `on_layer()` / `on_status()` (which use `pdMS_TO_TICKS(10)`). Alternatively, document that callers must use `xSemaphoreTake` directly with a 10 ms timeout and not use the `portMAX_DELAY` wrappers for the callbacks. The plan takes the latter approach: callback code calls `xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(10))` directly (matching `on_layer()`), not the `half_state_lock()` macro.

- [ ] **Step 2: Add `on_status()` to `espnow_info.c`**

Inside the `#if CONFIG_KASE_DEVICE_ROLE_HALF` block, after `on_state()`:

```c
/* ── Half: receives EN_INFO_STATUS from dongle ────────────────── */
static void on_status(const en_status_t *st)
{
    bool link_left  = (st->flags & (1u << 0)) != 0;
    bool link_right = (st->flags & (1u << 1)) != 0;
    bool usb_active = (st->flags & (1u << 2)) != 0;

    bool changed = false;

    /* Update g_half_state under mutex (10 ms timeout, same as on_layer). */
    if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (g_half_state.link_left  != link_left  ||
            g_half_state.link_right != link_right ||
            g_half_state.usb_active != usb_active ||
            g_half_state.sig_left   != st->sig_left ||
            g_half_state.sig_right  != st->sig_right) {

            g_half_state.link_left  = link_left;
            g_half_state.link_right = link_right;
            g_half_state.usb_active = usb_active;
            g_half_state.sig_left   = st->sig_left;
            g_half_state.sig_right  = st->sig_right;
            changed = true;
        }
        /* Always update last_status_ms — resets the dongle-link timeout
         * regardless of whether the payload changed (Trap 5). */
        g_half_state.last_status_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        xSemaphoreGive(g_half_state_mutex);
    } else {
        ESP_LOGW(TAG, "on_status: mutex timeout — update skipped");
        return;
    }

    /* Wake eink_lvgl_task — bit 1 = status event (distinct from bit 0 = layer). */
    if (changed) {
        TaskHandle_t h = eink_get_task_handle();
        if (h != NULL) {
            xTaskNotify(h, 0x02, eSetBits);
        }
    }
    /* If unchanged: last_status_ms was updated (timeout reset) but no notify sent.
     * No e-ink refresh needed for an identical payload. */
}
```

Add the `esp_timer.h` include inside `#if CONFIG_KASE_DEVICE_ROLE_HALF` (for `esp_timer_get_time()`):

```c
#include "esp_timer.h"   /* esp_timer_get_time() — for last_status_ms */
```

- [ ] **Step 3: Add `case EN_INFO_STATUS:` to `espnow_info_dispatch()`**

Inside the `#if CONFIG_KASE_DEVICE_ROLE_HALF` section of the `switch (type)` block, after `case EN_INFO_STATE:`:

```c
    case EN_INFO_STATUS: {
        en_status_t st;
        if (en_decode_status(buf, len, &st)) {
            on_status(&st);
        }
        break;
    }
```

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
- `implicit declaration of esp_timer_get_time` → `esp_timer.h` not included under `CONFIG_KASE_DEVICE_ROLE_HALF`.
- `'en_status_t' undeclared` → `espnow_msg.h` is included via `espnow_info.h`, which includes it. If not, add `#include "espnow_msg.h"` explicitly.
- `'last_status_ms' undeclared` → field not yet in `half_state_t` → check Step 1 was applied.

- [ ] **Step 5: Verify dongle still builds green**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.` (dongle compiles `espnow_info.c` with `CONFIG_KASE_DEVICE_ROLE_DONGLE`; `on_status` is inside `#if CONFIG_KASE_DEVICE_ROLE_HALF`).

- [ ] **Step 6: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/comm/espnow/espnow_info.h main/comm/espnow/espnow_info.c
git commit -m "feat(half): on_status() — g_half_state link/sig/usb + xTaskNotify bit1"
```

---

## Task 5: E-ink — status labels + dongle-link timeout in `eink_lvgl_task`

**Purpose:** Add the three new LVGL labels (L-line, R-line, USB-line) and the two separator labels. Extend `eink_lvgl_task` to handle bit-1 notifies and the 15 s dongle-link timeout. Move `label_ver` to y=185. **Preserve the proven render path (`full_refresh=0`, `set_px_cb→s_fb`, `flush_cb→eink_push`, X-mirror) — only ADD labels and extend the task loop.**

**Files:**
- Modify: `main/periph/eink/eink_lvgl.c` only

- [ ] **Step 1: Add `build_link_label()` and `build_usb_label()` helpers**

Add as `static` functions just before `eink_lvgl_init()` (after the existing static variables):

```c
/*
 * build_link_label() — format a link-line string into buf (min 16 bytes).
 * side: 'L' or 'R'. dongle_alive: false = show '?' override.
 * ASCII-only glyphs: '*' up, '-' down, '?' unknown, '|' bar, '.' empty.
 * Output example: "L * [|||.]" (13 chars + NUL, well within 16 bytes).
 */
static void build_link_label(char *buf, char side,
                              bool dongle_alive, bool link_up, uint8_t bars)
{
    const char dot = dongle_alive ? (link_up ? '*' : '-') : '?';
    bars = (bars > 4) ? 4 : bars;   /* clamp */
    char bstr[7];   /* "[||||]" + NUL */
    bstr[0] = '[';
    for (int i = 0; i < 4; i++) bstr[1 + i] = (i < (int)bars) ? '|' : '.';
    bstr[5] = ']';
    bstr[6] = '\0';
    snprintf(buf, 16, "%c %c %s", side, dot, bstr);
}

/*
 * build_usb_label() — format the USB status line into buf (min 10 bytes).
 */
static void build_usb_label(char *buf, bool dongle_alive, bool usb_active)
{
    const char dot = dongle_alive ? (usb_active ? '*' : '-') : '?';
    snprintf(buf, 10, "USB %c", dot);
}
```

- [ ] **Step 2: Add static label pointers for the new labels**

After the existing `static lv_obj_t *s_label_layer = NULL;` line, add:

```c
/* ── Status dashboard labels — added by eink-status-dashboard plan ─
 * Initialized in eink_lvgl_init(). Updated on bit-1 notify or timeout. */
static lv_obj_t *s_label_sep_top  = NULL;   /* static separator */
static lv_obj_t *s_label_link_l   = NULL;   /* L link + signal line */
static lv_obj_t *s_label_link_r   = NULL;   /* R link + signal line */
static lv_obj_t *s_label_sep_bot  = NULL;   /* static separator */
static lv_obj_t *s_label_usb      = NULL;   /* USB status line */
```

- [ ] **Step 3: Add a `s_showing_degraded` guard static**

After the label pointers:

```c
/* Tracks whether the task is currently showing the "dongle lost" degraded view.
 * Guards against re-invalidating every 50 ms loop iteration when the dongle is
 * persistently absent (which would cause continuous 1.5 s e-ink refreshes). */
static bool s_showing_degraded = false;
```

- [ ] **Step 4: Extend `eink_lvgl_init()` — move label_ver + create new labels**

In `eink_lvgl_init()`, find the existing `label_ver` creation:

```c
    lv_obj_set_pos(label_ver, 60, 100);
```

Change to `y=185`:

```c
    lv_obj_set_pos(label_ver, 60, 185);
```

After the existing `s_label_layer` and `label_ver` creation lines, add:

```c
    /* ── Status dashboard labels ─────────────────────────────────────
     * Separator, L-line, R-line, separator, USB-line.
     * Initial text shows "unknown" state (dongle not yet connected). */

    s_label_sep_top = lv_label_create(scr);
    lv_label_set_text(s_label_sep_top, "-------------------");
    lv_obj_set_style_text_color(s_label_sep_top, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(s_label_sep_top, 10, 105);

    s_label_link_l = lv_label_create(scr);
    lv_label_set_text(s_label_link_l, "L ? [....]");
    lv_obj_set_style_text_color(s_label_link_l, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(s_label_link_l, 20, 120);

    s_label_link_r = lv_label_create(scr);
    lv_label_set_text(s_label_link_r, "R ? [....]");
    lv_obj_set_style_text_color(s_label_link_r, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(s_label_link_r, 20, 140);

    s_label_sep_bot = lv_label_create(scr);
    lv_label_set_text(s_label_sep_bot, "-------------------");
    lv_obj_set_style_text_color(s_label_sep_bot, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(s_label_sep_bot, 10, 155);

    s_label_usb = lv_label_create(scr);
    lv_label_set_text(s_label_usb, "USB ?");
    lv_obj_set_style_text_color(s_label_usb, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(s_label_usb, 20, 170);

    ESP_LOGI(TAG, "status dashboard labels created");
```

- [ ] **Step 5: Extend `eink_lvgl_task()` with bit-1 handling + timeout check**

The existing task loop handles bit 0 (layer). Extend it to also handle bit 1 (status) and the dongle-link timeout. The timeout check runs on every loop iteration (not only on notify) to detect persistent dongle absence.

Replace the existing `eink_lvgl_task()` body with the following. The layer-update block (bit 0) is preserved verbatim from Plan RF-3; only the `if (notified == pdTRUE)` section is extended and the timeout check is added after it:

```c
static void eink_lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "eink_lvgl_task started");

    for (;;) {
        /* Run LVGL timers (drives pending invalidations from previous notify). */
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms == 0 || sleep_ms > 50) sleep_ms = 50;

        /* Wait for LVGL timer OR notify (bit 0 = layer, bit 1 = status). */
        uint32_t notify_val = 0;
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &notify_val,
                                              pdMS_TO_TICKS(sleep_ms));

        /* ── Bit 0: layer/state changed ──────────────────────────── */
        if (notified == pdTRUE && (notify_val & 0x01)) {
            char name_copy[17] = {0};
            if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                memcpy(name_copy, g_half_state.layer_name, 16);
                xSemaphoreGive(g_half_state_mutex);
            }
            name_copy[16] = '\0';
            if (name_copy[0] == '\0') memcpy(name_copy, "KaSe", 5);

            if (s_label_layer != NULL && lv_obj_is_valid(s_label_layer)) {
                lv_label_set_text(s_label_layer, name_copy);
                lv_obj_invalidate(s_label_layer);
                ESP_LOGI(TAG, "eink: layer → '%s'", name_copy);
            }
        }

        /* ── Dongle-link timeout check (runs every loop, ~50 ms) ──
         * Compute dongle_alive independently of whether a notify fired.
         * This catches persistent absence without requiring a notify. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        bool dongle_alive = false;
        bool link_left  = false;
        bool link_right = false;
        bool usb_active = false;
        uint8_t sig_left  = 0;
        uint8_t sig_right = 0;

        if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            dongle_alive = (now_ms - g_half_state.last_status_ms) < 15000u;
            link_left    = g_half_state.link_left;
            link_right   = g_half_state.link_right;
            usb_active   = g_half_state.usb_active;
            sig_left     = g_half_state.sig_left;
            sig_right    = g_half_state.sig_right;
            xSemaphoreGive(g_half_state_mutex);
        }

        /* ── Bit 1: status changed (on_status notified) ──────────── */
        bool update_status = (notified == pdTRUE && (notify_val & 0x02));

        /* ── Degraded state transition: dongle lost / recovered ───── */
        bool newly_degraded   = (!dongle_alive && !s_showing_degraded);
        bool newly_recovered  = ( dongle_alive &&  s_showing_degraded);
        if (newly_degraded) {
            s_showing_degraded = true;
            update_status = true;   /* force label update to '?' */
        }
        if (newly_recovered) {
            s_showing_degraded = false;
            update_status = true;   /* force label update from '?' to real values */
        }

        /* ── Update status labels if needed ───────────────────────── */
        if (update_status) {
            char lbuf[16], rbuf[16], ubuf[10];
            build_link_label(lbuf, 'L', dongle_alive, link_left,  sig_left);
            build_link_label(rbuf, 'R', dongle_alive, link_right, sig_right);
            build_usb_label(ubuf, dongle_alive, usb_active);

            if (s_label_link_l != NULL && lv_obj_is_valid(s_label_link_l)) {
                lv_label_set_text(s_label_link_l, lbuf);
                lv_obj_invalidate(s_label_link_l);
            }
            if (s_label_link_r != NULL && lv_obj_is_valid(s_label_link_r)) {
                lv_label_set_text(s_label_link_r, rbuf);
                lv_obj_invalidate(s_label_link_r);
            }
            if (s_label_usb != NULL && lv_obj_is_valid(s_label_usb)) {
                lv_label_set_text(s_label_usb, ubuf);
                lv_obj_invalidate(s_label_usb);
            }
            ESP_LOGI(TAG, "eink: status → %s  %s  %s (alive=%d)", lbuf, rbuf, ubuf, dongle_alive);
        }

        /* Stack headroom check — log once */
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

**Why this structure is correct:**
- Bit-0 (layer) and bit-1 (status) are checked in the same wakeup — both can fire together.
- The `dongle_alive` check runs every loop iteration (~50 ms) so the degraded transition is detected within one loop even when no notify fires (persistent absence case).
- `s_showing_degraded` ensures the transition only triggers one invalidation, not continuous refreshes.
- The `newly_recovered` path re-renders real values immediately when the dongle comes back.
- `full_refresh=0`, `set_px_cb→s_fb→flush_cb→eink_push`, and X-mirror are untouched.

- [ ] **Step 6: Build half_right — verify green**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

Common failures:
- `implicit declaration of esp_timer_get_time` → `esp_timer.h` already included in `eink_lvgl.c` (for `s_tick_timer`) — verify.
- `s_showing_degraded` undeclared → Step 3 not applied.
- `build_link_label` / `build_usb_label` undeclared → Step 1 not applied (functions must be defined BEFORE `eink_lvgl_task`).
- Stack overflow at build (large stack frame from `lbuf/rbuf/ubuf` + `name_copy`) → 16+16+10+17 = 59 bytes on stack, well within 4096. Not an issue.

- [ ] **Step 7: Build half_left + dongle — verify green**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.` for both.

- [ ] **Step 8: Commit**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware
git add main/periph/eink/eink_lvgl.c
git commit -m "feat(eink): status dashboard labels — L/R/USB lines + 15s dongle timeout"
```

---

## Task 6: Build-green checkpoint — all 4 boards

Full clean rebuild from scratch. kase_v2 must still build (no regression from half_state_t struct change — `espnow_info.h` is not included by kase_v2 build since `CONFIG_KASE_HAS_ESPNOW` is not set there).

- [ ] **Step 1: Rebuild dongle**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

- [ ] **Step 2: Rebuild half_left**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_left -DBOARD=kase_half_left -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

- [ ] **Step 3: Rebuild half_right**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.`

- [ ] **Step 4: Rebuild kase_v2 (regression check)**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_kase_v2 -DBOARD=kase_v2 -DIDF_TARGET=esp32s3 build 2>&1 \
  | grep -iE "Project build complete|error:" | head -5'
```

Expected: `Project build complete.` (`espnow_info.h` / `espnow_msg.h` are only compiled under `KASE_HAS_ESPNOW`; kase_v2 does not set this).

- [ ] **Step 5: Run full host test suite**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. && make && ./test_runner
```

Expected: count ≥ 995 (972 baseline + ~23 new cases from `test_en_status` + `test_rf_signal_bars`), 0 failed.

---

## Task 7: Bench validation — flash + paired set + visual verification

**Hardware required:** one dongle + one half-right (paired set_id=0x9044, NVS pairing data present from Plan RF-2). No code changes permitted except a stack bump (see Step 6).

- [ ] **Step 1: Flash dongle**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build && \
  idf.py -B build_dongle -p /dev/ttyUSB0 flash'
```

- [ ] **Step 2: Flash half_right + open monitor**

```bash
bash -c 'source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && \
  idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build && \
  idf.py -B build_half_right -p /dev/ttyUSB1 flash monitor'
```

- [ ] **Step 3: Verify boot log on half**

Expected log lines (order may vary):

```
I espnow_info: espnow_info_init OK (half role)
I espnow_link: ESP-NOW peer 0 registered: <dongle MAC> ch=<N>
I eink_lvgl: LVGL init OK, display registered (200x200, 1bpp, set_px_cb)
I eink_lvgl: status dashboard labels created
I eink_lvgl: eink_lvgl_task started
I eink_lvgl: eink_lvgl_task stack HWM: <N> words free
```

Expected log lines on dongle:

```
I rf_rx: RF RX started (L=1 R=1)
I rf_rx: status push timer started (5 s period)
```

- [ ] **Step 4: Verify initial e-ink render (unpaired / first boot)**

After ~3 s (first SSD1681 full refresh):
- Layer name (center): `"KaSe"` (no EN_INFO_LAYER received yet)
- L line: `"L ? [....]"` (dongle alive = false for first 15 s? No — `last_status_ms=0`, `now_ms` starts at ~0 ms, so `dongle_alive = true` for 15 s. But no EN_INFO_STATUS received yet means values are default zeros. Wait: `dongle_alive` starts true, so labels show `-` for link down and `.` bars. Correct initial display: `"L - [....]"` (dongle_alive=true, link_left=false, sig_left=0).)
- R line: `"R - [....]"`
- USB line: `"USB -"` (usb_active=false)
- Version string at y=185 (bottom area)

After first EN_INFO_STATUS (~5 s after dongle boot): labels update to real values.

- [ ] **Step 5: Verify periodic 5 s status update**

On half monitor, every ~5 s:

```
I eink_lvgl: eink: status → L * [||||]  R * [|||.]  USB * (alive=1)
```

Or similar, depending on actual signal quality. After update: e-ink panel refreshes (~1.5 s) and shows updated bars.

- [ ] **Step 6: Verify layer update still works (regression from Plan RF-3)**

From the KaSe controller software, switch the active layer (e.g., layer 1 "Nav"). Expected:

```
I espnow_info: layer update: idx=1 name='Nav'
I eink_lvgl: eink: layer → 'Nav'
```

After ~1.5 s: panel shows "Nav" (not "KaSe"). Switch back: panel updates.

- [ ] **Step 7: Verify dongle-link timeout — unplug the dongle**

Power off or disconnect the dongle. Wait 15–20 s. Expected on half:

```
I eink_lvgl: eink: status → L ? [....]  R ? [....]  USB ? (alive=0)
```

Panel refreshes to show `?` indicators. After reconnecting the dongle: within ~5 s the labels recover to real values and `s_showing_degraded` transitions back.

- [ ] **Step 8: Stack bump if needed**

If `eink_lvgl_task stack HWM: N words free` shows N < 128 (< 512 bytes):

In `eink_lvgl_start()`, change `4096` → `6144`.

```bash
git add main/periph/eink/eink_lvgl.c
git commit -m "fix(eink): bump eink_lvgl_task stack to 6144 (HWM < 512 bytes)"
```

Otherwise no commit for Task 7.

---

## Self-review checklist

- [ ] `EN_INFO_STATUS = 0x04` is the next free value after `0x03` (EN_INFO_STATE). No conflict with OTA range (`0x10+`). Verified in `espnow_msg.h`.
- [ ] `en_status_t` is `__attribute__((packed))`, 3 bytes. Wire: 1 (type) + 3 = 4 bytes. Well within 250-byte ESP-NOW limit.
- [ ] `en_encode_status` / `en_decode_status` are inline in `espnow_msg.h` (header-only), consistent with `en_encode_layer`, `en_decode_layer`. No new `.c` file needed.
- [ ] `rf_signal_bars()` is outside any `#ifndef TEST_HOST` guard in `rf_rx_task.c` — compiles in both firmware and test builds.
- [ ] `rf_rx_task.h` declares `rf_signal_bars()` without ESP-IDF deps (`<stdint.h>`, `<stdbool.h>` only) — host test can include it.
- [ ] `s_link_q_left/right` module-statics in `rf_rx_task.c` default to 0 (BSS) = "no retries" = retry_score=4 (best). Conservative before first heartbeat: conservative means bars start at 4 if age is young, which is fine — it degrades correctly once age grows.
- [ ] `status_push_cb` is in `esp_timer` task context — no SPI calls, no `portMAX_DELAY` mutex, no `rf_driver_*`. Only `rf_rx_get_status()` + `espnow_send()` + `tud_ready()`.
- [ ] `rf_rx_get_status()` fills `link_q_left/right` in the `rf_link_status_t` out struct — consistent with the appended fields in Step 1 of Task 3.
- [ ] Lazy MAC loading in `status_push_cb` uses a `static macs_loaded` bool — safe because the callback fires from the `esp_timer` task (single-threaded timer dispatch in ESP-IDF). No mutex needed (same rationale as `layer_changed()` lazy load).
- [ ] `on_status()` updates `last_status_ms` BEFORE the `if (changed)` branch, always — dongle-link timeout resets on every received packet (Trap 5).
- [ ] `xTaskNotify(h, 0x02, eSetBits)` — bit 1 for status, distinct from bit 0 (layer). No collision.
- [ ] `eink_lvgl_task` checks both `notify_val & 0x01` AND `notify_val & 0x02` independently in the same wakeup — both can fire together.
- [ ] `s_showing_degraded` prevents continuous e-ink refreshes when dongle is persistently absent (Trap 6).
- [ ] `dongle_alive` timeout check runs every loop iteration (~50 ms) — not only on notify. Detects persistent absence without a stale notify.
- [ ] `label_ver` moved from `(60, 100)` to `(60, 185)` in `eink_lvgl_init()` — one-line change, no other impact.
- [ ] All ASCII glyphs: `*`, `-`, `?`, `|`, `.` — no Unicode, no `●`, no `▮` (Trap 9 / spec §8.2).
- [ ] `build_link_label()` and `build_usb_label()` are static in `eink_lvgl.c`; also reproduced in `test_en_status.c` for host testing. If implementation changes, both must be updated.
- [ ] `lv_obj_is_valid()` called before every `lv_label_set_text()` — safe against `display_clear_screen()` races.
- [ ] `full_refresh=0`, `set_px_cb→s_fb`, `flush_cb→eink_push`, X-mirror in `eink_lvgl_flush_cb` are untouched (proven render path from Plan Bricks-3).
- [ ] `half_state_t` additions add ~9 bytes (bool×3=3B, uint8_t×2=2B, uint32_t=4B). No ABI break — this struct is never stored in NVS.
- [ ] kase_v2 build is verified green at Task 6 Step 4 — `espnow_info.h` changes do not affect non-ESPNOW builds.
- [ ] Host test count grows from 972 to ≥ 995 (0 failed) at Task 6 Step 5.

---

## Out of scope

| Feature | Status |
|---|---|
| Battery level display | Out of scope — ADC not implemented on half. Row space reserved on screen (between separator-bot and label_ver). |
| WPM (words per minute) | Out of scope — no key stats forwarding from dongle yet. |
| Partial e-ink refresh | Out of scope — full refresh only (~1.5 s). Acceptable at 5 s cadence. |
| Unicode link/signal glyphs (`●`, `▮`) | Out of scope as default. Requires verified Montserrat range extension in `lv_conf.h` — document in the commit if upgraded post-implementation. |
| BLE connection dot on e-ink | `flags.bit1=bt_connected` in `en_state_t` exists but is not displayed in this layout. Reserved. |
| OTA / config push over ESP-NOW | Reserved type range `0x10+`; out of scope. |
| Per-half battery push (EN_INFO_BATTERY) forwarded to e-ink | Already stubbed in `on_battery()` (dongle); not wired to e-ink display. |
| `uint32_t` overflow in `last_status_ms` after ~49 days | Accepted — any reboot resets `esp_timer_get_time()`. Documented in spec §6.2. |
