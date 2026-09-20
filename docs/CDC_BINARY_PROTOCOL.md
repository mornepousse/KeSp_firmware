# KaSe Binary CDC Protocol

Binary protocol for communication between KaSe_soft and the firmware. Coexists with the legacy ASCII protocol — the firmware auto-detects the mode from the first byte.

## Frame Format

### Request (Host → Keyboard)

```
Offset  Size  Field
  0       1    Magic 0x4B ('K')
  1       1    Magic 0x53 ('S')
  2       1    Command ID
  3       2    Payload length (u16 LE)
  5       N    Payload (0 to 4096 bytes)
  5+N     1    CRC-8 of payload
```

### Response (Keyboard → Host)

```
Offset  Size  Field
  0       1    Magic 0x4B ('K')
  1       1    Magic 0x52 ('R')
  2       1    Command ID (echo)
  3       1    Status code
  4       2    Payload length (u16 LE)
  6       N    Payload
  6+N     1    CRC-8 of payload
```

## CRC-8

Polynomial: **0x31**, init 0x00, MSB-first, **no input/output reflection**, no final XOR.
Computed over payload bytes only (not the header).

> ⚠️ This is **not** the catalogued CRC-8/MAXIM (which is reflected, poly 0x8C).
> It is a plain MSB-first CRC-8 with polynomial 0x31. Use the reference
> implementation below verbatim — do not pull a library "CRC-8/MAXIM".

Test vectors:
- Empty payload → `0x00`
- `[0x01]` → `0x31`
- `[0x4B, 0x53]` → `0xBE`

```c
uint8_t crc8(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}
```

```python
def crc8(data: bytes) -> int:
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc
```

## Status Codes

| Code | Name | Description |
|------|------|-------------|
| 0x00 | OK | Success |
| 0x01 | ERR_UNKNOWN | Unknown command ID |
| 0x02 | ERR_CRC | CRC mismatch |
| 0x03 | ERR_INVALID | Invalid payload format |
| 0x04 | ERR_RANGE | Parameter out of range |
| 0x05 | ERR_BUSY | Resource busy (OTA in progress) |
| 0x06 | ERR_OVERFLOW | Payload exceeds max size |
| 0x07 | ERR_STORAGE | NVS persistence failed (e.g. NVS full) — the in-RAM change applied but was not saved |

## Backward Compatibility

If the first byte received is not `0x4B`, the firmware falls back to legacy ASCII text mode. No text command starts with `KS`, so there is no ambiguity.

The old version of KaSe_soft continues to work for OTA and all text commands.

---

## Command Reference

### System (0x01–0x0F)

#### PING (0x04)
Tests the connection.
- Request: `KS 04 0000 00`
- Response: `KR 04 00 0000 00`

#### VERSION (0x01)
Returns the firmware version.
- Request: empty payload
- Response: payload = UTF-8 version string (e.g.: `v3.2-87-gd34c39f`)

#### FEATURES (0x02)
List of supported features.
- Request: empty payload
- Response: payload = CSV list (e.g.: `MT,LT,LM,OSM,OSL,CAPS_WORD,...`)

#### DFU (0x03)
Reboots into DFU mode. OK response sent before reboot.
- Request: empty payload
- Response: OK then reboot

---

### Keymap (0x10–0x1F)

#### LAYER_INDEX (0x14)
Returns the active layer.
- Request: empty payload
- Response: `[layer:u8]`

#### KEYMAP_CURRENT (0x12)
Keymap of the active layer.
- Request: empty payload
- Response: `[layer:u8][keycodes: ROWS*COLS * u16 LE]`

#### KEYMAP_GET (0x13)
Keymap of a specific layer.
- Request: `[layer:u8]`
- Response: `[layer:u8][keycodes: ROWS*COLS * u16 LE]`
- Error: `ERR_RANGE` if layer >= LAYERS

#### SETKEY (0x11)
Modifies a key. Saved to NVS immediately.
- Request: `[layer:u8][row:u8][col:u8][value:u16 LE]`
- Response: OK
- Note: native board coordinates (each variant has its own layout)

#### SETLAYER (0x10)
Replaces an entire layer. Saved immediately.
- Request: `[layer:u8][keycodes: ROWS*COLS * u16 LE]`
- Response: OK

#### LAYER_NAME (0x15)
Name of a layer.
- Request: `[layer:u8]`
- Response: `[layer:u8][name bytes]`

---

### Layout (0x20–0x2F)

#### LIST_LAYOUTS (0x21)
All layer names.
- Request: empty payload
- Response: `[count:u8][{idx:u8, name_len:u8, name[]}...]`

#### SET_LAYOUT_NAME (0x20)
Renames a layer. Saved immediately.
- Request: `[layer:u8][name bytes]`
- Response: OK

#### GET_LAYOUT_JSON (0x22)
JSON of the keyboard's physical layout.
- Request: empty payload
- Response: payload = raw JSON (may exceed 4KB, sent as a stream)

