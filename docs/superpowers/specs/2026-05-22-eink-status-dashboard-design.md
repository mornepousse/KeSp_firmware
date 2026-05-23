# E-Ink Status Dashboard — KaSe Half (Dongle → Half via ESP-NOW)

**Date:** 2026-05-22
**Branch:** `dongle-firmware`
**Status:** Design spec — approved, ready to implement
**Target boards:** `kase_half_left`, `kase_half_right`
**ESP-IDF:** 5.5
**Panel:** WeAct 1.54" SSD1681, 200×200 px, 1bpp, full refresh only

---

## 1. Overview

The live-layer e-ink path is working end-to-end: dongle `layer_changed()` →
`espnow_send(EN_INFO_LAYER)` → half `on_layer()` → `g_half_state` update →
`xTaskNotify` → `eink_lvgl_task` → `lv_label_set_text(s_label_layer, ...)` →
`eink_push()`. That path is the template; this spec builds on it without touching
it.

This spec adds:

1. **EN_INFO_STATUS** — a new ESP-NOW message type the dongle sends periodically
   (~5 s) to both halves, carrying RF link status (up/down per half) + per-half
   signal quality (0..4 bars) + USB active flag.
2. **`half_state_t` additions** — link/signal/USB/timeout fields.
3. **Three new LVGL labels** on the e-ink: L line, R line, USB line.
4. **On-change-only refresh discipline** — the 5 s push rate-limits the
   1.5 s e-ink refresh; identical status does not trigger a redraw.
5. **Dongle-link timeout on the half** — if no EN_INFO_STATUS is received for
   >15 s, the half degrades its display to show link/USB as unknown.

Battery is **out of scope** (ADC not implemented on either half). Space for a
battery row is reserved on-screen but no data is shown.

---

## 2. Screen Layout (200×200, monochrome)

```
┌──────────────────────┐
│                      │
│                      │
│       B A S E        │  ← s_label_layer (existing, x=60 y=80)
│                      │
│  ─────────────────── │  ← static separator label
│                      │
│  L  ●  [|||·]        │  ← s_label_link_l (x=20, y=120)
│  R  ●  [||||]        │  ← s_label_link_r (x=20, y=140)
│                      │
│  ─────────────────── │  ← static separator label
│                      │
│  USB ●   v3.7.12     │  ← s_label_usb + existing label_ver (x=20, y=170)
│                      │
│                      │  ← battery row reserved (blank, no data)
│                      │
└──────────────────────┘
```

### Field descriptions

| Field | Source | When unknown/down |
|---|---|---|
| Layer name (center) | `g_half_state.layer_name` | `"KaSe"` (existing fallback) |
| `L` link dot | `g_half_state.link_left` | `○` |
| `L` signal bars | `g_half_state.sig_left` (0..4) | `"[····]"` (0 bars) |
| `R` link dot | `g_half_state.link_right` | `○` |
| `R` signal bars | `g_half_state.sig_right` (0..4) | `"[····]"` (0 bars) |
| USB dot | `g_half_state.usb_active` | `○` |
| Firmware version | `esp_app_get_description()->version` | static, never changes |

### Label positions (LVGL coordinates, origin top-left)

| Label | `lv_obj_set_pos(x, y)` | Initial text |
|---|---|---|
| `s_label_layer` | `(60, 80)` | `"KaSe"` (existing) |
| `s_label_sep_top` | `(10, 105)` | `"-------------------"` |
| `s_label_link_l` | `(20, 120)` | `"L o [....]"` |
| `s_label_link_r` | `(20, 140)` | `"R o [....]"` |
| `s_label_sep_bot` | `(10, 155)` | `"-------------------"` |
| `s_label_usb` | `(20, 170)` | `"USB o"` |
| `label_ver` (existing) | `(60, 100)` | firmware version string |

The existing `label_ver` is moved from `y=100` to `y=185` (bottom-right corner,
`lv_obj_set_pos(60, 185)`) to free vertical space for the new rows. If the
version string is longer than ~8 characters it may clip — acceptable at 1bpp.

