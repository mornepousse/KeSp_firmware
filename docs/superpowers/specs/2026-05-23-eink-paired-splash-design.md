# E-ink "Paired" splash — design

**Date:** 2026-05-23
**Status:** approved
**Scope:** small (single feature, ~3 files)

## Problem

Pairing a half is currently done blind: you hold BOOT (GPIO0) for 3 s, the half
sends pairing REQs, gets an ACK, saves NVS and reboots — but there is **no visual
confirmation** on the half itself that pairing succeeded. During bring-up this was
painful (we paired "à l'aveugle"). The right half has an SSD1681 e-ink that is
otherwise idle during pairing; use it to confirm.

## Goal

After a half successfully pairs (receives a valid ACK), show a confirmation splash
on its e-ink for ~1.5 s before the reboot, then fall through to the normal status
dashboard on reboot.

## Non-goals (YAGNI)

- No splash on pairing **timeout** — the dashboard already shows the dongle link as
  down (`?`/`-`), which signals "not paired".
- No splash on the half-left (it has no e-ink panel). Best-effort by construction.
- No new persisted state, no CDC command, no animation.

## Design

### 1. New API — `eink_lvgl.{c,h}`

```c
/* Show a one-shot "PAIRED" confirmation splash on the e-ink, then it stays until
 * reboot. set_id is the assigned set identifier; slot is 0x01 (left) / 0x02 (right).
 * No-op if the eink task was never started (no panel on this half). Best-effort. */
void eink_lvgl_show_paired(uint16_t set_id, uint8_t slot);
```

Implementation: stores `set_id`/`slot` into file-static variables, then
`if (s_eink_task_handle) xTaskNotify(s_eink_task_handle, 0x04, eSetBits);`.
The NULL guard makes it a no-op on the half-left (where `eink_init()` failed and
the task was never created).

### 2. Task handling — new notify bit `0x04`

`eink_lvgl_task` already dispatches on `notify_val`:
- `0x01` = layer/state event
- `0x02` = status update
- **`0x04` = pairing splash (new)**

On `0x04`, build a dedicated full-screen splash on a **fresh screen**
(`lv_obj_create(NULL)` + `lv_scr_load`) so the dashboard's static label pointers
(`s_label_layer`, …) are left untouched — we reboot right after, so the old screen
is never restored. White background, black text. Layout (200×200, centered):

```
   y= 30   PAIRED        Montserrat 28
   y= 78   <OK glyph>    Montserrat 28 (LV_SYMBOL_OK — bundled, safe)
   y=128   set 0xXXXX    Montserrat 14
   y=152   side L | side R   Montserrat 14
```

The next `lv_timer_handler()` iteration (top of the loop) renders the new screen,
`flush_cb` assembles `s_fb` and `eink_push` writes the panel (~1.5 s).

### 3. Trigger — `half_pairing_task` in `half_scan_task.c`

In the `acked` branch, right after `rf_pairing_save_half(...)`:

```c
#if CONFIG_KASE_HAS_EINK
    eink_lvgl_show_paired(ack.set_id, ack.slot);
#endif
    vTaskDelay(pdMS_TO_TICKS(3000));   /* was 2000 — let the splash render (~1.5s) + stay visible */
    esp_restart();
```

Add `#include "eink_lvgl.h"` under the existing `#if CONFIG_KASE_HAS_EINK` block.

## Concurrency

The SPI bus is shared (NRF24 PTX + e-ink). At the splash point the pairing TX is
already finished (ACK received), so the only SPI user is `eink_push` — the same
concurrency profile as the live dashboard, which is hardware-validated. The
`half_pairing_task`'s `vTaskDelay(3000)` yields, letting the prio-3 eink task render.

## Files

- `main/periph/eink/eink_lvgl.h` — declare `eink_lvgl_show_paired`.
- `main/periph/eink/eink_lvgl.c` — statics, `eink_lvgl_show_paired`, bit-`0x04` handler.
- `main/comm/rf/half_scan_task.c` — include + call + delay bump.

## Test / validation

- Build `kase_half_right` (has e-ink) — must compile.
- Build `kase_half_left` (no e-ink panel, but `CONFIG_KASE_HAS_EINK` may be set) —
  must compile; runtime no-op via NULL handle.
- Hardware: hold BOOT on the right half → splash "PAIRED ✓ / set 0xXXXX / side R"
  appears, then reboots to the dashboard. (Left half: no splash, still pairs.)
