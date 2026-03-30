# KaSe Keycode Map

All keycodes are 16-bit unsigned integers stored in `keymaps[layer][row][col]`.
All hex values in CDC commands use **hex format** (e.g. `29` = 0x29 = ESC).

## Encoding Summary

| Range | Type | Encoding | Example |
|-------|------|----------|---------|
| `0x0000-0x00FF` | HID standard | Direct HID usage code | `0x04` = A |
| `0x0100-0x0A00` | Momentary Layer (MO) | `0x0100 + layer * 0x100` | `MO_L2` = `0x0300` |
| `0x0B00-0x1400` | Toggle Layer (TO) | `0x0B00 + layer * 0x100` | `TO_L1` = `0x0C00` |
| `0x1500-0x2800` | Macro | `0x1500 + index * 0x100` | `MACRO_3` = `0x1700` |
| `0x2900` | BT Next | Switch to next BT slot | `K_BT_NEXT` |
| `0x2A00` | BT Previous | Switch to previous BT slot | `K_BT_PREV` |
| `0x2B00` | BT Pair | Enter pairing mode | `K_BT_PAIR` |
| `0x2C00` | BT Disconnect | Disconnect current device | `K_BT_DISCONNECT` |
| `0x2E00` | BT Switch | USB / BLE toggle | `BT_SWITCH_DEVICE` |
| `0x2F00` | BT Toggle | Enable/disable BLE | `BT_TOGGLE` |
| `0x3000-0x30FF` | One-Shot Modifier (OSM) | `0x3000 \| mod_mask` | `K_OSM(Shift)` = `0x3002` |
| `0x3100-0x310F` | One-Shot Layer (OSL) | `0x3100 \| layer` | `K_OSL(2)` = `0x3102` |
| `0x3200` | Caps Word | Toggle caps-word mode | |
| `0x3300` | Repeat Key | Repeat last keypress | |
| `0x3400` | Leader Key | Start leader sequence | `K_LEADER` |
| `0x3500-0x3800` | Tama Actions | Feed/Play/Sleep/Medicine | `K_TAMA_*` |
| `0x3900` | Grave Escape | Tap=ESC, Shift+tap=\` | `K_GESC` |
| `0x3A00` | Layer Lock | Lock/unlock current MO layer | `K_LAYER_LOCK` |
| `0x3C00` | Auto Shift Toggle | Toggle auto-shift on/off | `K_AUTO_SHIFT_TOGGLE` |
| `0x3D00-0x3DFF` | Key Override | Trigger key override slot | `K_OVERRIDE(n)` |
| `0x4000-0x4FFF` | Layer-Tap (LT) | `0x4000 \| (layer << 8) \| keycode` | `K_LT(2, Space)` = `0x422C` |
| `0x5000-0x5FFF` | Mod-Tap (MT) | `0x5000 \| (mod << 8) \| keycode` | `K_MT(Shift, A)` = `0x5204` |
| `0x6000-0x6FFF` | Tap Dance (TD) | `0x6000 \| (index << 8)` | `K_TD(0)` = `0x6000` |

## Modifier Mask (for OSM and MT)

| Bit | Value | Modifier |
|-----|-------|----------|
| 0 | 0x01 | Left Ctrl |
| 1 | 0x02 | Left Shift |
| 2 | 0x04 | Left Alt |
| 3 | 0x08 | Left GUI |
| 4 | 0x10 | Right Ctrl |
| 5 | 0x20 | Right Shift |
| 6 | 0x40 | Right Alt |
| 7 | 0x80 | Right GUI |

## Feature Behaviors

### Tap/Hold (LT, MT, OSM)

- **Tap** (press + release < 200ms, no interrupt): sends the tap keycode
- **Hold** (> 200ms OR another key pressed while held): activates modifier/layer
- OSM tap: arms one-shot modifier for next key. OSM hold: regular modifier.

### One-Shot Layer (OSL)

Press and release: next keypress uses that layer, then returns to base.

### Caps Word

Toggle on/off. While active, letters are shifted. Deactivates on space/punctuation.

### Repeat Key

Sends the same HID keycode as the last non-modifier key pressed.

### Tap Dance

Up to 4 actions per slot: 1-tap, 2-taps, 3-taps, hold. 200ms between taps.

### Combos

Two keys pressed simultaneously trigger a different keycode. 50ms window.

### Leader Key

Press Leader, then a sequence of keys (up to 4). Matches against configured
sequences and emits result keycode + modifier. 1000ms timeout between keys.

## CDC Commands

### Query

| Command | Response | Description |
|---------|----------|-------------|
| `FEATURES?` | `MT,LT,OSM,OSL,...` | List supported features |
| `VERSION?` | `KaSe V1 v3.4` | Firmware version |
| `TD?` | `TD0: 04,05,06,29` | List tap dance configs |
| `COMBO?` | `COMBO0: r3c3+r3c4=29` | List combo configs |
| `LEADER?` | `LEADER0: 04,->29+00` | List leader sequences |
| `MACROS?` | Binary response | List macros |

### Configure

| Command | Format | Example |
|---------|--------|---------|
| `SETKEY` | `layer,v2row,v2col,hex_keycode` | `SETKEY 0,4,0,5229` |
| `TDSET` | `index;a1,a2,a3,a4` (hex) | `TDSET 0;04,05,06,29` |
| `COMBOSET` | `index;r1,c1,r2,c2,result` (V1 coords) | `COMBOSET 0;3,3,3,4,29` |
| `LEADERSET` | `index;seq_hex;result,mod` | `LEADERSET 0;04;29,00` |
| `MACROADD` | `slot;name;key1,key2,...` (simultaneous) | `MACROADD 0;Copy;E0,06` |
| `MACROSEQ` | `slot;name;key:mod,...` (sequence) | `MACROSEQ 0;CopyPaste;06:01,FF:0A,19:01` |
| `MACRODEL` | `slot` | `MACRODEL 0` |

### MACROSEQ Step Format

Each step is `keycode:modifier` in hex:
- `06:01` = C key with Ctrl modifier
- `FF:0A` = delay 10 * 0x0A = 100ms
- `19:01` = V key with Ctrl modifier

Example: `MACROSEQ 0;CopyPaste;06:01,FF:0A,19:01` = Ctrl+C, 100ms, Ctrl+V