---

## 3. EN_INFO_STATUS Message

### 3.1 Type ID

```c
#define EN_INFO_STATUS  0x04   /* dongle → half: link + signal + USB status */
```

Assigned as next free value in the `0x00-0x0F` info-channel range (0x01..0x03
already allocated, 0x05-0x0F reserved for future info messages). No impact on
the OTA/config/telemetry reserved ranges (0x10+).

### 3.2 Packed struct

```c
/* ── EN_INFO_STATUS (dongle → half) — 3 bytes ─────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t flags;      /* bit0=link_left_up, bit1=link_right_up,
                           bit2=usb_active, bits3-7=rsvd (must be 0) */
    uint8_t sig_left;   /* signal quality 0..4, 0=link_down or no data */
    uint8_t sig_right;  /* signal quality 0..4, 0=link_down or no data */
} en_status_t;
```

Wire format: `[0x04][flags:u8][sig_left:u8][sig_right:u8]` — 4 bytes total
(type byte + 3-byte struct), well within the 250-byte ESP-NOW payload limit.

Flags bitmap:

| Bit | Name | Meaning |
|---|---|---|
| 0 | `link_left_up` | Left half RF link is live |
| 1 | `link_right_up` | Right half RF link is live |
| 2 | `usb_active` | `tud_ready()` is true on the dongle |
| 3-7 | reserved | Must be zero; half must ignore on decode |

### 3.3 Encode / decode helpers

Added to `espnow_msg.h` alongside the existing `en_encode_*` / `en_decode_*` set.
Both functions are pure (no I/O), host-testable.

```c
static inline uint8_t en_encode_status(uint8_t *buf, const en_status_t *st)
{
    buf[0] = EN_INFO_STATUS;
    memcpy(buf + 1, st, sizeof(*st));
    return 1 + sizeof(*st);   /* 4 */
}

static inline bool en_decode_status(const uint8_t *buf, uint8_t len,
                                     en_status_t *out)
{
    if (len < 1 + sizeof(*out) || buf[0] != EN_INFO_STATUS) return false;
    memcpy(out, buf + 1, sizeof(*out));
    return true;
}
```

---

## 4. Signal Quality Derivation (dongle side)

### 4.1 Inputs

From `rf_rx_get_status(&s)`:

- `s.link_left` / `s.link_right` — bool: heartbeat recently received.
- `s.hb_age_left_ms` / `s.hb_age_right_ms` — ms since last heartbeat.
- `s.pkt_rx_left` / `s.pkt_rx_right` — cumulative packets received (not used
  for bars, but available for future debug).

From the `rf_heartbeat_t.link_q` field embedded in each heartbeat packet — the
half reports its cumulative MAX_RT retry count since the previous heartbeat. The
dongle stores the most-recently-received `link_q` per half internally in
`rf_rx_task` (already parsed at heartbeat ingestion; exposed as needed — see
§6.2 for the implementation note on surfacing `link_q`).

### 4.2 Pure derivation function

```c
/*
 * rf_signal_bars() — derive 0..4 signal quality bars for one half.
 *
 * Inputs:
 *   link_up    : true if rf_link_status_t.link_left/right is true
 *   hb_age_ms  : milliseconds since last heartbeat (rf_link_status_t.hb_age_*)
 *   link_q     : half-reported MAX_RT cumulative retries (rf_heartbeat_t.link_q)
 *                0 = no retries = perfect; higher = worse RF.
 *
 * Returns: 0..4 (0 = link down or very bad; 4 = excellent)
 *
 * This is a pure function: no globals, no I/O. Host-testable.
 */
static uint8_t rf_signal_bars(bool link_up, uint32_t hb_age_ms, uint8_t link_q)
{
    /* Link is considered down if rf_rx_task already flagged it, OR if the
     * heartbeat age exceeds 3× the nominal heartbeat interval (assumed 500 ms
     * → 1500 ms threshold). 3× gives margin for two missed HBs. */
    if (!link_up || hb_age_ms >= 1500) return 0;

    /* Both dimensions matter: freshness (age) and retry load (link_q).
     * Score each independently, then take the minimum (worst wins). */

    /* Age score: lower age = better freshness */
    uint8_t age_score;
    if      (hb_age_ms <  200) age_score = 4;
    else if (hb_age_ms <  400) age_score = 3;
    else if (hb_age_ms <  700) age_score = 2;
    else if (hb_age_ms < 1200) age_score = 1;
    else                        age_score = 0;

    /* Retry score: lower link_q = fewer retries = better RF */
    uint8_t retry_score;
    if      (link_q == 0) retry_score = 4;
    else if (link_q <= 2) retry_score = 3;
    else if (link_q <= 5) retry_score = 2;
    else if (link_q <= 10) retry_score = 1;
    else                   retry_score = 0;

    /* Minimum of both scores: a stale heartbeat overrides a clean retry count,
     * and vice-versa. This ensures both dimensions must be good to show 4 bars. */
    return (age_score < retry_score) ? age_score : retry_score;
}
```

