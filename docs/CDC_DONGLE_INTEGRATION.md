# KaSe dongle — software integration guide

This document is for the KeSp_controller team (the remapping software).
It describes how to detect, talk to, and display the state of a KaSe dongle
over USB, as opposed to a standalone keyboard (V1/V2/V2D).

> Prerequisite: read `CDC_BINARY_PROTOCOL.md` for the KS/KR framing + CRC-8.
> All commands here are standard `KS_CMD_*`.

---

## 1. Role detection (dongle vs standalone keyboard)

The dongle firmware and the keyboard firmware expose **the same USB device**:
composite HID (keyboard + mouse) + CDC ACM. The VID/PID does not change. To
tell the two apart, the software must send `KS_CMD_FEATURES` (0x02) at boot
and look for the `RF_DONGLE` tag in the returned string.

```
KS [02] []  → KR [02] OK "MT,LT,LM,...,MATRIX_TEST,RF_DONGLE,RF_STATUS,RF_PAIR,BATTERY"
```

| Tag present | Role detected                |
|--------------|-----------------------------|
| `RF_DONGLE`  | Wireless (split) dongle     |
| absent       | Standalone keyboard (V1/V2/V2D)|

On a standalone keyboard, the `RF_*` and `BATTERY` commands answer
`ERR_UNKNOWN` (status 0x01) — treat this the same as the tag being absent.

**Recommendation**: hide `paired_count`, signal bars, and battery from the UI
when the tag is absent, so as not to display empty fields on a V2.

---

## 2. Typical lifecycle

```
┌─ The software starts ─────────────────────────────────────────┐
│ 1. Opens /dev/ttyACM<N> (CDC ACM)                             │
│ 2. sends KS_CMD_PING → KR_OK to verify the CDC link           │
│ 3. sends KS_CMD_VERSION → dongle version (e.g.: v3.8.0-...)   │
│ 4. sends KS_CMD_FEATURES → detects the RF_DONGLE tag          │
│ 5. if dongle → sends KS_CMD_RF_PAIR_LIST → paired MACs        │
│ 6. starts the RF_STATUS polling loop (every 1-2 s)            │
│ 7. starts the BATTERY polling loop (every 5-10 s)             │
└──────────────────────────────────────────────────────────────┘
```

The "heavy" commands (keymap, macros, layout) stay identical between the
dongle and a standalone keyboard — this is intentional: the dongle stores the
keymap and does all the HID decoding. The halves only send matrix bytes.

---

## 3. Displaying the radio link (polling RF_STATUS)

`KS_CMD_RF_STATUS` (0xB3) returns 27 bytes, idempotent, no side effects.
Recommended rate: **1-2 Hz** in the UI (the resource is free, but there is no
point saturating the CDC).

### Output bytes (reminder)

```
[0]    flags         bit0=link_L_up, bit1=link_R_up
[1]    sig_L         0..255 (0 = down)
[2]    sig_R         0..255 (0 = down)
[3..6] hb_age_L_ms   u32 LE
[7..10] hb_age_R_ms  u32 LE
[11..14] pkt_rx_L    u32 LE
[15..18] pkt_rx_R    u32 LE
[19..22] pkt_dup_L   u32 LE
[23..26] pkt_dup_R   u32 LE
```

### Mapping signal → bars

The firmware already encodes the combined quality (heartbeat age + retry
count) in `sig_<side>` via `rf_signal_q255()`. The software only needs to map it:

```python
def bars(sig: int) -> int:
    if sig >= 200: return 4
    if sig >= 140: return 3
    if sig >=  80: return 2
    if sig >=  30: return 1
    return 0  # link lost, display an X icon
```

### State of the three possible halves

| Half           | `link_up` | `pkt_rx` | Interpretation                  |
|----------------|-----------|----------|---------------------------------|
| Never seen     | 0         | 0        | Never connected (not paired, or powered off) |
| Paired, no contact | 0     | > 0      | Known but link broken (dead battery, out of range) |
| Active         | 1         | > 0      | OK, display `bars(sig)`        |

### `pkt_dup_*` counters

Duplicates are packets retransmitted by the half that carry the same `seq`
as a packet already accepted. This is normal and reflects link health: a
stable duplicate rate = stable link; a spike = radio disturbance. The
software may display this ratio in a "diagnostics" panel but it is not vital.

---

## 4. Pairing workflow

Pairing is a user-driven operation: pressing a "+ Half" button in the
software, which triggers this sequence.

```
software                              dongle              half
 │                                       │                  │
 │   KS_CMD_RF_PAIR_START [reset=0]      │                  │
 ├──────────────────────────────────────>│                  │
 │   KR OK [set_id_hi,set_id_lo,paired]  │                  │
 │<──────────────────────────────────────┤                  │
 │                                       │  (radio L on     │
 │                                       │   rendezvous,    │
 │                                       │   30s window)    │
 │                                       │                  │
 │                                       │<- rf_pair_req ───┤
 │                                       ├-- rf_pair_ack --→│
 │                                       │                  │
 │   (poll RF_PAIR_LIST every 2 s)       │                  │
 ├──────────────────────────────────────>│                  │
 │   KR OK [paired=1, mac_L=AA..., mac_R=00...]             │
 │<──────────────────────────────────────┤                  │
```

**Suggested UI**:
1. "Pair half" button shows a 30 s countdown
2. Every 2 s, poll `RF_PAIR_LIST` and compare `paired_count` to the previous value
3. If `paired_count` increases: show "Half paired!" and exit
4. On timeout: show "Timeout — check that the half is in pairing mode"

