# V2D RF light-sleep + wake — design

**Date:** 2026-06-28
**Status:** approved (brainstorm — 60s timeout, light-sleep)
**Scope:** wireless-relay V2D (`CONFIG_KASE_KBD_WIRELESS`)

## Goal

Save battery: when the V2D runs on **RF (USB unplugged)** and is idle for **60 s**,
enter **light-sleep**; **wake on any keypress** (matrix GPIO), restore, and emit the
waking keystroke. Never sleep while on USB (powered).

## Trigger
- In `vTaskKeyboard` (10 ms loop), check each pass:
  `kbd_active_route() == KBD_OUT_RF && (now - get_last_activity_time_ms()) >= 60000`
  → call `v2d_sleep_enter()`. Activity timestamp already maintained by
  matrix_scan.c (`last_activity_time_ms`, bumped on every key event).
- On USB (route USB/NONE) the timer is irrelevant — never sleeps.
- `v2d_sleep_enter()` runs in the keyboard task context (the scanner), so it can
  tear down the scan driver and block in `esp_light_sleep_start()`.

## Building blocks (already exist)
- `get_last_activity_time_ms()` / `last_activity_time_ms` (matrix_scan).
- `rtc_matrix_deinit()` (keyboard_button_delete) + `matrix_setup()` (recreate).
- `rf_driver_power_down()` / `rf_driver_power_up()` (NRF standby ~900nA).
- `status_display_sleep()` + display backend `sleep`/`wake` vtable (OLED off/on).
- ESP-NOW/WiFi teardown/restart pattern from `half_sleep.c`
  (`esp_now_deinit` + `esp_wifi_stop` … `esp_wifi_start` + `espnow_link_restart_espnow`).

## New code
### `main/comm/rf/kbd_relay_tx.c` — sleep helpers
- `void kbd_relay_sleep_prepare(void)`: stop the 10 ms refresh esp_timer (so it
  won't touch the NRF mid-sleep), take `s_tx_mutex`, `rf_driver_power_down(&s_radio)`.
- `void kbd_relay_wake_restore(void)`: `rf_driver_power_up(&s_radio)`, give the
  mutex, restart the refresh timer.
  (Hold the relay mutex across sleep so nothing sends while powered down.)

### `main/input/matrix_scan.c` — GPIO key-wake (mirror half_scan_arm_key_wake)
- `void matrix_arm_key_wake(void)`: V2D drives **COLS** and reads **ROWS** (per
  matrix_setup: outputs=COLS, inputs=ROWS). So: each COL → output HIGH +
  `gpio_sleep_sel_dis`; each ROW → input + `GPIO_PULLDOWN_ONLY` +
  `gpio_wakeup_enable(row, GPIO_INTR_HIGH_LEVEL)` + `gpio_sleep_sel_dis`; then
  `esp_sleep_enable_gpio_wakeup()`. A keypress ties a HIGH col to a row → row HIGH → wake.
- `void matrix_disarm_key_wake(void)`: clear the wake enables (the subsequent
  `matrix_setup()` re-takes the pins).

### `main/sys/v2d_sleep.{c,h}` (new, KBD_WIRELESS-gated) — orchestrator
```
void v2d_sleep_enter(void) {
    status_display_sleep();          // OLED off + STOP the LVGL tick (see pitfall)
    esp_now_deinit(); esp_wifi_stop();   // WiFi off — BEFORE any long hold, heap uncontested
    rtc_matrix_deinit();             // stop the scan driver
    kbd_relay_sleep_prepare();       // refresh timer off, NRF power_down (holds relay mutex)
    matrix_arm_key_wake();
    esp_light_sleep_start();         // returns on a matrix keypress
    matrix_disarm_key_wake();
    kbd_relay_wake_restore();        // NRF power_up, refresh timer on
    matrix_setup();                  // recreate the scan driver
    esp_wifi_start(); espnow_link_restart_espnow();
    status_display_wake();           // OLED on + LVGL tick on
    last_activity_time_ms = now;     // reset idle timer so we don't immediately re-sleep
}
```

## Pitfall — esp_wifi_stop() vs LVGL heap spinlock (CRITICAL)
`half_sleep.c` documents a deadlock: `esp_wifi_stop()` drains a WIFI_EVENT on the
event task which calls `malloc` (heap spinlock); if an LVGL task is inside
`lv_timer_handler` holding that spinlock, it deadlocks. The V2D OLED runs LVGL via
esp_lvgl_port → SAME risk. Mitigation: **`status_display_sleep()` must stop the
LVGL tick/timer BEFORE `esp_wifi_stop()`** (mirror eink_lvgl_suspend). Verify the
OLED backend `sleep` actually pauses the LVGL port timer; if it only blanks the
panel, add an explicit LVGL-tick stop in the sleep path. This is the #1 thing to
get right and to watch for on HW (hang on first sleep = this).

## Wake correctness
- The waking keypress: `matrix_setup()` recreates keyboard_button after wake; the
  driver re-reads the matrix and reports the still-held key → it is relayed
  normally (no lost keystroke). Same approach as half_scan_restart_after_wake.
- WiFi restart (~100 ms) + NRF re-init happen after wake; the first relayed report
  may be delayed ~100 ms but not lost (idempotent refresh covers it).

## Testing
- Host: the only pure-ish logic is the trigger predicate
  `v2d_should_sleep(route_is_rf, idle_ms, threshold)` → host-test it. The rest is
  RTOS/peripheral glue (HW only).
- HW (with Mae): on RF, idle 60 s → sleeps (current drops); any key wakes + the
  key registers; OLED restores; relay resumes; on USB it never sleeps. Watch for a
  hang on the first sleep (→ the LVGL/WiFi pitfall above).

## Out of scope
- Deep-sleep / multi-stage (chosen: light-sleep only).
- USB-mode sleep (powered, not needed).