### 4.3 Threshold table (normative)

| Condition | Bars |
|---|---|
| `link_up == false` OR `hb_age_ms >= 1500` | **0** (link down) |
| age < 200 ms AND link_q == 0 | **4** (excellent) |
| age < 200 ms AND link_q 1..2 | **3** (good) |
| age < 400 ms AND link_q 0..2 | **3** |
| age < 400 ms AND link_q 3..5 | **2** |
| age < 700 ms AND link_q 0..5 | varies, see min() rule |
| link_q > 10 (any age) | **1** max |
| age >= 1200 ms (any link_q) | **1** max |

The minimum rule in `rf_signal_bars()` is authoritative; the table above is
illustrative of representative cases.

### 4.4 `link_q` per-half storage in rf_rx_task

`rf_link_status_t` does not currently expose `link_q`. The implementation must
add two fields: `uint8_t link_q_left, link_q_right;` (updated each time a
`PKT_HEARTBEAT` is parsed for that half, inside `rf_rx_task`). These are read by
`rf_rx_get_status()` without breaking the existing struct layout (fields are
appended). See §6.2 for the minimal change note.

---

## 5. Data Flow and Cadence

### 5.1 Periodic dongle tick (~5 s)

**Location:** An `esp_timer` (one-shot retriggered, or periodic) created in
`rf_rx_start()` (or in `espnow_link_init()` after ESP-NOW is ready). Name:
`"status_push_tick"`. Period: **5 000 000 µs (5 s)**.

Rationale for `rf_rx_start()`: `rf_rx_task` already holds the radios and calls
`rf_rx_get_status()` internally; creating the timer there avoids a new
cross-module dependency. The timer callback context is the esp_timer task (not
`rf_rx_task`), so it must not call NRF SPI functions — it only calls
`rf_rx_get_status()` (thread-safe by internal mutex in rf_rx_task) and
`espnow_send()` (thread-safe per esp_now spec).

**Timer callback pseudocode:**

```c
static void status_push_cb(void *arg)
{
    /* 1. Get current RF link status from rf_rx_task */
    rf_link_status_t st;
    rf_rx_get_status(&st);

    /* 2. Get per-half link_q (new fields, see §4.4) */
    /* rf_rx_get_status fills st.link_q_left, st.link_q_right */

    /* 3. Derive signal bars */
    en_status_t msg;
    msg.sig_left  = rf_signal_bars(st.link_left,  st.hb_age_left_ms,  st.link_q_left);
    msg.sig_right = rf_signal_bars(st.link_right, st.hb_age_right_ms, st.link_q_right);
    msg.flags = 0;
    if (st.link_left)   msg.flags |= (1u << 0);
    if (st.link_right)  msg.flags |= (1u << 1);
    if (tud_ready())    msg.flags |= (1u << 2);   /* USB active */

    /* 4. Unicast to both paired halves (lazy MACs loaded once, same pattern
     *    as layer_changed() in dongle_engine_state.c) */
    /* espnow_send(mac_left,  EN_INFO_STATUS, &msg, sizeof(msg)); */
    /* espnow_send(mac_right, EN_INFO_STATUS, &msg, sizeof(msg)); */
}
```

