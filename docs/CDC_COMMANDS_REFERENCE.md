# KaSe CDC Command Reference (Legacy ASCII)

> **New: Binary protocol available.** See [`CDC_BINARY_PROTOCOL.md`](CDC_BINARY_PROTOCOL.md) for the KS/KR binary frame format with CRC-8 protection. The binary protocol is faster, more robust, and supports all commands listed here. The ASCII protocol below remains fully functional for backward compatibility.

Complete list of all CDC serial commands. Connect to the keyboard's CDC ACM port (ttyACM0) at any baud rate.
Commands are text lines terminated by `\r\n`. Responses end with `\r\n`.

## Connection

```python
import serial
ser = serial.Serial('/dev/ttyACM0', timeout=2)
ser.write(b'VERSION?\r\n')
print(ser.readline().decode())
```

---

## System

| Command | Response | Description |
|---------|----------|-------------|
| `VERSION?` | `KaSe V1 v3.4` | Firmware version + board name |
| `FEATURES?` | `MT,LT,OSM,...` | List of supported advanced features |
| `DFU` | `Rebooting to DFU...` | Reboot into USB DFU mode |

---

## Keymap

| Command | Format | Description |
|---------|--------|-------------|
| `KEYMAP?` | — | Get current layer keymap (binary) |
| `KEYMAP<n>` | `KEYMAP0`, `KEYMAP1` | Get keymap for layer n (binary) |
| `SETKEY` | `layer,v2row,v2col,hex_keycode` | Set one key |
| `SETLAYER` | `<layer>:val1,val2,...` | Set entire layer |

**SETKEY example:** `SETKEY 0,4,0,5229` → Layer 0, V2 row 4 col 0 = MT(Shift, ESC)

**Note:** SETKEY uses V2 coordinates. V1 boards translate internally.

---

## Layout

| Command | Description |
|---------|-------------|
| `LAYOUT?` | Physical key position JSON |
| `LAYOUTS?` | List all layer names |
| `LAYOUTNAME<n>:<name>` | Rename layer n |
| `L?` | Current layer index (binary) |
| `L<n>` / `LN<n>` | Get layer n name (binary) |

---

## Macros

| Command | Format | Description |
|---------|--------|-------------|
| `MACROS?` | — | List all macros (binary) |
| `MACROADD` | `slot;name;key1,key2,...` | Simple macro (simultaneous keys) |
| `MACROSEQ` | `slot;name;key:mod,...` | Sequence macro with delays |
| `MACRODEL` | `slot` | Delete a macro |

### MACROSEQ format

Each step is `keycode:modifier` in hex. Special: `FF:nn` = delay (nn × 10ms).

```
MACROSEQ 0;CopyPaste;06:01,FF:0A,19:01
```
= Ctrl+C, 100ms delay, Ctrl+V

### MACROADD format (legacy)

```
MACROADD 0;Copy;E0,06
```
= Left Ctrl + C (simultaneous)

---

## Statistics

| Command | Response | Description |
|---------|----------|-------------|
| `KEYSTATS` | Binary packet | Per-key press counts (for heatmap) |
| `KEYSTATS?` | Text | Human-readable key stats |
| `KEYSTATS_RESET` | `KEYSTATS_RESET:OK` | Reset all key stats |
| `BIGRAMS` | Binary packet | Top 256 bigrams |
| `BIGRAMS?` | Text | Human-readable top 20 bigrams |
| `BIGRAMS_RESET` | `BIGRAMS_RESET:OK` | Reset bigram stats |

Binary equivalents: `KEYSTATS_BIN` (0x40), `KEYSTATS_TEXT` (0x41), `KEYSTATS_RESET` (0x42), `BIGRAMS_BIN` (0x43), `BIGRAMS_TEXT` (0x44), `BIGRAMS_RESET` (0x45). See `CDC_BINARY_PROTOCOL.md`.

---

## Tap Dance

| Command | Format | Description |
|---------|--------|-------------|
| `TD?` | — | List configured dances |
| `TDSET` | `index;a1,a2,a3,a4` | Configure dance (all hex) |
| `TDDEL` | `index` | Delete a tap dance |

**TDSET example:** `TDSET 0;04,05,06,29` → 1-tap=A, 2-tap=B, 3-tap=C, hold=ESC

---

## Combos

| Command | Format | Description |
|---------|--------|-------------|
| `COMBO?` | — | List configured combos |
| `COMBOSET` | `index;r1,c1,r2,c2,result_hex` | Configure combo |
| `COMBODEL` | `index` | Delete a combo |

**COMBOSET example:** `COMBOSET 0;3,3,3,4,29` → J+K = ESC

**Note:** Positions use V1 internal coordinates.

---

## Leader Key

| Command | Format | Description |
|---------|--------|-------------|
| `LEADER?` | — | List leader sequences |
| `LEADERSET` | `index;seq_hex;result_hex,mod_hex` | Configure sequence |
| `LEADERDEL` | `index` | Delete a leader sequence |

**LEADERSET example:** `LEADERSET 0;09,16;16,01` → Leader + F + S = Ctrl+S

Sequence keys and result are HID keycodes in hex. Mod is modifier mask.

---

## Bluetooth Multi-Device

