# Fastfetch-style e-ink dashboard — design

**Date:** 2026-05-23
**Status:** approved
**Scope:** medium (e-ink screen rewrite + local stat gathering, ~3 files)
**Replaces:** the current card dashboard in eink_lvgl.c

## Goal

Replace the e-ink dashboard with a fastfetch/neofetch-style layout: a drawn
keyboard icon on the left + a dense monospace `key: value` info column on the
right, with nerdy local stats.

## Layout (200×200, monospace `unscii_8`, ~19 cols in the info column)

```
        kase@dongle
 ▛▀▀▜   -------------------
 ▌▪▪▐   lay : BASE
 ▌▪▪▐   L   : 248/255 --%
 ▙▄▄▟   R   : 250/255 --%
        usb : on
        set : 0x9044
        net : ch11 104/105
        mem : 207K  42C
        flash: 16M  5M used
        soc : ESP32-S3 x2
        fw  : v3.7.12
```
Left ~44 px: keyboard icon drawn with LVGL primitives (rounded body rect + key
squares + spacebar). Info column: x≈48, `unscii_8`, ~12 lines at 12 px pitch.

## Fields & sources

| Line | Content | Source | Dynamic? |
|------|---------|--------|----------|
| header | `kase@dongle` | static | no |
| `lay`  | layer name | dongle EN_INFO_LAYER (existing g_half_state.layer_name) | on layer change |
| `L`/`R`| `q255/255 bat%` | q255 from dongle EN_INFO_STATUS; bat = `--%` placeholder | q255 on status push |
| `usb`  | on/off/? | dongle EN_INFO_STATUS flags | on status push |
| `set`  | `0x%04X` | `rf_pairing_load_set_id_half()` | static |
| `net`  | `ch%u %u/%u` ESP-NOW + NRF L/R | `rf_derive_wifi_ch`, base=`80+2*(sid%20)` (L=base, R=base+1) | static |
| `mem`  | `%uK %dC` heap free + CPU temp | `esp_get_free_heap_size()`, `temperature_sensor` | quantized, on status push |
| `flash`| `%uM %uM used` | `esp_flash_get_size()`, max partition end (esp_partition iter) | static |
| `soc`  | `ESP32-S3 x%d` | `esp_chip_info()` | static |
| `fw`   | version | `esp_app_get_description()` | static |

Battery (L+R) shows `--%` until the ADC feature lands (slots reserved).

## Refresh strategy (avoid e-ink churn)

- Static fields drawn once at init; never repaint on their own.
- Dynamic fields (`lay`, `L`, `R`, `usb`) updated on the existing notify path.
- Volatile `mem` (heap/temp) **quantized**: heap to nearest 8 KB, temp to integer
  °C → text rarely changes → e-ink rarely repaints. Updated on status push (~5 s);
  the existing framebuffer de-dup in `eink_push` suppresses no-op refreshes.

## Implementation

1. **Font:** add `CONFIG_LV_FONT_UNSCII_8=y` to `sdkconfig.defaults` (shared) +
   `sdkconfig.defaults.half_right`. (Half builds only need it; harmless on dongle.)
2. **Temp sensor:** add `esp_driver_tsens` to `main/CMakeLists.txt` priv_requires;
   install + enable a `temperature_sensor` once in `eink_lvgl_init`.
3. **Stats helpers** (`eink_lvgl.c`, file-static): gather set_id/channels (local),
   chip info, flash size + used (max partition end), version; format helpers for
   heap (`207K`) and the link line.
4. **Icon:** `draw_keyboard_icon(lv_obj_t *parent)` — body `lv_obj` rounded rect +
   a 2×3 grid of small key `lv_obj`s + a spacebar rect, black on white.
5. **Screen rebuild:** rewrite the label creation block in `eink_lvgl_init` to the
   fastfetch layout (icon + static lines + dynamic line handles). Keep the
   `s_label_*` handles for dynamic lines; add `s_label_mem`.
6. **Update path:** in `eink_lvgl_task`, update `lay`/`L`/`R`/`usb`/`mem`; format
   `L`/`R` as `"%u/255 --%%"`; quantize mem.

## Files

- `sdkconfig.defaults`, `sdkconfig.defaults.half_right` — unscii_8 font.
- `main/CMakeLists.txt` — esp_driver_tsens.
- `main/periph/eink/eink_lvgl.c` — screen rewrite, icon, stats.

## YAGNI / notes

- No keystroke counter / uptime (not selected). No bitmap asset (drawn icon).
- Disk "used" = highest partition end (the half doesn't mount littlefs), not live FS usage.
- CPU temp is the half's own ESP32-S3 sensor (the e-ink is on the half).