The MAC loading follows the identical lazy-static pattern from `layer_changed()`:
static `mac_left[6]`, `mac_right[6]`, `macs_loaded` bool; loaded once from NVS
`"rf"` on first callback invocation.

`tud_ready()` is from `tusb.h` — already included in the dongle build.

### 5.2 Half reception: `on_status()`

Added to `espnow_info.c` under `CONFIG_KASE_DEVICE_ROLE_HALF`, following the
`on_layer()` / `on_state()` pattern exactly.

```c
static void on_status(const en_status_t *st)
{
    /* Compute new values */
    bool link_left  = (st->flags & (1u << 0)) != 0;
    bool link_right = (st->flags & (1u << 1)) != 0;
    bool usb_active = (st->flags & (1u << 2)) != 0;

    bool changed = false;

    half_state_lock();   /* 10 ms timeout, same pattern as on_layer */
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
    g_half_state.last_status_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    half_state_unlock();

    if (changed) {
        TaskHandle_t h = eink_get_task_handle();
        if (h != NULL) {
            xTaskNotify(h, 0x02, eSetBits);   /* bit 1 = status event */
        }
    }
    /* If unchanged: last_status_ms was updated (dongle-link timeout reset),
     * but no xTaskNotify is sent — no e-ink refresh needed. */
}
```

`xTaskNotify` uses **bit 1** (`0x02`) for the status event, distinct from the
existing bit 0 used by `on_layer()` / `on_state()`. The eink task checks both.

### 5.3 Dispatch wiring

`espnow_info_dispatch()` gains a new `case EN_INFO_STATUS:` branch under
`CONFIG_KASE_DEVICE_ROLE_HALF`, following the existing `EN_INFO_LAYER` pattern.

### 5.4 E-ink refresh: on-change only

`eink_lvgl_task` already wakes on `xTaskNotifyWait`. The notify bit determines
what to update:

- **Bit 0** (existing): layer/state changed — update `s_label_layer`.
- **Bit 1** (new): status changed — update `s_label_link_l`, `s_label_link_r`,
  `s_label_usb`.

Both bits can fire in the same `xTaskNotifyWait` result (they are ORed by
`eSetBits`). The task processes all set bits before returning to `lv_timer_handler`.

If `on_status()` receives an identical payload, `changed == false`, no notify is
sent, and `eink_lvgl_task` does not wake for a status redraw. This prevents
needless 1.5 s e-ink refreshes when the system is stable.

**Worst-case refresh latency after a real status change:**

- Dongle detects change: at next 5 s tick send.
- Half receives, `changed=true`, `xTaskNotify(bit 1)`.
- `eink_lvgl_task` wakes within `sleep_ms` (≤50 ms), updates labels, calls
  `lv_obj_invalidate()`.
- `lv_timer_handler()` next iteration triggers flush → `eink_push()` → ~1.5 s
  hardware refresh.

Total worst case: **5 s + 50 ms + 1.5 s ≈ 6.6 s** from actual state change to
visible update. This is acceptable for a status indicator (not a real-time
display).

---

## 6. Dongle-Link Timeout on the Half

### 6.1 Timeout value

**15 seconds** (15 000 ms). This is 3× the 5 s push cadence, giving the dongle
three missed sends before the half degrades its display. WiFi interference could
delay a single ESP-NOW packet; 3× provides adequate margin.

### 6.2 Detection mechanism

`eink_lvgl_task` checks the timeout on every loop iteration (not only on notify):

```c
/* Inside the eink_lvgl_task loop, after xTaskNotifyWait returns: */

uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
bool dongle_alive;

half_state_lock();
dongle_alive = (now_ms - g_half_state.last_status_ms) < 15000u;
half_state_unlock();

if (!dongle_alive) {
    /* Dongle link lost: override link/usb display to "unknown" state.
     * Do NOT modify g_half_state — just render the degraded view. */
    /* Update s_label_link_l → "L ? [....]"  */
    /* Update s_label_link_r → "R ? [....]"  */
    /* Update s_label_usb   → "USB ?"         */
    /* Invalidate and let lv_timer_handler redraw if text changed. */
}
```