| Command | Response | Description |
|---------|----------|-------------|
| `BT?` | Slot list + status | Show all 3 BT slots with addresses |
| `BT SWITCH <n>` | `BT:SWITCHING` | Switch to slot n (0-2) |
| `BT PAIR` | `BT:PAIRING` | Enter pairing mode on active slot |
| `BT DISCONNECT` | `BT:DISCONNECTED` | Disconnect current device |
| `BT NEXT` | `BT:NEXT` | Switch to next slot |
| `BT PREV` | `BT:PREV` | Switch to previous slot |

**BT? response example:**
```
BT: slot=0 init=1 conn=1 name=iPhone
  SLOT0: AA:BB:CC:DD:EE:FF iPhone *
  SLOT1: 11:22:33:44:55:66 MacBook
  SLOT2: empty
OK
```

### Bluetooth Keycodes

| Keycode | Value | Description |
|---------|-------|-------------|
| `BT_SWITCH_DEVICE` | `0x2E00` | Toggle USB ↔ BLE |
| `BT_TOGGLE` | `0x2F00` | Enable/disable BLE |
| `K_BT_NEXT` | `0x2900` | Switch to next BT slot |
| `K_BT_PREV` | `0x2A00` | Switch to previous BT slot |
| `K_BT_PAIR` | `0x2B00` | Enter pairing mode |
| `K_BT_DISCONNECT` | `0x2C00` | Disconnect current device |

**Map via SETKEY:** `SETKEY 0,4,0,2900` (ESC = BT Next)

---

## Auto Shift / Key Override / WPM / Tri-Layer

| Command | Response | Description |
|---------|----------|-------------|
| `AUTOSHIFT` | `AUTOSHIFT:ON/OFF` | Toggle auto-shift |
| `KOSET` | `KOSET n:OK` | Configure key override: `index;trigger_key,trigger_mod,result_key,result_mod` (hex) |
| `KODEL` | `KO n deleted` | Delete a key override |
| `KO?` | List overrides | Show all configured key overrides |
| `WPM?` | `WPM: 42` | Current words-per-minute |
| `TRILAYER` | `TRILAYER: L1+L2=L3` | Configure tri-layer: `layer1,layer2,result_layer` |

**KOSET example:** `KOSET 0;2A,02,4C,00` → Shift+Backspace = Delete

**TRILAYER example:** `TRILAYER 1,2,3` → Layer1+Layer2 active = activate Layer3

---

## Tamagotchi

| Command | Response | Description |
|---------|----------|-------------|
| `TAMA?` | `TAMA: Lv1 hunger=800 ...` | Query tama stats |
| `TAMA ENABLE` | `TAMA:ENABLED` | Enable tama |
| `TAMA DISABLE` | `TAMA:DISABLED` | Disable tama |
| `TAMA FEED` | `TAMA:FED` | Feed the pet (+300 hunger) |
| `TAMA PLAY` | `TAMA:PLAYED` | Play with pet (+200 happiness) |
| `TAMA SLEEP` | `TAMA:SLEPT` | Put pet to sleep (+400 energy) |
| `TAMA MEDICINE` | `TAMA:HEALED` | Give medicine (+100 all stats) |
| `TAMA SAVE` | `TAMA:SAVED` | Force save stats to NVS |

---

## OTA Firmware Update

| Command | Response | Description |
|---------|----------|-------------|
| `OTA <size>` | `OTA_READY 4096` | Start OTA (size in bytes) |

After `OTA_READY`, send raw binary in 4096-byte chunks. Wait for `OTA_OK` after each chunk. Ends with `OTA_DONE` and reboot.

See `CDC_KEYSTATS_PROTOCOL.md` for full OTA protocol flow.

---

## Keycode Reference

See `KEYCODE_MAP.md` for the complete keycode encoding specification:

| Range | Type | Example |
|-------|------|---------|
| `0x0000-0x00FF` | HID standard | `0x04`=A, `0x29`=ESC |
| `0x0100-0x0A00` | Momentary Layer | `MO_L2`=`0x0300` |
| `0x0B00-0x1400` | Toggle Layer | `TO_L1`=`0x0C00` |
| `0x1500-0x2800` | Macro | `MACRO_1`=`0x1500` |
| `0x2E00-0x2F00` | Bluetooth | BT_SWITCH, BT_TOGGLE |
| `0x3000-0x30FF` | One-Shot Modifier | `K_OSM(Shift)`=`0x3002` |
| `0x3100-0x310F` | One-Shot Layer | `K_OSL(2)`=`0x3102` |
| `0x3200` | Caps Word | Toggle caps word |
| `0x3300` | Repeat Key | Repeat last key |
| `0x3400` | Leader Key | Start leader sequence |
| `0x3500-0x3800` | Tama Actions | Feed/Play/Sleep/Medicine |
| `0x4000-0x4FFF` | Layer-Tap | `K_LT(2,Space)`=`0x422C` |
| `0x5000-0x5FFF` | Mod-Tap | `K_MT(Shift,A)`=`0x5204` |
| `0x6000-0x6FFF` | Tap Dance | `K_TD(0)`=`0x6000` |

### Modifier Mask

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

### Tap/Hold Behavior

Keys with dual function (LT, MT, OSM):
- **Tap** (< 200ms, no interrupt): sends tap keycode
- **Hold** (> 200ms OR another key pressed): activates modifier/layer
- Timeout configurable: `TAP_HOLD_TIMEOUT_MS` (default 200)