---

### Macros (0x30–0x3F)

#### LIST_MACROS (0x30)
Lists all configured macros.
- Request: empty payload
- Response:
```
[count:u8]
{
  [idx:u8]
  [keycode:u16 LE]       — macro keycode (MACRO_1 + idx*0x100)
  [name_len:u8][name[]]
  [keys_len:u8][keys[]]  — legacy simultaneous keys
  [step_count:u8]         — sequence steps
  [{kc:u8, mod:u8}...]   — step_count * 2 bytes
}...
```

#### MACRO_ADD (0x31)
Adds a legacy macro (simultaneous keys).
- Request: `[slot:u8][name_len:u8][name...][keys: 6 bytes]`
- Response: OK

#### MACRO_ADD_SEQ (0x32)
Adds a sequence macro.
- Request: `[slot:u8][name_len:u8][name...][step_count:u8][{kc:u8,mod:u8}...]`
- Response: OK
- Note: `kc=0xFF` + `mod=N` = delay of N*10ms

#### MACRO_DELETE (0x33)
Deletes a macro.
- Request: `[slot:u8]`
- Response: OK

---

### Statistics (0x40–0x4F)

#### KEYSTATS_BIN (0x40)
Key counters per position (structured binary format).
- Request: empty payload
- Response: `[rows:u8][cols:u8][counts: rows*cols * u32 LE]`
- Note: V2 coordinates

#### KEYSTATS_TEXT (0x41)
Key counters per position (human-readable text format).
- Request: empty payload
- Response: payload = multi-line UTF-8 text
```
Key Statistics - Total: 12345, Max: 678
R0:   123   456   789 ...
R1:    42    99   301 ...
...
```

#### KEYSTATS_RESET (0x42)
Resets the counters to zero.
- Request: empty payload
- Response: OK

#### BIGRAMS_BIN (0x43)
Top 256 bigrams sorted by frequency (structured binary format).
- Request: empty payload
- Response:
```
[module_id:u8]
[num_keys:u8]
[total:u32 LE]
[max:u16 LE]
[{prev:u8, curr:u8, count:u16 LE}...]  — top entries sorted desc
```

#### BIGRAMS_TEXT (0x44)
Top 20 bigrams (human-readable text format).
- Request: empty payload
- Response: payload = multi-line UTF-8 text
```
Bigram Statistics - Total: 5678, Max: 42
  R1C3 -> R0C5 : 42
  R0C2 -> R0C3 : 38
  ...
```

#### BIGRAMS_RESET (0x45)
Resets the bigrams to zero.
- Request: empty payload
- Response: OK

---

### Tap Dance (0x50–0x5F)

#### TD_LIST (0x51)
Lists the configured tap dances.
- Request: empty payload
- Response: `[count:u8][{idx:u8, a1:u8, a2:u8, a3:u8, a4:u8}...]`
- a1=1-tap, a2=2-taps, a3=3-taps, a4=hold (HID keycodes)

#### TD_SET (0x50)
Configures a tap dance.
- Request: `[index:u8][a1:u8][a2:u8][a3:u8][a4:u8]`
- Response: OK

#### TD_DELETE (0x52)
Deletes a tap dance.
- Request: `[index:u8]`
- Response: OK

---

### Combos (0x60–0x6F)

#### COMBO_LIST (0x61)
Lists the configured combos.
- Request: empty payload
- Response: `[count:u8][{idx:u8, row1:u8, col1:u8, row2:u8, col2:u8, result:u8}...]`

#### COMBO_SET (0x60)
Configures a combo.
- Request: `[index:u8][row1:u8][col1:u8][row2:u8][col2:u8][result:u8]`
- Response: OK
- Note: positions in internal V1 coordinates

#### COMBO_DELETE (0x62)
Deletes a combo.
- Request: `[index:u8]`
- Response: OK

---

### Leader Key (0x70–0x7F)

#### LEADER_LIST (0x71)
Lists the leader sequences.
- Request: empty payload
- Response: `[count:u8][{idx:u8, seq_len:u8, seq[], result:u8, result_mod:u8}...]`

#### LEADER_SET (0x70)
Configures a leader sequence.
- Request: `[index:u8][seq_len:u8][seq...][result:u8][result_mod:u8]`
- Response: OK
- The sequence is a series of HID keycodes (max 4), result_mod = modifier mask

#### LEADER_DELETE (0x72)
Deletes a leader sequence.
- Request: `[index:u8]`
- Response: OK

---

### Bluetooth (0x80–0x8F)

#### BT_QUERY (0x80)
Full Bluetooth state.
- Request: empty payload
- Response:
```
[active_slot:u8]
[initialized:u8]   — 0/1
[connected:u8]      — 0/1
[pairing:u8]        — 0/1
[{slot_idx:u8, valid:u8, addr[6], name_len:u8, name[]}...]  — BT_MAX_DEVICES (3) entries
```