The `last_status_ms` field is initialized to 0 (zero). At boot, `now_ms` will
immediately exceed 15 s (since `esp_timer_get_time()` starts at ~0 and
`last_status_ms` is 0, the difference is ~0 ms and `dongle_alive` is initially
true for the first 15 s). This gives the system 15 s after boot to receive the
first EN_INFO_STATUS before the display degrades — sufficient time for ESP-NOW
channel negotiation and initial pairing.

**Overflow note:** `esp_timer_get_time()` is 64-bit µs; dividing to ms and
casting to `uint32_t` overflows after ~49 days. For a keyboard, this is
acceptable — any reboot resets the timer. If overflow protection is needed,
compare the 64-bit values directly.

### 6.3 Degraded display strings

When `dongle_alive == false`:

| Label | Degraded text |
|---|---|
| `s_label_link_l` | `"L ? [....]"` |
| `s_label_link_r` | `"R ? [....]"` |
| `s_label_usb` | `"USB ?"` |

The `?` indicates "status unknown" (not necessarily "down"). The eink task must
track whether it is already showing the degraded view (via a static bool
`s_showing_degraded`) to avoid re-invalidating on every loop iteration when the
dongle is persistently absent — this prevents continuous ~1.5 s refreshes.

---

## 7. `half_state_t` Additions

The `half_state_t` struct in `espnow_info.h` gains five fields:

```c
typedef struct {
    /* Existing fields (unchanged) */
    uint8_t layer_idx;
    char    layer_name[16];
    uint8_t modifiers;
    uint8_t flags;           /* bit0=caps_word, bit1=bt_connected, bit2=usb_active */

    /* New: status dashboard fields (EN_INFO_STATUS driven) */
    bool    link_left;       /* Left half RF link up */
    bool    link_right;      /* Right half RF link up */
    uint8_t sig_left;        /* 0..4 signal bars, left */
    uint8_t sig_right;       /* 0..4 signal bars, right */
    bool    usb_active;      /* Dongle USB active (tud_ready()) */
    uint32_t last_status_ms; /* esp_timer_get_time()/1000 at last EN_INFO_STATUS recv */
} half_state_t;
```

Note: `usb_active` now appears both in `half_state_t.usb_active` (new, set by
`on_status()`) and in the legacy `flags` bit 2 of `g_half_state.flags` (set by
`on_state()`). The eink task should use `half_state_t.usb_active` exclusively for
the USB dot display. The `flags` bit 2 field remains for backwards compatibility
with `on_state()` but is not read by the status dashboard labels.

**Mutex discipline:** All accesses to the new fields follow the existing pattern —
`half_state_lock()` / `half_state_unlock()` from any task that reads or writes.
Mutex timeout: 10 ms (consistent with `on_layer()` / `on_state()`).

The `g_half_state_mutex` is defined only on the HALF role (`CONFIG_KASE_DEVICE_ROLE_HALF`),
unchanged.

---

## 8. E-Ink Labels — Text Formatting and Font / Glyph Caveat

### 8.1 Link and signal label format

The `s_label_link_l` and `s_label_link_r` labels use a single text string
combining the side indicator, link dot, and signal bars:

```
"L ● [||||]"   → link up, 4 bars
"L ● [|||·]"   → link up, 3 bars
"L ● [||··]"   → link up, 2 bars
"L ● [|···]"   → link up, 1 bar
"L ● [····]"   → link up, 0 bars (very weak)
"L ○ [····]"   → link down
"L ? [....]"   → dongle-link timeout (unknown)
```

The signal bar glyph set `[||||]` / `[···]` uses **ASCII pipe characters**
(`|` = 0x7C, filled bar) and **middle dots** (`·` = 0xB7, UTF-8: `\xC2\xB7`,
empty bar slot). The dot character `●` (filled circle, U+25CF, UTF-8:
`\xE2\x97\x8F`) and `○` (open circle, U+25CB, UTF-8: `\xE2\x97\x8B`) are
Unicode.

