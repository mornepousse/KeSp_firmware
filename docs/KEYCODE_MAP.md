# KaSe Keycode Map

All keycodes are 16-bit unsigned integers stored in `keymaps[layer][row][col]`.

## Encoding Summary

| Range | Type | Encoding | Example |
|-------|------|----------|---------|
| `0x0000-0x00FF` | HID standard | Direct HID usage code | `0x04` = A |
| `0x0100-0x0A00` | Momentary Layer (MO) | `0x0100 + layer * 0x100` | `MO_L2` = `0x0300` |
| `0x0B00-0x1400` | Toggle Layer (TO) | `0x0B00 + layer * 0x100` | `TO_L1` = `0x0C00` |
| `0x1500-0x2800` | Macro | `0x1500 + index * 0x100` | `MACRO_3` = `0x1700` |
| `0x2E00` | BT Switch | USB ↔ BLE toggle | — |
| `0x2F00` | BT Toggle | Enable/disable BLE | — |
| `0x3000-0x30FF` | One-Shot Modifier (OSM) | `0x3000 \| mod_mask` | `OSM(Shift)` = `0x3002` |
| `0x3100-0x310F` | One-Shot Layer (OSL) | `0x3100 \| layer` | `OSL(2)` = `0x3102` |
| `0x3200` | Caps Word | Toggle caps-word mode | — |
| `0x3300` | Repeat Key | Repeat last keypress | — |
| `0x4000-0x4FFF` | Layer-Tap (LT) | `0x4000 \| (layer << 8) \| keycode` | `LT(2, Space)` = `0x422C` |
| `0x5000-0x5FFF` | Mod-Tap (MT) | `0x5000 \| (mod << 8) \| keycode` | `MT(Shift, A)` = `0x5204` |
| `0x6000-0x6FFF` | Tap Dance (TD) | `0x6000 \| (index << 8)` | `TD(0)` = `0x6000` |

## Modifier Mask (for OSM and MT)

| Bit | Modifier |
|-----|----------|
| 0 | Left Ctrl |
| 1 | Left Shift |
| 2 | Left Alt |
| 3 | Left GUI |
| 4 | Right Ctrl |
| 5 | Right Shift |
| 6 | Right Alt |
| 7 | Right GUI |

## Tap/Hold Behavior

For **Layer-Tap (LT)** and **Mod-Tap (MT)** keys:
- **Tap** (press + release within 200ms, no other key pressed): sends the tap keycode
- **Hold** (held longer than 200ms OR another key is pressed while held): activates modifier/layer
- Configurable via `TAP_HOLD_TIMEOUT_MS` (default: 200)

## One-Shot Behavior

- **OSM**: Press and release modifier → next keypress includes that modifier → auto-deactivates
- **OSL**: Press and release layer key → next keypress uses that layer → returns to base layer

## Caps Word

When active, all letter keys are shifted. Deactivates on space, enter, or any non-alphanumeric key.

## Repeat Key

Sends the same HID keycode as the last non-modifier key that was pressed.

## Tap Dance

Each dance index (0-15) has a configurable action table:
- 1 tap → action A
- 2 taps → action B
- 3 taps → action C
- Hold → action D

Dance timeout: 200ms between taps.

## CDC Commands for Advanced Features

### `FEATURES?` — Query supported features
Response: `MT,LT,OSM,OSL,CAPS_WORD,REPEAT,TAP_DANCE`

### `TDSET <index>;<a1>,<a2>,<a3>,<a4>` — Configure a tap dance slot
- `index`: 0-15
- `a1-a4`: HID keycodes (hex) for 1-tap, 2-tap, 3-tap, hold
- Example: `TDSET 0;04,05,06,29` (1-tap=A, 2-tap=B, 3-tap=C, hold=ESC)
- Response: `TDSET 0:OK`

### `TD?` — List configured tap dance slots
Response: one line per configured slot, then `OK`
```
TD0: 04,05,06,29
OK
```