#### BT_SWITCH (0x81)
Switches BT slot.
- Request: `[slot:u8]` (0-2)
- Response: OK (note: reconnection can take ~3s)

#### BT_PAIR (0x82)
Enables pairing mode (undirected advertising).
- Request: empty payload
- Response: OK

#### BT_DISCONNECT (0x83)
Disconnects the current device.
- Request: empty payload
- Response: OK

#### BT_NEXT (0x84) / BT_PREV (0x85)
Next/previous slot.
- Request: empty payload
- Response: OK

---

### Features (0x90–0x9F)

#### AUTOSHIFT_TOGGLE (0x90)
Toggles auto-shift on/off.
- Request: empty payload
- Response: `[enabled:u8]` (0 or 1, state after toggling)

#### KO_SET (0x91)
Configures a key override.
- Request: `[index:u8][trigger_key:u8][trigger_mod:u8][result_key:u8][result_mod:u8]`
- Response: OK

#### KO_LIST (0x92)
Lists the key overrides.
- Request: empty payload
- Response: `[count:u8][{idx:u8, trigger_key:u8, trigger_mod:u8, result_key:u8, result_mod:u8}...]`

#### KO_DELETE (0x93)
Deletes a key override.
- Request: `[index:u8]`
- Response: OK

#### WPM_QUERY (0x94)
Current words per minute.
- Request: empty payload
- Response: `[wpm:u16 LE]`

#### TRILAYER_SET (0x95)
Configures the tri-layer.
- Request: `[layer1:u8][layer2:u8][result:u8]`
- Response: OK

---

### Tamagotchi (0xA0–0xAF)

#### TAMA_QUERY (0xA0)
Full tamagotchi state.
- Request: empty payload
- Response:
```
[enabled:u8]
[state:u8]         — 0=idle, 1=happy, 2=excited, 3=eating, 4=sleepy, 5=sleeping, 6=sick, 7=sad, 8=celebrating
[hunger:u16 LE]    — 0-1000
[happiness:u16 LE] — 0-1000
[energy:u16 LE]    — 0-1000
[health:u16 LE]    — 0-1000 (computed average)
[level:u16 LE]     — 0-19
[xp:u16 LE]
[total_keys:u32 LE]
[max_kpm:u32 LE]
```

#### TAMA_ENABLE (0xA1) / TAMA_DISABLE (0xA2)
Enables/disables the tamagotchi.
- Request: empty payload
- Response: OK

#### TAMA_FEED (0xA3) / TAMA_PLAY (0xA4) / TAMA_SLEEP (0xA5) / TAMA_MEDICINE (0xA6)
Direct actions on the pet.
- Request: empty payload
- Response: OK

#### TAMA_SAVE (0xA7)
Forces a save to NVS.
- Request: empty payload
- Response: OK

---

### Diagnostics (0xB0–0xBF)

#### MATRIX_TEST (0xB0)
Toggles matrix test mode. In test mode, the keyboard stops sending HID reports and instead sends key state change events via unsolicited KR frames.

- Request: empty payload (toggle on/off)
- Response: `[enabled:u8][rows:u8][cols:u8]`
  - `enabled`: 1 = test mode active, 0 = normal mode
  - `rows`, `cols`: matrix dimensions

**Events (firmware → host, unsolicited):**

When test mode is active, every key state change generates a frame:
```
KR [0xB0] [OK] [3 bytes] [row:u8][col:u8][state:u8] [crc]
```
- `row`, `col`: position in the matrix
- `state`: 1 = pressed, 0 = released

**Typical flow:**
```
Host                          Firmware
 │                               │
 │  KS [B0] (toggle ON)         │
 ├──────────────────────────────>│
 │  KR [B0] OK [01,05,0D]       │  ← enabled=1, 5 rows, 13 cols
 │<──────────────────────────────┤
 │                               │
 │  KR [B0] OK [02,03,01]       │  ← row 2 col 3 pressed
 │<──────────────────────────────┤
 │  KR [B0] OK [02,03,00]       │  ← row 2 col 3 released
 │<──────────────────────────────┤
 │  KR [B0] OK [00,05,01]       │  ← row 0 col 5 pressed
 │<──────────────────────────────┤
 │  ...                          │
 │                               │
 │  KS [B0] (toggle OFF)        │
 ├──────────────────────────────>│
 │  KR [B0] OK [00,05,0D]       │  ← enabled=0, back to normal
 │<──────────────────────────────┤
```