### 8.2 CRITICAL: LVGL Montserrat font and Unicode coverage

**The default LVGL Montserrat bitmap font (included via `lv_font_montserrat_*.c`)
does NOT contain arbitrary Unicode block characters or box-drawing glyphs.** Its
Unicode range is limited to the Basic Latin block (U+0020-U+007E) plus a small set
of European accented characters. Specifically:

- `|` (U+007C) — ASCII pipe, **present** in Montserrat. Safe to use.
- `·` middle dot (U+00B7) — in the Latin-1 Supplement block. **Check at build
  time** whether the Montserrat variant compiled into the project includes this
  glyph. If absent, LVGL renders a blank box.
- `●` U+25CF and `○` U+25CB — Geometric Shapes block. **Almost certainly absent**
  from the compiled Montserrat font unless explicitly range-enabled in `lv_conf.h`
  via `LV_FONT_MONTSERRAT_*` or a custom range.
- `▮` U+25AE / `▯` U+25AF and similar block glyphs — **not present** in any
  standard LVGL Montserrat compile.

**Decision: use ASCII-only glyphs.** This is the safe fallback that is guaranteed
to work with any Montserrat variant and does not require font changes:

| Symbol | ASCII representation | Bytes |
|---|---|---|
| Link up dot | `*` | 1 |
| Link down dot | `-` | 1 |
| Link unknown | `?` | 1 |
| 4 signal bars | `[||||]` | 8 |
| 3 signal bars | `[|||.]` | 8 |
| 2 signal bars | `[||..]` | 8 |
| 1 signal bar  | `[|...]` | 8 |
| 0 signal bars | `[....]` | 8 |

Full example strings:

```
"L * [||||]"   → left link up, 4 bars
"L - [....]"   → left link down
"R * [|...]"   → right link up, 1 bar
"USB *"         → USB active
"USB -"         → USB inactive
```

**If the project enables a custom font or extended Montserrat range** (verify in
`lv_conf.h` by searching for `LV_FONT_CUSTOM_DECLARE` or `LV_FONT_MONTSERRAT_28`
range settings), it is possible to upgrade to Unicode glyphs. This should be
verified at implementation time and documented in the commit that adds the labels.

**Action at implementation time:** Search `lv_conf.h` and `CMakeLists.txt` for
font configuration. If `LV_FONT_MONTSERRAT_*` covers U+00B7 (middle dot), it is
safe to use `\xC2\xB7` for the empty-bar glyph. For dots (`●` `○`), the risk of
missing glyphs is high enough that ASCII `*` and `-` are the default choice
regardless.

### 8.3 Label helper function

The eink task calls a small pure helper to build the label string:

```c
/*
 * build_link_label() — format a link-line string into buf (min 16 bytes).
 * side: 'L' or 'R'.
 * link_up: true = link alive, false = link down.
 * bars: 0..4.
 * dongle_alive: false = show "?" override.
 */
static void build_link_label(char *buf, char side,
                              bool dongle_alive, bool link_up, uint8_t bars)
{
    const char dot = dongle_alive ? (link_up ? '*' : '-') : '?';
    bars = (bars > 4) ? 4 : bars;   /* clamp */

    /* Signal bar string: `bars` pipes, then `4-bars` dots, in brackets */
    char bstr[7];   /* "[||||]" + NUL */
    bstr[0] = '[';
    for (int i = 0; i < 4; i++) bstr[1 + i] = (i < (int)bars) ? '|' : '.';
    bstr[5] = ']';
    bstr[6] = '\0';

    snprintf(buf, 16, "%c %c %s", side, dot, bstr);
}
```

Called from `eink_lvgl_task` after reading `g_half_state` under mutex. `snprintf`
with a hard-coded 16-byte cap is safe; the result is always ≤15 chars + NUL.

### 8.4 USB label

