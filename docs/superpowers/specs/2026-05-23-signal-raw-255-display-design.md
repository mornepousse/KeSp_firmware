# Raw 0–255 signal value on e-ink — design

**Date:** 2026-05-23
**Status:** approved (format: decimal `/255`)
**Scope:** small (evolves the ARC_CNT metric display, ~5 files + tests)
**Supersedes the display half of:** 2026-05-23-finer-signal-gauge-arc-cnt-design.md
(the `link_q` retry-% metric stays; only the dongle-side mapping + display change.)

## Problem

The dashboard shows reception as 4-bar `[||||]` glyphs. The user wants the **raw
quality value** (out of 255), shown clearly like `235/255`, for a finer/nerdier read.

## Design

### 1. Continuous quality — `rf_rx_task.c` / `.h`

Replace `rf_signal_bars()` (0..4) with `rf_signal_q255()` (0..255, 255 = best):

```c
uint8_t rf_signal_q255(bool link_up, uint32_t hb_age_ms, uint8_t link_q)
{
    if (!link_up || hb_age_ms >= 1500u) return 0;
    uint32_t age_factor   = 255u * (1500u - hb_age_ms) / 1500u;  /* fresh→255, 1500ms→0 */
    uint8_t  lq           = (link_q > 100) ? 100 : link_q;       /* retry % clamp */
    uint32_t retry_factor = 255u * (100u - lq) / 100u;           /* 0%→255, 100%→0 */
    uint32_t q = (age_factor < retry_factor) ? age_factor : retry_factor;  /* both must be good */
    return (uint8_t)q;
}
```
A healthy link (age ~100 ms, 0 retries) reads ~238 → matches the "235/255" expectation.
Update the two callers in `status_push_cb` (`msg.sig_left/right`).

### 2. Wire — `espnow_msg.h`

`en_status_t.sig_left/sig_right` stay `u8` (no layout change) — semantics doc only:
`0..255 link quality, 0 = link down / no data` (was `0..4`). Half + dongle flashed
together; left half just stores the byte (no e-ink) so it is wire-compatible
without a reflash.

### 3. Display — `eink_lvgl.c`

`build_link_label()` takes the q255 value (drop the bars + the `*`/`-` dot):
- dongle not alive → `"L  --/255"`
- otherwise        → `"L  235/255"` (link down ⇒ value 0 ⇒ `"L  0/255"`)

`build_usb_label()` → `"USB on"` / `"USB off"` / `"USB ?"` (cleaner next to the number,
matches the approved mock). Update the callers in `eink_lvgl_task`.

### 4. Tests — rename `test_rf_signal_bars.c` → `test_rf_signal_q255.c`

Rewrite for the 0..255 range; update `test_main.c` (extern + call) and
`test/CMakeLists.txt` (source name). Boundary cases:
- link down / age≥1500 → 0
- age=0,  q=0   → 255
- age=100,q=0   → 238
- age=750,q=0   → 127   (half age)
- age=0,  q=50  → 127   (half retries)
- age=0,  q=100 → 0
- age=1200,q=0  → 51
- min rule: age=750 (127), q=50 (127) → 127; age=0, q=75 → 63

## Files

- `main/comm/rf/rf_rx_task.c` — `rf_signal_q255` + caller updates.
- `main/comm/rf/rf_rx_task.h` — declaration rename.
- `main/comm/espnow/espnow_msg.h` — sig field doc (0..255).
- `main/periph/eink/eink_lvgl.c` — `build_link_label` / `build_usb_label` format + callers.
- `test/test_rf_signal_q255.c` (renamed), `test/test_main.c`, `test/CMakeLists.txt`.

## Deployment

Reflash **dongle** (computes q255) + **right half** (displays it). Left half not
required (wire-compatible). The right half must also be **paired** (in the dongle's
peer list) to receive EN_INFO_STATUS at all.

## YAGNI

No hex display (decimal chosen). No per-half history/averaging beyond the existing
per-interval retry %.