**Usage**: equivalent to the [QMK Key Tester](https://config.qmk.fm/#/test). Lets you verify that each physical key works and identify faulty columns/rows.

#### NVS_RESET (0xB1)
Erases the configurations saved in NVS and reboots with default values.

- Request: `[mask:u8]` — bitmask of what to erase
- Response: OK then reboot

**Bitmask:**

| Bit | Value | Data erased |
|-----|--------|------------------|
| 0 | 0x01 | Keymaps + layer names |
| 1 | 0x02 | Macros |
| 2 | 0x04 | Statistics (keystats + bigrams) |
| 3 | 0x08 | Tap Dance, Combos, Leader, Key Override |
| 4 | 0x10 | Bluetooth (slots, state) |
| 5 | 0x20 | Tamagotchi |
| all | 0xFF | Erase everything |

**Examples:**
- `KS [B1] [01] [crc]` → erases keymaps only, reboot
- `KS [B1] [09] [crc]` → erases keymaps + advanced features, reboot
- `KS [B1] [FF] [crc]` → full factory reset, reboot

**Usage**: useful after a board variant change (V2 ↔ V2D) or when the NVS keymaps no longer match the physical layout.

---

### Dongle / Wireless (0xB2–0xB6, dongle role only)

These commands are only exposed on the dongle firmware (`CONFIG_KASE_DEVICE_ROLE_DONGLE`). To detect the role on the software side, read the feature list (KS_CMD_FEATURES) and look for the `RF_DONGLE` tag. If absent, the device is a standalone keyboard (V1/V2/V2D) — the commands below answer `ERR_UNKNOWN`.

All multi-byte fields are little-endian unless stated otherwise.

---

#### RF_PAIR_START (0xB2)
Opens a 30-second pairing window to welcome a half. The dongle switches the left radio to the rendezvous address/channel and waits for an `rf_pair_req`. The response is sent immediately (non-blocking); the exchange continues in `rf_rx_task`.

- Request: `[reset:u8]`
  - `reset = 0`: adds the next half to the existing pairs
  - `reset = 1`: first erases all NVS pairs, then opens the window
- Response: `[set_id_hi:u8][set_id_lo:u8][paired_count:u8]`
  - `set_id`: 16-bit set identifier (derived from eFuse) — useful to display on the software side
  - `paired_count`: number of halves currently paired (0..2)
- Error: `ERR_BUSY` if a window is already open or if the left radio is not present

To find out whether pairing succeeded, poll `RF_PAIR_LIST` after ~5-30 s: `paired_count` increases when a new half has completed the exchange.

---

#### RF_STATUS (0xB3)
Full snapshot of the radio link state for both halves. Idempotent, no side effects — can be polled at 1-2 Hz to drive a signal-bar indicator in the software.

- Request: empty payload
- Response: `51 bytes` (27 originally, then 31, 35, 43, 47; a client that only reads the first bytes stays correct)

| Offset | Type   | Field           | Description                                                |
|-------:|--------|-----------------|------------------------------------------------------------|
| 0      | u8     | `flags`         | bit0=link_left_up, bit1=link_right_up, bits2-7=reserved    |
| 1      | u8     | `sig_left`      | quality 0..255 (rf_signal_q255 — 0 = down/timeout)         |
| 2      | u8     | `sig_right`     | same, right side                                           |
| 3..6   | u32 LE | `hb_age_left`   | ms since the last heartbeat from the left half             |
| 7..10  | u32 LE | `hb_age_right`  | same, right side                                           |
| 11..14 | u32 LE | `pkt_rx_left`   | total number of accepted packets (incremented on success)  |
| 15..18 | u32 LE | `pkt_rx_right`  | same, right side                                           |
| 19..22 | u32 LE | `pkt_dup_left`  | number of rejected duplicates (seq already seen)           |
| 23..26 | u32 LE | `pkt_dup_right` | same, right side                                           |
| 27..30 | u32 LE | `transitions_ecrasees` | fusion: frames that changed a half's state before the engine had played the previous change (tap potentially lost/merged); 0 outside fusion |
| 31..34 | u32 LE | `gap_moteur_max_ms` | fusion: longest gap between two engine cycles since the last read (reset to 0 on each read) |
| 35..36 | u16 LE | `kb_usb_ok` | USB keyboard reports sent (saturates at 65535) |
| 37..38 | u16 LE | `kb_usb_refuses` | USB keyboard reports refused (endpoint silent for 2.5 ms) |
| 39..40 | u16 LE | `reprises` | USB bus suspended on send: remote wake-ups requested |
| 41..42 | u16 LE | `reprises_ratees` | same, bus still suspended 100 ms later |
| 43..46 | u32 LE | `reappuis` | fusion: re-presses of the same key < 30 ms after its release (stale repeat emitted by a half, or mechanical bounce longer than the debounce) — 0 expected since 2026-09-20 |
| 47 | u8 | `reappui_half` | last re-press: half (1 left, 2 right) |
| 48 | u8 | `reappui_key` | last re-press: key, `row*7+col` (half's coordinates) |
| 49..50 | u16 LE | `reappui_ms` | last re-press: delay after release, ms |

**Recommended mapping for 4 signal bars:**
```
sig >= 200 → 4 bars
sig >= 140 → 3 bars
sig >=  80 → 2 bars
sig >=  30 → 1 bar
sig <   30 → 0 bars / link broken
```

**Detecting a missing half**: if `link_<side>_up = 0` AND `pkt_rx_<side> == 0`, the half has never been seen since boot. If `link_<side>_up = 0` AND `pkt_rx_<side> > 0`, the half lost the link after having worked.

---

#### CONFIG_COHERENCE (0x17)
Fusion sync guard rail. In wireless mode, it is the dongle that runs the keymap engine with ITS OWN config; if it diverges from the one set on the left half, it silently types something else. The left half broadcasts the CRC-32 fingerprint of its keymap over RF (the `config_fp` field of the state frame); the dongle compares it to its own and exposes the result here. The software can poll at 1 Hz and warn the user of a divergence. Idempotent, no side effects. Outside fusion: all fields at 0, `match=0`.

- Request: empty payload
- Response: `13 bytes`

| Offset | Type   | Field     | Description                                                        |
|-------:|--------|-----------|-------------------------------------------------------------------|
| 0..3   | u32 LE | `own_fp`  | CRC-32 fingerprint of the dongle's keymap (0 = outside fusion)    |
| 4..7   | u32 LE | `left_fp` | last fingerprint broadcast by the left half (0 = never broadcast) |
| 8..11  | u32 LE | `age_ms`  | ms since that broadcast (0xFFFFFFFF = never)                      |
| 12     | u8     | `match`   | 1 = consistent (fingerprints equal and non-zero), 0 otherwise     |

**Reading it**: `match=1` → both engines type the same thing. `match=0` with `left_fp != 0` → **divergence** — it resolves ON ITS OWN: the dongle slips its keymap into the ACK payloads of the left half's normal transmissions (auto-sync, phase 3), and `match` goes back to 1 within a few seconds without plugging in the left half. The software therefore has nothing else to do but poll: `match` back at 1 IS the end-to-end acknowledgment of the sync. A divergence that PERSISTS (> ~1 min with the left half powered on wirelessly) signals a real problem (left half out of range, or in USB mode — it only syncs over the RF route). `left_fp = 0` or a high `age_ms` → the left half has not (or no longer) broadcast: unknown state, not necessarily a divergence. A single device's fingerprint can also be read via `CONFIG_FINGERPRINT` (0x16) on each one.

---

#### RF_PAIR_LIST (0xB4)
List of MAC addresses of the currently paired halves (read from NVS namespace `rf`).

- Request: empty payload
- Response: `13 bytes`

| Offset | Type    | Field           | Description                                  |
|-------:|---------|-----------------|----------------------------------------------|
| 0      | u8      | `paired_count`  | 0..2                                         |
| 1..6   | u8[6]   | `mac_left`      | WiFi STA MAC of the left half (or zeros)     |
| 7..12  | u8[6]   | `mac_right`     | same, right side                             |

If `mac_<side>` is `00:00:00:00:00:00`, the slot is free.

---

#### RF_PAIR_RESET (0xB5)
Erases all pairs (NVS namespace `rf` purged). Paired halves will time out their heartbeat and will not be able to reconnect without re-pairing. Useful for migrating a dongle to another set of halves, or for debugging.

- Request: empty payload
- Response: `[paired_count:u8]` — 0 after reset
- Error: `ERR_UNKNOWN` if the NVS write fails

The dongle keeps running; the radio keeps its current config. To repopulate the pairs, call `RF_PAIR_START` with `reset = 0`.

---

#### BATTERY (0xB6)
Last cached battery measurement for each half. The halves only send `EN_INFO_BATTERY` when the value changes (rate-limited), so `age_ms` can legitimately grow between two samples.

- Request: empty payload
- Response: `14 bytes` — 2 records of 7 bytes each

Format of a record (slot 0 = LEFT, slot 1 = RIGHT):

| Offset | Type   | Field      | Description                                      |
|-------:|--------|------------|---------------------------------------------------|
| 0      | u8     | `batt_dV`  | Voltage × 10 (volts × 10). `0xFF` = never seen    |
| 1      | u8     | `soc_pct`  | State of charge 0..100, DERIVED from voltage by the dongle (Li-ion 16340 table: 3.3 V→0, 3.5→15, 3.7→40, 3.9→70, 4.15→100). `0xFF` = unknown |
| 2      | u8     | `charging` | INFERRED state (no VBUS on the halves): 0 = discharging/unknown, 1 = probably charging (voltage rising by >= 0.1 V), 2 = full (plateau >= 4.15 V held >= 2 min). `0xFF` = voltage unknown |
| 3..6   | u32 LE | `age_ms`   | ms since the last update. `0xFFFFFFFF` = never received |

**Display rules on the software side:**
- If `batt_dV == 0xFF` OR `soc_pct == 0xFF` → show "—" / placeholder
- If `age_ms > 60000` (1 minute) → gray out the value (potentially stale)
- If `age_ms == 0xFFFFFFFF` → the half does not yet support battery telemetry (old firmware or battery not yet connected)

---

#### MONITOR (0xB7)
Consolidated snapshot of the keyboard's live state (and its wireless halves). Designed to drive a monitoring dashboard in KaSe_soft. Idempotent, no side effects — polling recommended at **1-2 Hz**.

- Request: empty payload (`KS [B7] 0000 00`)
- Response: `28 bytes` — always OK, sentinels for fields with no available source

| Offset | Type   | Field          | Description                                                       |
|-------:|--------|----------------|-------------------------------------------------------------------|
| 0      | u8     | `fmt`          | = `0x01` — format version, allows for future evolution            |
| 1      | u8     | `flags`        | bitmask (see below)                                               |
| 2      | u32 LE | `uptime_s`     | Seconds since boot                                                |
| 6      | u16 LE | `heap_free_kb` | Free heap in KB (saturates at 0xFFFF)                             |
| 8      | i8     | `temp_c`       | Internal temperature in °C; `INT8_MIN` (−128) = no sensor         |
| 9      | u8     | `layer_idx`    | Index of the active layer                                         |
| 10     | u8     | `wpm`          | Current words per minute (saturates at 255)                       |
| 11     | u32 LE | `keys_total`   | Total number of keys pressed, lifetime cumulative (restored from NVS `key_stats_tot` at boot) |
| 15     | u8     | `sig_left`     | Quality of the left RF link 0..255 (0 if `has_rf=0`)              |
| 16     | u8     | `sig_right`    | Same, right side                                                  |
| 17     | u16 LE | `hb_age_L_ms`  | ms since the last heartbeat from the left half (truncated to u16, saturates at 0xFFFF — RF_STATUS uses u32 for these fields) |
| 19     | u16 LE | `hb_age_R_ms`  | Same, right side (same u16/0xFFFF truncation)                     |
| 21     | u8     | `batt_L_dV`    | Left battery voltage x10. `0xFF` = unknown (0 if `has_rf=0`)      |
| 22     | u8     | `batt_L_soc`   | Left state of charge 0..100%. `0xFF` = unknown                    |
| 23     | u8     | `batt_L_chg`   | 0 = discharging, 1 = charging. `0xFF` = unknown                   |
| 24     | u8     | `batt_R_dV`    | Right battery voltage x10. `0xFF` = unknown (0 if `has_rf=0`)     |
| 25     | u8     | `batt_R_soc`   | Right state of charge 0..100%. `0xFF` = unknown                   |
| 26     | u8     | `batt_R_chg`   | 0 = discharging, 1 = charging. `0xFF` = unknown                   |
| 27     | u8     | `bt_slot`      | Active Bluetooth slot (0-2; `BT_MAX_DEVICES = 3`)                 |

Battery note: on the dongle, the battery fields are relayed as-is from the cache (same source as BATTERY 0xB6) → `0xFF` = unknown / never seen. On a standalone keyboard (`has_rf=0`) these offsets are 0. On the software side: treat `batt_*_dV` (or `_soc`) == `0xFF`, and 0 when `has_rf=0`, as "no data" (display "—").

**Flags (offset 1):**

| Bit | Mask | Constant     | Meaning                              |
|-----|--------|--------------|-------------------------------------|
| 0   | 0x01   | `HAS_RF`     | Dongle-role firmware with RF radio  |
| 1   | 0x02   | `LINK_L`     | Left half connected                 |
| 2   | 0x04   | `LINK_R`     | Right half connected                |
| 3   | 0x08   | `USB`        | USB link active                     |
| 4   | 0x10   | `BT_CONN`    | Bluetooth connected                 |

**Standalone keyboard (no dongle):** `has_rf = 0`, offsets 15..26 (RF signal, heartbeat, battery) are zero. The software detects RF presence via the `HAS_RF` flag (and/or the `RF_DONGLE` tag in the FEATURES response).

**Temperature:** `temp_c = INT8_MIN` (−128) means the sensor is not available.

**Python parsing example:**

```python
import struct

def parse_monitor(payload: bytes) -> dict:
    assert len(payload) == 28
    fmt, flags = payload[0], payload[1]
    uptime,    = struct.unpack_from("<I", payload, 2)
    heap_kb,   = struct.unpack_from("<H", payload, 6)
    temp       = struct.unpack_from("<b", payload, 8)[0]
    layer, wpm = payload[9], payload[10]
    keys,      = struct.unpack_from("<I", payload, 11)
    sig_l, sig_r = payload[15], payload[16]
    hb_l,      = struct.unpack_from("<H", payload, 17)
    hb_r,      = struct.unpack_from("<H", payload, 19)
    bl_dv, bl_soc, bl_chg = payload[21], payload[22], payload[23]
    br_dv, br_soc, br_chg = payload[24], payload[25], payload[26]
    bt_slot    = payload[27]
    has_rf     = bool(flags & 0x01)
    return dict(
        fmt=fmt, flags=flags, uptime_s=uptime, heap_free_kb=heap_kb,
        temp_c=temp if temp != -128 else None,
        layer_idx=layer, wpm=wpm, keys_total=keys,
        sig_left=sig_l if has_rf else None,
        sig_right=sig_r if has_rf else None,
        hb_age_L_ms=hb_l, hb_age_R_ms=hb_r,
        batt_L=(bl_dv / 10, bl_soc, bool(bl_chg)) if bl_dv not in (0, 0xFF) else None,
        batt_R=(br_dv / 10, br_soc, bool(br_chg)) if br_dv not in (0, 0xFF) else None,
        bt_slot=bt_slot,
    )
```

**Exemple de parsing C# (KaSe_soft) :**

```csharp
public class MonitorSnapshot
{
    // Raw fields
    public byte   Fmt        { get; init; }
    public byte   Flags      { get; init; }
    public uint   UptimeS    { get; init; }
    public ushort HeapFreeKb { get; init; }
    public sbyte  TempC      { get; init; }
    public byte   LayerIdx   { get; init; }
    public byte   Wpm        { get; init; }
    public uint   KeysTotal  { get; init; }
    public byte   SigLeft    { get; init; }
    public byte   SigRight   { get; init; }
    public ushort HbAgeLeftMs  { get; init; }
    public ushort HbAgeRightMs { get; init; }
    public byte   BattLdV    { get; init; }
    public byte   BattLSoc   { get; init; }
    public byte   BattLChg   { get; init; }
    public byte   BattRdV    { get; init; }
    public byte   BattRSoc   { get; init; }
    public byte   BattRChg   { get; init; }
    public byte   BtSlot     { get; init; }

    // Flag helpers
    public bool HasRf      => (Flags & 0x01) != 0;
    public bool LinkLeft   => (Flags & 0x02) != 0;
    public bool LinkRight  => (Flags & 0x04) != 0;
    public bool UsbActive  => (Flags & 0x08) != 0;
    public bool BtConnected => (Flags & 0x10) != 0;

    // Derived
    public bool TempAvailable => TempC != -128;
    // Battery unknown sentinel: 0xFF (dongle cache) or 0 when !HasRf.
    public bool BattLValid => BattLdV != 0 && BattLdV != 0xFF;
    public bool BattRValid => BattRdV != 0 && BattRdV != 0xFF;
    public double? BattLVoltage => BattLValid ? BattLdV / 10.0 : (double?)null;
    public double? BattRVoltage => BattRValid ? BattRdV / 10.0 : (double?)null;

    public static MonitorSnapshot Parse(byte[] p)
    {
        // All multi-byte fields are little-endian.
        // BitConverter is LE on all modern platforms; assert if needed:
        // if (!BitConverter.IsLittleEndian) throw new PlatformNotSupportedException();
        if (p.Length < 28) throw new ArgumentException("payload must be 28 bytes");
        return new MonitorSnapshot
        {
            Fmt          = p[0],
            Flags        = p[1],
            UptimeS      = BitConverter.ToUInt32(p, 2),
            HeapFreeKb   = BitConverter.ToUInt16(p, 6),
            TempC        = (sbyte)p[8],
            LayerIdx     = p[9],
            Wpm          = p[10],
            KeysTotal    = BitConverter.ToUInt32(p, 11),
            SigLeft      = p[15],
            SigRight     = p[16],
            HbAgeLeftMs  = BitConverter.ToUInt16(p, 17),
            HbAgeRightMs = BitConverter.ToUInt16(p, 19),
            BattLdV      = p[21],
            BattLSoc     = p[22],
            BattLChg     = p[23],
            BattRdV      = p[24],
            BattRSoc     = p[25],
            BattRChg     = p[26],
            BtSlot       = p[27],
        };
    }
}
```

**Recommended dashboard usage:** 1-2 Hz timer → `KS [B7] 0000 00` → `Parse(response.Payload)` → update the WPF bindings. No error handling needed (the command always returns OK).

---

#### TRACKPAD_GET (0xB8)
Returns the active trackpad acceleration config (dongle only).

- Request: empty payload
- Response: `7 bytes`

| Offset | Type   | Field      | Description                                      |
|-------:|--------|------------|--------------------------------------------------|
| 0      | u8     | `fmt`      | = `0x01` — format version                        |
| 1      | u16 LE | `base`     | Base gain x100 (e.g. 90 = 0.90x)                 |
| 3      | u16 LE | `accel`    | Acceleration coefficient (added per unit of speed / 100) |
| 5      | u16 LE | `gain_max` | Maximum gain x100 (e.g. 300 = 3.00x)             |

Gains are expressed in hundredths: 100 = 1.00x, 90 = 0.90x, 300 = 3.00x. The applied curve is: `gain = clamp(base + accel * speed / 100, base, gain_max)`.

Factory defaults: `base=90, accel=40, gain_max=300`.

---

#### TRACKPAD_SET (0xB9)
Modifies the trackpad acceleration config, applies it immediately, and persists it to NVS (dongle only).

- Request: `6 bytes` — `[base:u16 LE][accel:u16 LE][gain_max:u16 LE]`
- Response: `7 bytes` — echo of the applied config (same format as TRACKPAD_GET)
- Error: `ERR_INVALID` if payload < 6 bytes
- Error: `ERR_RANGE` if out of bounds (`base < 1`, `base > gain_max`, `gain_max > 1000`, `accel > 1000`)

**Example:** enabling an aggressive curve (base=80, accel=60, gain_max=400):
```python
import struct
payload = struct.pack("<HHH", 80, 60, 400)   # 6 bytes
ser.write(ks_frame(0xB9, payload))
```

---

### OTA (0xF0–0xFF)

#### OTA_START (0xF0)
Starts a firmware update.
- Request: `[firmware_size:u32 LE]`
- Response: `[chunk_size:u16 LE]` (always 4096)
- Error: `ERR_RANGE` if size = 0 or > 2MB

#### OTA_DATA (0xF1)
Sends a firmware chunk. Repeat until completion.
- Request: payload = raw firmware bytes (max chunk_size)
- Response: `[received:u32 LE][total:u32 LE]`
- When received == total: firmware validated, OK response then reboot
- Each chunk is CRC-protected by the KS frame

#### OTA_ABORT (0xF2)
Cancels the ongoing OTA.
- Request: empty payload
- Response: OK
- Error: `ERR_INVALID` if no OTA is in progress

### OTA Flow

```
Host                          Firmware
 │                               │
 │  KS [F0] [size:u32]          │
 ├──────────────────────────────>│
 │  KR [F0] OK [chunk_size:u16] │
 │<──────────────────────────────┤
 │                               │
 │  KS [F1] [chunk 1]           │   ─┐
 ├──────────────────────────────>│    │
 │  KR [F1] OK [recv][total]    │    │ repeat
 │<──────────────────────────────┤    │
 │  ...                          │   ─┘
 │                               │
 │  KS [F1] [last chunk]        │
 ├──────────────────────────────>│
 │  KR [F1] OK [total][total]   │  ← firmware validated
 │<──────────────────────────────┤
 │                               │  reboot
```

---

## Full Python example

```python
import serial, struct

def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def ks_frame(cmd_id, payload=b""):
    hdr = bytes([0x4B, 0x53, cmd_id, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    return hdr + payload + bytes([crc8(payload)])

def parse_kr(data):
    if len(data) < 7 or data[0:2] != b"\x4b\x52":
        return None
    cmd, status = data[2], data[3]
    plen = data[4] | (data[5] << 8)
    payload = data[6:6+plen]
    return {"cmd": cmd, "status": status, "payload": payload}

ser = serial.Serial("/dev/ttyACM0", timeout=2)

# Ping
ser.write(ks_frame(0x04))
r = parse_kr(ser.read(64))
assert r["status"] == 0

# Get version
ser.write(ks_frame(0x01))
r = parse_kr(ser.read(256))
print(f"Version: {r['payload'].decode()}")

# Get current layer keymap
ser.write(ks_frame(0x12))
r = parse_kr(ser.read(4096))
layer = r["payload"][0]
keycodes = [struct.unpack_from("<H", r["payload"], 1+i*2)[0] for i in range(65)]
print(f"Layer {layer}: {keycodes[:5]}...")

# Set one key: layer 0, row 0, col 0 = KC_A (0x04)
ser.write(ks_frame(0x11, bytes([0, 0, 0, 0x04, 0x00])))
r = parse_kr(ser.read(64))
assert r["status"] == 0

# BT query
ser.write(ks_frame(0x80))
r = parse_kr(ser.read(256))
slot, init, conn, pairing = r["payload"][:4]
print(f"BT: slot={slot} init={init} conn={conn} pairing={pairing}")

ser.close()
```

## C# example (KaSe_soft)

```csharp
byte[] KsFrame(byte cmdId, byte[] payload = null) {
    payload ??= Array.Empty<byte>();
    var frame = new byte[5 + payload.Length + 1];
    frame[0] = 0x4B; frame[1] = 0x53;
    frame[2] = cmdId;
    frame[3] = (byte)(payload.Length & 0xFF);
    frame[4] = (byte)((payload.Length >> 8) & 0xFF);
    Array.Copy(payload, 0, frame, 5, payload.Length);
    frame[^1] = Crc8(payload);
    return frame;
}

byte Crc8(byte[] data) {
    byte crc = 0;
    foreach (var b in data) {
        crc ^= b;
        for (int i = 0; i < 8; i++)
            crc = (byte)((crc & 0x80) != 0 ? (crc << 1) ^ 0x31 : crc << 1);
    }
    return crc;
}
```

## Test

```bash
source ~/esp/esp-idf/export.sh
python3 scripts/test_binary_protocol.py /dev/ttyACM0
```

32 tests covering all commands, error handling, and legacy coexistence.