```c
static void build_usb_label(char *buf, bool dongle_alive, bool usb_active)
{
    const char dot = dongle_alive ? (usb_active ? '*' : '-') : '?';
    snprintf(buf, 10, "USB %c", dot);
}
```

`label_ver` (existing firmware version) is rendered as a separate label and is
not modified by the status path.

---

## 9. `rf_link_status_t` Minor Addition (dongle side)

The existing struct in `rf_rx_task.h` gains two fields to expose per-half `link_q`:

```c
typedef struct {
    bool link_left, link_right;
    uint32_t hb_age_left_ms, hb_age_right_ms;
    uint32_t pkt_rx_left, pkt_rx_right;
    uint32_t pkt_dup_left, pkt_dup_right;
    /* New: last link_q reported by each half in PKT_HEARTBEAT */
    uint8_t link_q_left;    /* 0 if no heartbeat received yet */
    uint8_t link_q_right;
} rf_link_status_t;
```

Updated inside `rf_rx_task` at heartbeat ingestion:
`s_status.link_q_left = hb.link_q;` (for the left half slot). Already parsed via
`rf_decode_heartbeat()` and available in the existing heartbeat handling path.

---

## 10. Testing

All test targets run host-side under `test/` (CMake standalone, no embedded
target required).

### 10.1 `en_status` codec round-trip

```c
/* test: en_encode_status → en_decode_status — exact byte layout */
en_status_t orig = { .flags=0x05, .sig_left=3, .sig_right=1 };
uint8_t buf[4];
uint8_t n = en_encode_status(buf, &orig);
assert(n == 4);
assert(buf[0] == 0x04);   /* type byte */
assert(buf[1] == 0x05);   /* flags */
assert(buf[2] == 0x03);   /* sig_left */
assert(buf[3] == 0x01);   /* sig_right */

en_status_t dec;
assert(en_decode_status(buf, 4, &dec));
assert(dec.flags == 0x05 && dec.sig_left == 3 && dec.sig_right == 1);

/* Wrong type: must reject */
buf[0] = 0x99;
assert(!en_decode_status(buf, 4, &dec));

/* Short buffer: must reject */
buf[0] = 0x04;
assert(!en_decode_status(buf, 3, &dec));   /* 3 < 4 = reject */
```

### 10.2 `rf_signal_bars()` derivation

```c
/* Link down: always 0 regardless of other args */
assert(rf_signal_bars(false, 0,    0) == 0);
assert(rf_signal_bars(false, 5000, 0) == 0);

/* Age timeout: always 0 */
assert(rf_signal_bars(true, 1500, 0) == 0);
assert(rf_signal_bars(true, 2000, 5) == 0);

/* Excellent: age < 200 ms, link_q == 0 */
assert(rf_signal_bars(true, 100, 0) == 4);

/* Good: age < 200 ms, link_q 1..2 → min(4,3) = 3 */
assert(rf_signal_bars(true, 150, 1) == 3);
assert(rf_signal_bars(true, 150, 2) == 3);

/* Age degrades: age 200..399 ms, link_q 0 → min(3,4) = 3 */
assert(rf_signal_bars(true, 300, 0) == 3);

/* Both degrade: age 400..699, link_q 3..5 → min(2,2) = 2 */
assert(rf_signal_bars(true, 500, 4) == 2);

/* Heavy retries: link_q > 10 → retry_score=0 → 0 */
assert(rf_signal_bars(true, 100, 15) == 0);

/* Boundary: age exactly 1499 ms, link_q 0 → age_score=1, min(1,4)=1 */
assert(rf_signal_bars(true, 1499, 0) == 1);
assert(rf_signal_bars(true, 1500, 0) == 0);   /* boundary: >= 1500 = 0 */
```

### 10.3 `on_status()` changed-predicate

The "changed" check in `on_status()` is a pure comparison of five fields. Test
with a mock `g_half_state`:

```c
/* Identical payload: changed == false */
/* Different sig_left: changed == true */
/* Different flags bit 0: changed == true */
/* last_status_ms always updated regardless of changed */
```