**`reset = 1` vs `reset = 0`**:
- `reset = 0` (default): adds to the existing pairs (max 2)
- `reset = 1`: equivalent to `RF_PAIR_RESET` followed by `PAIR_START` — useful
  to re-pair from scratch (e.g. a new pair of halves)

---

## 5. Displaying the battery

`KS_CMD_BATTERY` (0xB6) returns 14 bytes: 7 per half. Recommended rate:
**5-10 s** in the UI (the halves only send `EN_INFO_BATTERY` when the value
changes, so polling faster serves no purpose).

```
slot 0 (LEFT):  [batt_dV][soc_pct][charging][age_ms u32 LE]
slot 1 (RIGHT): [batt_dV][soc_pct][charging][age_ms u32 LE]
```

### Sentinel values

| Field      | `0xFF` / `0xFFFFFFFF` means                      |
|------------|-------------------------------------------------|
| `batt_dV`  | never received from the half                    |
| `soc_pct`  | SoC unknown (BMS not connected / old firmware)  |
| `charging` | state unknown                                   |
| `age_ms`   | no sample received since the dongle booted      |

### Display rules

```python
def render_battery(rec):
    dv, soc, chg, age = rec
    if dv == 0xFF or age == 0xFFFFFFFF:
        return "—"             # no telemetry
    label = f"{soc}%"          # or f"{dv/10:.1f} V"
    if chg == 1:
        label = "⚡ " + label
    if age > 60_000:           # more than 60 s
        label = "(?) " + label  # potentially stale
    return label
```

---

## 6. Compatibility and evolution

### Guarantees

- Command IDs are **never** reassigned. A future firmware may add
  `KS_CMD_XXX` on a free ID but will not renumber the existing ones.
- The `RF_DONGLE` tag remains the official marker of the role.
- The size of the RF_STATUS/RF_PAIR_LIST/BATTERY responses is **fixed** and
  will not change. Any future extension will go through a new ID.

### Forward compatibility

If a future firmware adds bits to `RF_STATUS.flags` or fields after
`pkt_dup_right`, **the software must ignore unknown bits/bytes** and rely
only on the `len` field of the KR header. No hashing on the content.

### Reserved

Free IDs around the RF commands:
- `0xB7..0xBF`: reserved for future dongle/wireless diagnostics
- `0x96..0x9F`: reserved for generic features

---

## 7. Errors and reconnection

| Status      | When                                        | Software-side action                    |
|-------------|---------------------------------------------|-----------------------------------------|
| `OK` 0x00   | Everything is fine                          | —                                       |
| `ERR_BUSY` 0x05 | `RF_PAIR_START` while a window is already open | Back off 2 s, retry              |
| `ERR_UNKNOWN` 0x01 | Command sent to a standalone keyboard   | Check `RF_DONGLE` in FEATURES      |
| `ERR_CRC` 0x02 | Wrong CRC8 on receipt                     | Re-check the CRC8 computation (poly 0x07) |

**CDC disconnection**: if `/dev/ttyACM<N>` disappears (dongle unplugged or
rebooted), the software should (1) detect it via SIGIO/poll, (2) periodically
try to reopen it (~1 s), (3) replay the workflow from section 2 on
reconnection — the dongle's state (pairs, layer, keymap) is fully persisted
in NVS, nothing needs "restoring".

---

## 8. Appendix: raw framing examples

All examples use CRC-8/MAXIM (polynomial 0x31, init 0x00, no reflection)
computed only over the **payload** (not over cmd_id or len). An empty payload
gives CRC = 0x00.

### Polling RF_STATUS

```
TX (software → dongle):
  4B 53      magic 'KS'
  B3         cmd RF_STATUS
  00 00      len = 0
  <crc8>     CRC over [B3, 00, 00]

RX (dongle → software):
  4B 52      magic 'KR'
  B3         cmd RF_STATUS
  00         status OK
  1B 00      len = 27
  03         flags = link_L + link_R up
  C8 D2      sig_L=200 sig_R=210
  E8 03 00 00  hb_age_L = 1000 ms
  D0 07 00 00  hb_age_R = 2000 ms
  ... 16 remaining bytes ...
  <crc8>
```

### Reading pairing

```
TX : 4B 53  B4  00 00  <crc>
RX : 4B 52  B4  00  0D 00
     02                                       paired_count = 2
     AA BB CC DD EE FF                        mac_left
     11 22 33 44 55 66                        mac_right
     <crc>
```

### Starting pairing with reset

```
TX : 4B 53  B2  01 00 01 <crc>     (reset = 1)
RX : 4B 52  B2  00  03 00 1A 2F 00 <crc>
     set_id = 0x1A2F, paired_count = 0
```

---

## 9. Appendix: quick ID reference

| ID    | Name              | Direction | Resp. size  | Software frequency |
|-------|-------------------|-----------|-------------|-----------------|
| 0x01  | VERSION           | get       | variable    | on connect      |
| 0x02  | FEATURES          | get       | variable    | on connect      |
| 0x04  | PING              | get       | 0           | slow heartbeat  |
| 0xB2  | RF_PAIR_START     | action    | 3 bytes     | on user action  |
| 0xB3  | RF_STATUS         | get       | 27 bytes    | 1-2 Hz          |
| 0xB4  | RF_PAIR_LIST      | get       | 13 bytes    | 0.5 Hz during pairing, otherwise on demand |
| 0xB5  | RF_PAIR_RESET     | action    | 1 byte      | on user action  |
| 0xB6  | BATTERY           | get       | 14 bytes    | 0.1-0.2 Hz      |

For the full list (keymap, macros, stats, etc.), see
`CDC_BINARY_PROTOCOL.md`.