### 10.4 `build_link_label()` formatting

```c
char buf[16];
build_link_label(buf, 'L', true,  true,  4); assert(strcmp(buf, "L * [||||]") == 0);
build_link_label(buf, 'R', true,  true,  0); assert(strcmp(buf, "R * [....]") == 0);
build_link_label(buf, 'L', true,  false, 0); assert(strcmp(buf, "L - [....]") == 0);
build_link_label(buf, 'R', false, false, 0); assert(strcmp(buf, "R ? [....]") == 0);
build_link_label(buf, 'L', true,  true,  5); assert(strcmp(buf, "L * [||||]") == 0);  /* clamp */
```

---

## 11. Out of Scope

| Feature | Status |
|---|---|
| Battery level display | Out of scope — ADC not implemented on half. Row space reserved on screen for future addition. |
| WPM (words per minute) | Out of scope — no key stats on the dongle yet. |
| Partial e-ink refresh | Out of scope — full refresh only (1.5 s); acceptable at 5 s cadence. |
| Per-half battery push (EN_INFO_BATTERY) | Already stubbed (dongle logs it); not wired to e-ink in this spec. |
| OTA / config push over ESP-NOW | Reserved (0x10+ type range); out of scope for this spec. |
| BLE connection dot on e-ink | The `flags.bit1=bt_connected` field exists in EN_INFO_STATE but is not displayed in this layout. Reserved for a future layout iteration. |

---

## 12. Self-Review

**Struct sizes / wire byte count verified:**
- `en_status_t`: 3 bytes packed. Wire: 1 (type) + 3 = 4 bytes. Well within 250-byte ESP-NOW limit.
- `half_state_t`: adds 5 fields (`bool×3` + `uint8_t×2` + `uint32_t`). Total addition: ~9 bytes. No ABI break (NVS never stores this struct).

**Type ID consistency:**
- `EN_INFO_STATUS = 0x04` is the next free value after 0x01/0x02/0x03. No conflict with reserved OTA range (0x10+).

**Font/glyph caveat is explicit and actionable:**
- Section 8.2 flags the Montserrat Unicode coverage issue, specifies the ASCII fallback as the default, and gives the implementer a concrete verification step.

**Dongle-link timeout wraparound:**
- Section 6.2 notes the `uint32_t` overflow at ~49 days and accepts it for a keyboard device. Flagged, not silently ignored.

**`changed == false` path in `on_status()` still updates `last_status_ms`:**
- Intentional: the timeout must reset even if the status is unchanged, otherwise a stable-good system would degrade after 15 s. Spec §5.2 and §6.2 both note this.

**`last_status_ms` initial value = 0:**
- At boot, `now_ms - 0` starts at ~0 ms, so `dongle_alive = true` for the first 15 s. Correct boot behaviour (15 s grace window). Spec §6.2 documents this explicitly.

**Existing `label_ver` position change:**
- Moved from `(60, 100)` to `(60, 185)` to make room for the new rows. This is a cosmetic change to the existing static screen init, not a new architectural dependency.

**Notify bit assignment:**
- Bit 0 = layer/state (existing). Bit 1 = status (new). No collision. `xTaskNotifyWait` clears all bits on exit (mask `0xFFFFFFFF`), so a combined bit0+bit1 notify processes both updates in one wakeup.

**`usb_active` duplication in `half_state_t.flags` vs `.usb_active`:**
- Flagged in §7. The e-ink task uses `.usb_active` exclusively. `flags.bit2` is written by `on_state()` (existing) and is not changed by this spec.

**`s_showing_degraded` guard in eink_lvgl_task:**
- Required to prevent continuous refreshes when the dongle is persistently absent. Mentioned in §6.3 as an implementation requirement.

**`rf_link_status_t` backward compatibility:**
- New fields `link_q_left`, `link_q_right` are appended. Any existing caller of `rf_rx_get_status()` continues to compile. Fields default to 0 (BSS) = "no retries" = best retry score; conservative but acceptable before the first heartbeat.
