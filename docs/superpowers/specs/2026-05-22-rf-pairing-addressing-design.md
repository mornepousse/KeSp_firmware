# KaSe RF Pairing & Per-Set Addressing — Design

**Date** : 2026-05-22
**Branch** : `dongle-firmware`
**Status** : Design decided — ready for implementation
**Depends on** : `2026-05-11-dongle-firmware-design.md` (dongle arch), `2026-05-22-half-peripherals-espnow-design.md` (ESP-NOW stubs)

---

## 1. Problem & Goal

The KaSe split wireless keyboard (1 dongle + 2 halves) currently uses **fixed NRF24 addresses** (`KaSe.01` / `KaSe.02`) and **fixed channels** (76 / 82 decimal). Two KaSe keyboards placed side by side would cross-talk at the NRF level: dongle A's radios accept packets from half B because the address and channel match.

Goals:
1. **Per-set isolation** — each set (1 dongle + its 2 halves) gets a unique NRF address + channel derived from the dongle's hardware identity. NRF hardware address filtering then rejects foreign packets even on the same channel.
2. **Good neighbor** — per-set channel spread reduces aggregate air-time collision; unicast-only ESP-NOW avoids broadcast storms; both protocols stay low duty-cycle.
3. **Backwards-compatible fallback** — an unpaired set works identically to today's fixed-address firmware. Pairing is opt-in.
4. **Live e-ink layer display** — pairing establishes the MAC addresses needed for the ESP-NOW unicast info channel, enabling the dongle to push layer-change events to the halves whose e-ink panels then display the active layer name dynamically.

---

## 2. Identity & Derivation

### 2.1 set_id

`set_id` is a `uint16_t` that uniquely identifies one KaSe set (one dongle + its paired halves).

**Source**: the dongle's factory WiFi MAC address, read via:

```c
uint8_t mac[6];
esp_read_mac(mac, ESP_MAC_WIFI_STA);   // reads eFuse, no WiFi init required
```

**Derivation** (CRC-16/CCITT, polynomial 0x1021, init 0xFFFF, no reflect, no final XOR):

```
set_id = crc16_ccitt(mac, 6)
```

Properties:
- Deterministic from hardware; recomputed at every boot (not stored in NVS).
- Two ESP32-S3 chips from different factory runs: CRC-16 collision probability ≈ 1/65536. Acceptable — address filtering is the primary mechanism; channel spread is a bonus.
- `set_id = 0x0000` and `set_id = 0xFFFF` are reserved as the "factory default / unpaired" sentinel (see §2.4). In the astronomically unlikely event that crc16 of a real MAC lands on 0x0000 or 0xFFFF, treat it as 0x0001 (one-off adjust with no stored state needed, document in code).

### 2.2 NRF Address Derivation (set_id ≠ 0)

5-byte NRF24 address structure:

```
addr[0] = 'K'           (0x4B)
addr[1] = 'S'           (0x53)
addr[2] = set_id >> 8   (high byte)
addr[3] = set_id & 0xFF (low byte)
addr[4] = slot          (0x01 for left half, 0x02 for right half)
```

- The dongle listens on both slot addresses (two physical radios, each programmed to its own address).
- Each half transmits to the address matching its slot.
- ESB auto-ACK: the half sets TX_ADDR = RX_ADDR_P0 = the same 5-byte address (required for auto-ACK in PTX mode — already the case in `rf_driver_init_tx`).

### 2.3 NRF Channel Derivation (set_id ≠ 0)

NRF channel N corresponds to 2400 + N MHz. WiFi channel 13 (the highest used in home environments) tops at ~2484 MHz center (N ≈ 84 with 20 MHz bandwidth edge at ~2494 MHz, but real deployments rarely use ch 13). To stay in the upper band and above all WiFi:

```
base_ch = 80 + 2 * (set_id % 20)     // range: 80..118, even values only
ch_left  = base_ch                    // 2480..2518 MHz
ch_right = base_ch + 1                // 2481..2519 MHz
```

Properties:
- 20 slots × 2 channels = 40 distinct (ch_left, ch_right) pairs across the pool.
- All slots ≥ 2480 MHz → above WiFi channel 13 upper edge (~2473 MHz for ch 11, last widely used channel in 2.4 GHz).
- Minimum separation between two neighboring sets sharing the same slot index: NRF address filtering prevents cross-reception even if two sets land on the same channel. Channel diversity is a secondary benefit (reduces aggregate collision probability for sets that are also neighbours in space).
- `ch_right = base_ch + 1` preserves the existing 1 MHz separation between L and R channels (current firmware: 76/82, i.e. 6 MHz apart; this design reduces it to 1 MHz, which is sufficient at 2 Mbps where the spectral null is at ±1 MHz from centre). If bench measurement shows interference between L and R channels within the same set, increase to `base_ch + 2` in the implementation — does not affect the spec.

### 2.4 Unpaired Fallback (set_id = 0 sentinel)

**Critical invariant**: if no NVS pairing data is present on a half (i.e. the half has never been paired), the half uses `set_id = 0` as a sentinel meaning "use factory defaults". Factory defaults are the compile-time values currently in `board_rf.h` / `board.h`:

| Half | Factory default address | Factory default channel |
|------|------------------------|------------------------|
| Left  | `{'K','a','S','e', 0x01}` | 0x4C (76, 2476 MHz) |
| Right | `{'K','a','S','e', 0x02}` | 0x52 (82, 2482 MHz) |

The dongle similarly falls back to these values when no pairing NVS record exists (i.e. `paired_count = 0`).

**The derivation formula does NOT map set_id=0 to the factory addresses.** The formula is only applied for `set_id != 0`. The code checks NVS for a valid pairing record at init time; if absent, it uses the board compile-time constants unchanged. This is cleaner than trying to force the formula to reproduce the existing 4-byte ASCII base `KaSe`, which would require a special-case encoding anyway.

Concretely, the existing `board_rf_left_cfg()` and `board_rf_right_cfg()` inline functions remain valid: they are the factory default configuration. The pairing module, when active, overrides `rx_addr[4]`, `addr_suffix`, and `channel` in the returned `rf_radio_cfg_t` before it is passed to `rf_driver_init()`.

### 2.5 WiFi/ESP-NOW Channel Derivation (set_id ≠ 0)

```
wifi_ch = (set_id % 3 == 0) ? 1 : (set_id % 3 == 1) ? 6 : 11
```

Equivalently: `wifi_ch = {1, 6, 11}[set_id % 3]`.

These are the three non-overlapping 2.4 GHz WiFi channels. Sets are distributed across all three. Both ends of a set (dongle and each half) derive this value identically from `set_id` — no coordination needed.

**Divergence from the dongle firmware design spec (2026-05-11)**: the dongle spec §6 defines an NVS key `rf.wifi_ch` (default 11, user-configurable). With per-set derivation, `wifi_ch` is no longer configurable independently — it is derived from `set_id`. The `rf.wifi_ch` NVS key is **deprecated** for paired sets and becomes a dead key that is ignored. For unpaired sets (factory default), `rf.wifi_ch` is also ignored and WiFi channel 6 (the current hardcoded default in `espnow_link.c`) is used as-is for the factory ESP-NOW channel. Document this change in the code comment at `espnow_link.c:62`. If a future requirement needs manual channel override for RF regulatory compliance reasons, add a separate `rf.wifi_ch_override` key with explicit documentation.

---

## 3. Pairing Flow

### 3.1 States

```
DONGLE:
  IDLE (normal RX) ──→ PAIRING (30 s window) ──→ IDLE (resume RX, new addresses)
                         triggered by: KS_CMD_RF_PAIR_START (CDC from controller)

HALF:
  IDLE (normal TX) ──→ PAIRING (30 s window) ──→ IDLE (resume TX, new address/channel)
                         triggered by: BOOT button held 3 s (or key combo TBD)
```

Both devices must enter the pairing window within the same 30-second period. The pairing exchange is initiated by the half.

### 3.2 Pairing Rendezvous Parameters (Fixed, Immutable)

The pairing channel and address are fixed constants known to all KaSe firmware:

```c
#define RF_PAIR_CHANNEL      0x28       /* 40 decimal, 2440 MHz */
#define RF_PAIR_ADDR         { 'K', 'S', 'P', 'R', 0xFF }  /* 5 bytes */
```

`RF_PAIR_CHANNEL = 0x28 (40)` sits below WiFi ch 1 center (2412 MHz) and below the data-channel pool (80+). This maximizes RF isolation from both live data traffic and WiFi.

`RF_PAIR_ADDR` uses the `KSP` prefix (KaSe Pairing) + `0xFF` suffix (distinct from any slot 0x01/0x02).

**Rendezvous collision risk**: if two nearby keyboards simultaneously enter pairing mode, both halves transmit `PKT_PAIR_REQ` on the same channel + address. The dongle will receive both and could mis-assign. Mitigation: the dual-trigger requirement (dongle requires explicit CDC command from its tethered controller; half requires physical button hold) makes simultaneous pairing accidental. The 30-second window limits exposure. Document as known limitation; cryptographic confirmation is out of scope for this design.

### 3.3 Packet Formats

#### PKT_PAIR_REQ (half → dongle)

Type nibble: `0xF` (already reserved in `rf_packet.h` as `PKT_PAIR_REQ`).

```
Byte 0    : 0xF0  (type=0xF, flags=0x00)
Bytes 1-6 : half_wifi_mac[6]   (ESP_MAC_WIFI_STA of the half, eFuse read)
Total     : 7 bytes
```

The half transmits `PKT_PAIR_REQ` repeatedly at 200 ms intervals during the pairing window (up to 150 transmissions over 30 s). ESB auto-ACK provides delivery confirmation; the half stops after receiving `PKT_PAIR_ACK`.

#### PKT_PAIR_ACK (dongle → half)

New type nibble: `0xE`. Add to `rf_packet.h`:

```c
#define PKT_TYPE_PAIR_REQ   0xF   /* already defined */
#define PKT_TYPE_PAIR_ACK   0xE   /* new */
```

The dongle sends `PKT_PAIR_ACK` as an **out-of-band NRF TX** on the pairing channel + address. The dongle temporarily switches to PTX mode on the pairing radio (radio L, `board_rf_left_cfg()` modified for pairing) to transmit the ACK. This requires a brief PTX/PRX mode switch.

```
Byte 0    : 0xE0  (type=0xE, flags=0x00)
Bytes 1-2 : set_id [big-endian, high byte first]
Bytes 3-8 : dongle_wifi_mac[6]
Byte  9   : slot (0x01 = left assigned, 0x02 = right assigned)
Total     : 10 bytes
```

After sending `PKT_PAIR_ACK`, the dongle switches back to PRX mode on the pairing channel to listen for the next half (if `paired_count < 2`).

### 3.4 Slot Assignment

The dongle maintains a `paired_count` (0, 1, or 2) in its working state during the pairing window:

- `paired_count = 0` → next `PKT_PAIR_REQ` → assign slot `0x01` (left).
- `paired_count = 1` → next `PKT_PAIR_REQ` → assign slot `0x02` (right).
- `paired_count = 2` → window closes.

The assignment is positional (first half to pair = left, second = right). The user is responsible for pairing the physically correct half first. This is a known UX limitation; a button-labelled confirmation UI is out of scope.

### 3.5 Pairing Sequence (Step by Step)

```
Controller (USB)    Dongle              Pairing channel (NRF)      Half
─────────────────   ──────────────────  ──────────────────────────  ──────────────
KS_CMD_RF_PAIR_START ──→                                            BOOT held 3s
                    enter PAIRING state                             enter PAIRING state
                    switch L radio →                                switch to PAIR ch+addr
                    PAIR ch+addr PRX                                begin REQ loop (200ms)
                                        ←── PKT_PAIR_REQ {mac} ───
                    validate (not dup)
                    assign slot
                    persist NVS (mac_left or mac_right)
                    switch L radio →
                    PAIR ch+addr PTX    ──→ PKT_PAIR_ACK ──────────→
                                            {set_id, dongle_mac, slot}
                    switch L radio →                                validate slot+set_id
                    PAIR ch+addr PRX                                persist NVS
                    (wait for 2nd half                              (set_id, slot, mac_dongle)
                     or 30s timeout)
                    ← KS_CMD_RF_PAIR_START returns KR_OK
                      with {set_id, paired_count}
```

After pairing both halves (or after the 30-second window expires):
- Dongle exits PAIRING, switches both radios to derived per-set addresses + channels (or factory defaults if `paired_count = 0`).
- A **reboot is NOT required**. The dongle calls `rf_driver_set_channel()` + reprograms the address register (`REG_RX_ADDR_P0`) on each radio via a new `rf_driver_set_address()` function. The halves do the same on their radio after writing NVS (they reboot at the half to apply new config, because the half has no runtime channel/address switch path at this design stage — the half's NRF is initialized once in `half_scan_task` from NVS + board defaults). The half reboots itself (`esp_restart()`) 2 seconds after writing NVS and receiving the ACK.

**Rationale for half reboot vs dongle hot-switch**: the dongle has two radios managed centrally in `rf_rx_start()`, with `rf_driver_set_channel()` already exposed. The half has a single radio initialized once in `half_scan_task`. Adding a hot-switch path on the half requires refactoring `board_nrf_cfg()` to load from NVS and re-running `rf_driver_init_tx()`. A reboot is simpler and the latency (~2 s) is acceptable since pairing is an infrequent user-initiated operation. The dongle does NOT reboot because it is USB-connected and a reboot would drop the HID/CDC connection to the host.

### 3.6 Re-pairing

To re-pair (e.g. replace a half):
1. Trigger `KS_CMD_RF_PAIR_START` from the controller with a `reset=1` flag (clears NVS `mac_left`/`mac_right`/`paired_count`).
2. Repeat the pairing flow from §3.5.

There is no "forget one half only" command in this design (add later if needed).

---

## 4. NVS Schema

### 4.1 Namespace `rf` (pre-existing, extended)

All keys in namespace `"rf"` (constant `RF_STORAGE_NAMESPACE`).

**Dongle-side keys** (written during pairing, read at boot):

| Key | Type | Size | Notes |
|-----|------|------|-------|
| `mac_left` | blob | 6 B | WiFi STA MAC of the left half. All-zeros = not paired. |
| `mac_right` | blob | 6 B | WiFi STA MAC of the right half. All-zeros = not paired. |
| `paired_count` | u8 | 1 B | 0 = factory defaults, 1 = one half paired, 2 = both paired. |

`set_id` is NOT stored on the dongle — recomputed at boot from `esp_read_mac(ESP_MAC_WIFI_STA)`.

The deprecated keys `addr_base`, `ch_left`, `ch_right` from the dongle spec §6 are superseded by the derivation. They are not read nor written by the new code; any existing values in NVS are silently ignored. Do not delete them — `nvs_get` on an absent key returns `ESP_ERR_NVS_NOT_FOUND`, which is harmless.

**Half-side keys** (written after receiving `PKT_PAIR_ACK`):

| Key | Type | Size | Notes |
|-----|------|------|-------|
| `set_id` | u16 | 2 B | Received from dongle in PKT_PAIR_ACK. |
| `slot` | u8 | 1 B | 0x01 = left, 0x02 = right. |
| `mac_dongle` | blob | 6 B | Dongle WiFi MAC from PKT_PAIR_ACK. Used as ESP-NOW peer. |

A half with `set_id = 0` (key absent or explicitly set to 0) uses factory defaults.

### 4.2 NVS Read at Boot

**Dongle boot sequence** (in `rf_rx_start()`, before `rf_driver_init()`):

```
1. Open NVS namespace "rf" (READONLY).
2. Read "paired_count" (u8). If absent or 0: use factory defaults (board_rf_left_cfg() / board_rf_right_cfg() unchanged). Return.
3. Compute set_id = crc16_ccitt(own_wifi_mac, 6). Guard: if set_id == 0 or 0xFFFF → set_id = 0x0001.
4. Derive NRF addresses (§2.2) and channels (§2.3) from set_id.
5. Patch the rf_radio_cfg_t structs returned by board_rf_left_cfg() / board_rf_right_cfg():
   - rx_addr[0..3] = {'K','S', set_id>>8, set_id&0xFF}
   - addr_suffix (for L) = 0x01; addr_suffix (for R) = 0x02
   - channel = ch_left (for L); channel = ch_right (for R)
6. Read "mac_left" and "mac_right" blobs for later ESP-NOW peer registration.
7. Close NVS.
```

**Half boot sequence** (in `half_scan_task()`, before `rf_driver_init_tx()`):

```
1. Open NVS namespace "rf" (READONLY).
2. Read "set_id" (u16). If absent or 0: use factory default (board_nrf_cfg() unchanged). Return.
3. If set_id == 0xFFFF: treat as 0x0001 (guard, same as dongle).
4. Derive NRF address (§2.2) and channel (§2.3) for own slot.
5. Read "slot" (u8) to determine own slot (0x01 or 0x02).
6. Patch rf_radio_cfg_t from board_nrf_cfg():
   - rx_addr[0..3] = {'K','S', set_id>>8, set_id&0xFF}
   - addr_suffix = slot
   - channel = (slot == 0x01) ? ch_left : ch_right
7. Read "mac_dongle" blob for ESP-NOW peer registration.
8. Close NVS.
```

---

## 5. Apply to NRF — Changes to Existing Code

### 5.1 New function: `rf_derive_config()`

Add to `main/comm/rf/` (new file `rf_pairing.c` / `rf_pairing.h`):

```c
/* Compute set_id from WiFi STA MAC (eFuse, no WiFi required). */
uint16_t rf_compute_set_id(void);

/* Derive NRF address bytes for a given set_id + slot into an rf_radio_cfg_t.
 * Modifies rx_addr[0..3], addr_suffix, and channel in place.
 * set_id = 0 or 0xFFFF: no-op (caller keeps factory defaults). */
void rf_apply_set_id(rf_radio_cfg_t *cfg, uint16_t set_id, uint8_t slot);
```

`rf_apply_set_id` formula:

```c
if (set_id == 0 || set_id == 0xFFFF) return;  /* factory default sentinel */
cfg->rx_addr[0] = 'K';
cfg->rx_addr[1] = 'S';
cfg->rx_addr[2] = (uint8_t)(set_id >> 8);
cfg->rx_addr[3] = (uint8_t)(set_id & 0xFF);
cfg->addr_suffix = slot;
uint8_t base_ch = (uint8_t)(80 + 2 * (set_id % 20));
cfg->channel = (slot == 0x01) ? base_ch : (uint8_t)(base_ch + 1);
```

### 5.2 `rf_rx_start()` — Dongle

After obtaining `lcfg`/`rcfg` from `board_rf_left_cfg()`/`board_rf_right_cfg()`, load pairing data from NVS and call `rf_apply_set_id(&lcfg, set_id, 0x01)` and `rf_apply_set_id(&rcfg, set_id, 0x02)` before passing them to `rf_driver_init()`. No other changes to the existing task logic.

### 5.3 `half_scan_task()` — Half

After obtaining `nrf_cfg` from `board_nrf_cfg()`, load pairing data from NVS and call `rf_apply_set_id(&nrf_cfg, set_id, slot)` before passing to `rf_driver_init_tx()`. No other changes.

### 5.4 Pairing Mode Radio Switch (Dongle)

During pairing, the dongle must reconfigure radio L temporarily:

```
a. ce_low(radio_L)                   → stop RX
b. rf_driver_set_channel(radio_L, RF_PAIR_CHANNEL)
c. write_reg_buf(radio_L, REG_RX_ADDR_P0, RF_PAIR_ADDR, 5)
d. ce_high(radio_L)                  → resume PRX on pairing channel

[receive PKT_PAIR_REQ]

e. ce_low(radio_L)
f. configure radio_L as PTX (write REG_TX_ADDR = REG_RX_ADDR_P0 = RF_PAIR_ADDR, CONFIG PTX)
g. rf_driver_send(radio_L, pair_ack_buf, 10)
h. configure radio_L back to PRX
i. ce_high(radio_L)                  → listen for next half
```

This sequence must be serialized with normal RX in `rf_rx_task`. The cleanest approach: add a `pairing_mode` flag checked at the top of `drain_radio()`; when set, skip normal packet processing and run the pairing state machine instead. The pairing state machine runs inside `rf_rx_task` (same task, no new task).

### 5.5 New `rf_packet.h` Additions

```c
#define PKT_TYPE_PAIR_REQ   0xF   /* half→dongle (already reserved) */
#define PKT_TYPE_PAIR_ACK   0xE   /* dongle→half (new) */

typedef struct {
    uint8_t half_wifi_mac[6];
} rf_pair_req_t;

typedef struct {
    uint16_t set_id;          /* big-endian */
    uint8_t  dongle_wifi_mac[6];
    uint8_t  slot;            /* 0x01=left, 0x02=right */
} rf_pair_ack_t;

static inline uint16_t rf_encode_pair_req(uint8_t *buf, const uint8_t mac[6]) {
    buf[0] = (PKT_TYPE_PAIR_REQ << 4);
    memcpy(buf + 1, mac, 6);
    return 7;
}

static inline uint16_t rf_encode_pair_ack(uint8_t *buf, const rf_pair_ack_t *a) {
    buf[0] = (PKT_TYPE_PAIR_ACK << 4);
    buf[1] = (uint8_t)(a->set_id >> 8);
    buf[2] = (uint8_t)(a->set_id & 0xFF);
    memcpy(buf + 3, a->dongle_wifi_mac, 6);
    buf[9] = a->slot;
    return 10;
}

static inline bool rf_decode_pair_req(const uint8_t *buf, uint16_t len, uint8_t mac_out[6]) {
    if (len < 7 || rf_packet_type(buf, len) != PKT_TYPE_PAIR_REQ) return false;
    memcpy(mac_out, buf + 1, 6);
    return true;
}

static inline bool rf_decode_pair_ack(const uint8_t *buf, uint16_t len, rf_pair_ack_t *a) {
    if (len < 10 || rf_packet_type(buf, len) != PKT_TYPE_PAIR_ACK) return false;
    a->set_id = ((uint16_t)buf[1] << 8) | buf[2];
    memcpy(a->dongle_wifi_mac, buf + 3, 6);
    a->slot = buf[9];
    return true;
}
```

### 5.6 CDC Command: `KS_CMD_RF_PAIR_START`

New CDC command in `cdc_dongle_cmds.c`:

```
KS request  : [0x4B][0x53][cmd][len=1][reset:u8][crc8]
KS response : [0x4B][0x52][cmd][status][len=3][set_id_hi:u8][set_id_lo:u8][paired_count:u8][crc8]
```

`reset = 1` clears `mac_left`, `mac_right`, `paired_count` from NVS before starting the pairing window. `reset = 0` resumes an interrupted pairing (adds to existing `paired_count`).

The command returns immediately with `KS_STATUS_OK` and the current `{set_id, paired_count}` once the pairing window timer has started (asynchronous — the actual pairing exchange is driven by RF events).

A separate `KS_CMD_RF_PAIR_STATUS` command (poll-able by the controller) returns the current pairing state: `{paired_count, mac_left[6], mac_right[6], pairing_active:u8}`.

---

## 6. Apply to ESP-NOW — Changes to Existing Code

### 6.1 `espnow_link_init()` — Peer Registration

Replace the current stub with real peer registration using paired MACs:

**Dongle** (reads `mac_left`, `mac_right` from NVS namespace `rf`):

```c
esp_now_peer_info_t peer = {
    .channel = derived_wifi_ch,   /* from set_id % 3 → {1,6,11} */
    .ifidx   = WIFI_IF_STA,
    .encrypt = false,
};
memcpy(peer.peer_addr, mac_left, 6);
esp_now_add_peer(&peer);            /* skip if mac_left is all-zeros */
memcpy(peer.peer_addr, mac_right, 6);
esp_now_add_peer(&peer);            /* skip if mac_right is all-zeros */
```

**Half** (reads `mac_dongle` from NVS namespace `rf`):

```c
esp_now_peer_info_t peer = {
    .channel = derived_wifi_ch,
    .ifidx   = WIFI_IF_STA,
    .encrypt = false,
};
memcpy(peer.peer_addr, mac_dongle, 6);
esp_now_add_peer(&peer);            /* skip if mac_dongle is all-zeros */
```

For unpaired devices (no pairing NVS), `derived_wifi_ch = 6` (current hardcoded default) and no peers are added — same behaviour as the current stub.

### 6.2 `espnow_recv_cb()` — Sender MAC Filter

Add MAC filtering in `espnow_info_dispatch()`. Load paired peer MACs at init into a static array; `espnow_info_dispatch` compares `recv_info->src_addr` against each entry and drops packets from unknown senders:

```c
/* Dongle: accept from mac_left and mac_right only.
 * Half:   accept from mac_dongle only.
 * If no peers configured (unpaired): drop all (recv is a no-op anyway). */
if (!is_known_peer(recv_info->src_addr)) {
    ESP_LOGD(TAG, "ESP-NOW recv from unknown MAC — dropped");
    return;
}
```

This prevents a neighbouring KaSe set (on the same WiFi channel) from injecting layer events into this set's halves.

### 6.3 WiFi Channel Application

After computing `derived_wifi_ch` from `set_id % 3` (or using the factory default 6 for unpaired), call:

```c
esp_wifi_set_channel(derived_wifi_ch, WIFI_SECOND_CHAN_NONE);
```

This replaces the current NVS `rf.wifi_ch` lookup in `espnow_link.c:62`. The `rf.wifi_ch` key is no longer read.

### 6.4 `espnow_send()` — Unicast Only

`espnow_send()` already takes an explicit `mac[6]` argument and calls `esp_now_send(mac, ...)` directly. No change needed. Callers must always pass a specific peer MAC, never the broadcast address `{0xFF×6}`. This invariant is enforced by the fact that the only callers are `layer_changed()` (dongle) and `heartbeat_timer_cb()` battery stub (half), both of which use MAC addresses loaded from NVS.

---

## 7. Live E-ink Layer Display

### 7.1 Dongle: `layer_changed()` → ESP-NOW Push

In `dongle_engine_state.c`, replace the `layer_changed()` stub:

```c
void layer_changed(void)
{
#if CONFIG_KASE_HAS_ESPNOW
    static uint8_t mac_left[6]  = {0};
    static uint8_t mac_right[6] = {0};
    static bool    macs_loaded  = false;

    if (!macs_loaded) {
        nvs_handle_t h;
        if (nvs_open("rf", NVS_READONLY, &h) == ESP_OK) {
            size_t sz = 6;
            nvs_get_blob(h, "mac_left",  mac_left,  &sz);
            sz = 6;
            nvs_get_blob(h, "mac_right", mac_right, &sz);
            nvs_close(h);
        }
        macs_loaded = true;
    }

    /* Build EN_INFO_LAYER payload */
    en_layer_t l = { .layer_idx = current_layout };
    const char *name = get_layer_name(current_layout);   /* from keymap.c */
    if (name) strncpy(l.name, name, 16);
    else       memset(l.name, 0, 16);

    /* Build EN_INFO_STATE payload */
    extern uint8_t current_modifiers;   /* from key_processor.c */
    en_state_t s = { .modifiers = current_modifiers, .flags = 0 };

    /* Unicast to both halves — fire-and-forget (low rate, no retry needed) */
    bool ll = !is_all_zeros(mac_left,  6);
    bool lr = !is_all_zeros(mac_right, 6);
    if (ll) {
        espnow_send(mac_left,  EN_INFO_LAYER, &l, sizeof(l));
        espnow_send(mac_left,  EN_INFO_STATE, &s, sizeof(s));
    }
    if (lr) {
        espnow_send(mac_right, EN_INFO_LAYER, &l, sizeof(l));
        espnow_send(mac_right, EN_INFO_STATE, &s, sizeof(s));
    }
    if (!ll && !lr) {
        ESP_LOGD(TAG, "layer_changed: no paired halves — ESP-NOW skip");
    }
#endif
}
```

`layer_changed()` is called from `rf_rx_task` context (inside `run_engine_cycle()` → `build_keycode_report()` → `process_matrix_changes()`). `espnow_send()` calls `esp_now_send()` which is thread-safe (internally queued). The call is low-rate (only on layer change, typically seconds apart) — acceptable in `rf_rx_task` context.

Loading `mac_left`/`mac_right` from NVS on first call (lazy load with static flag) avoids the overhead on every call while keeping the code simple. The static variables are safe because `layer_changed()` is always called from the same task.

### 7.2 Half: `on_layer()` → Update State → Notify E-ink

In `espnow_info.c`, replace the `on_layer()` stub:

```c
#if CONFIG_KASE_HAS_ESPNOW && CONFIG_KASE_DEVICE_ROLE_HALF
static void on_layer(const en_layer_t *l)
{
    if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_half_state.layer_idx = l->layer_idx;
        memcpy(g_half_state.layer_name, l->name, 16);
        xSemaphoreGive(g_half_state_mutex);
    }
    /* Wake eink task for refresh — non-blocking notify */
    TaskHandle_t h = eink_get_task_handle();
    if (h != NULL) {
        xTaskNotify(h, 0x01, eSetBits);
    }
}
#endif
```

`on_state()` similarly: update `g_half_state.modifiers` / `g_half_state.flags`, then notify eink.

### 7.3 E-ink Task: `eink_get_task_handle()` Exposure

Add to `eink.h` / `eink_lvgl.h`:

```c
/* Returns the handle of the eink_lvgl_task, or NULL if not started.
 * Used by espnow_info.c to wake the task on state change. */
TaskHandle_t eink_get_task_handle(void);
```

In `eink_lvgl.c`, save the task handle in `eink_lvgl_start()`:

```c
static TaskHandle_t s_eink_task_handle = NULL;

void eink_lvgl_start(void) {
    xTaskCreatePinnedToCore(eink_lvgl_task, "eink_lvgl", 4096, NULL, 3,
                            &s_eink_task_handle, 0);
}

TaskHandle_t eink_get_task_handle(void) { return s_eink_task_handle; }
```

### 7.4 E-ink Task: Dynamic Layer Label

In `eink_lvgl_init()`, replace (or augment) the static `"KaSe"` label with a dynamic layer-name label:

```c
/* Static label replaced by dynamic layer-name label.
 * Initial text "KaSe" shown until first EN_INFO_LAYER is received. */
static lv_obj_t *s_label_layer = NULL;
s_label_layer = lv_label_create(scr);
lv_label_set_text(s_label_layer, "KaSe");      /* placeholder */
lv_obj_set_style_text_color(s_label_layer, lv_color_black(), LV_PART_MAIN);
lv_obj_set_pos(s_label_layer, 60, 80);
```

In `eink_lvgl_task()`, react to the `xTaskNotify` wake:

```c
static void eink_lvgl_task(void *arg) {
    for (;;) {
        uint32_t notify_val = 0;
        /* Wake on LVGL timer OR on notify from espnow_info (layer change) */
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms == 0 || sleep_ms > 50) sleep_ms = 50;

        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &notify_val,
                                               pdMS_TO_TICKS(sleep_ms));
        if (notified == pdTRUE && (notify_val & 0x01)) {
            /* Layer or state changed — update label and invalidate */
            char name_copy[17] = {0};
            if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                memcpy(name_copy, g_half_state.layer_name, 16);
                xSemaphoreGive(g_half_state_mutex);
            }
            name_copy[16] = '\0';
            if (name_copy[0] == '\0') memcpy(name_copy, "KaSe", 5);
            if (s_label_layer != NULL && lv_obj_is_valid(s_label_layer)) {
                lv_label_set_text(s_label_layer, name_copy);
                lv_obj_invalidate(s_label_layer);
            }
        }
        /* lv_timer_handler was already called above — no second call needed */
    }
}
```

**E-ink refresh latency**: full refresh of SSD1681 takes ~1.5 s. This is acceptable for layer changes (user triggers a layer change, the panel updates within the next 1-2 seconds). The `lv_timer_handler` call drives LVGL rendering; `eink_push()` (in `flush_cb`) drives the SSD1681 full update sequence. The SPI lock (`half_spi_lock()`) is held for the duration of `eink_push()` — this is already the case in the existing eink architecture and the long BUSY wait occurs inside the lock. Key TX is unaffected because the NRF SPI transactions (3-10 bytes, ~5 µs) complete between lock acquisitions.

### 7.5 Divergence from Dongle Spec §3 (ESP-NOW Model)

The 2026-05-11 dongle design spec §4 states: "quand le dongle active ESP-NOW (cold path), on suspend les NRF (`ce_low()` + skip RX FIFO check) pour la fenêtre". This was designed for OTA and config push (high-bandwidth, user-initiated).

**This design CHANGES the ESP-NOW model for the info channel**:

- The info channel is **always-on unicast** at low rate (one ESP-NOW frame per layer change, typically < 1 frame/second in normal use).
- NRF is **NOT muted** during info channel ESP-NOW sends. The WiFi radio and NRF24 radio are different physical RF chains on the ESP32-S3; they can operate simultaneously. The risk is WiFi→NRF interference (WiFi transmit energy leaks into the 2.4 GHz band), but at the info channel's low duty cycle (< 0.1% air time), this causes at most 1-2 additional MAX_RT events per layer change, recovered by heartbeat reconciliation.
- NRF muting (CE low) remains the strategy for OTA and config push (Plan 5) only, where the ESP-NOW duty cycle is high enough to warrant it.

This divergence must be called out in a comment in `espnow_link.c` referencing this spec.

---

## 8. Coexistence Summary

### 8.1 NRF24 Layer

| Property | Value | Effect |
|----------|-------|--------|
| Address width | 5 bytes | 2^40 address space; hardware filtering rejects foreign packets |
| Per-set address | `{K,S,id_hi,id_lo,slot}` | Unique per dongle eFuse MAC; no coordination needed |
| Per-set channel | `80 + 2*(id%20)` | 20 possible channels; sets spread across upper 2.4 GHz band |
| ESB auto-ACK + retry | ARC=3, ARD=500 µs | Transient interference causes MAX_RT; heartbeat reconciles within 100 ms |
| Duty cycle | ~0.5% per half | Events only; no polling; minimal air time |
| L/R channel separation | 1 MHz | Sufficient at 2 Mbps; no intra-set collision |

### 8.2 ESP-NOW / WiFi Layer

| Property | Value | Effect |
|----------|-------|--------|
| Mode | Unicast only | No broadcast; no hidden-terminal amplification |
| Per-set WiFi channel | `{1,6,11}[id%3]` | Sets distributed across non-overlapping channels |
| Recv MAC filter | paired peers only | Foreign ESP-NOW frames dropped at application level |
| Rate | < 1 frame/s (info channel) | < 0.01% duty cycle; negligible |
| NRF muting | NOT used for info channel | WiFi TX leakage at 0.01% DC: ≤2 MAX_RT/layer-change |

### 8.3 Good-Neighbor Guarantee

Neither NRF nor ESP-NOW traffic uses broadcast addressing after pairing. Channel diversity ensures that two neighbouring sets are unlikely to share both RF band and WiFi channel simultaneously. Even in the worst case (same NRF channel, same WiFi channel): NRF hardware address filtering provides complete data isolation; ESP-NOW MAC filtering provides complete info-channel isolation. Throughput degradation from channel congestion is bounded by the low duty cycles of both protocols.

---

## 9. Security & Limitations

| Limitation | Severity | Mitigation |
|-----------|----------|------------|
| Simultaneous pairing of two nearby sets | Low | Requires dual-trigger (CDC + button); 30 s window limits exposure |
| No packet encryption or authentication | Medium | Physical proximity required for pairing; factory-floor attack scenario only |
| Slot assignment is positional | Low | UX limitation: user pairs left half first; label halves physically |
| CRC-16 collision between two dongles | Very low (1/65536) | Address filtering still isolates by full 5-byte address even if channel collides |
| Half reboot required after pairing | Low | ~2 s interruption; infrequent operation |
| `set_id = 0x0000` or `0xFFFF` MAC CRC | Negligible | Force to `0x0001`; documented in code |

**Not in scope**: ECDH key exchange, per-packet nonce/authentication, manufacturing-provisioned certificates, pairing via physical button alone (without CDC trigger on dongle), runtime half replacement without reboot.

---

## 10. Testing

### 10.1 Unit Tests (Host-Side, `test/`)

The following functions are pure (no FreeRTOS, no SPI, no NVS) and suitable for `test_runner` host tests:

**Test group: `rf_set_id_derivation`**

```c
// Test 1: set_id=0 → rf_apply_set_id is a no-op
// Setup: cfg with factory default rx_addr={'K','a','S','e'}, channel=0x4C, suffix=0x01
// Call: rf_apply_set_id(&cfg, 0, 0x01)
// Assert: cfg unchanged (rx_addr still 'K','a','S','e'; channel still 0x4C; suffix still 0x01)

// Test 2: set_id=0xFFFF → rf_apply_set_id is a no-op (same as set_id=0)

// Test 3: known set_id → correct address bytes
// set_id = 0x1234: rf_apply_set_id(&cfg, 0x1234, 0x01)
// Assert: rx_addr = {'K','S', 0x12, 0x34}, suffix = 0x01
//         channel = 80 + 2*(0x1234 % 20) = 80 + 2*8 = 96

// Test 4: slot 0x02 → suffix = 0x02, channel = base + 1
// set_id = 0x1234: rf_apply_set_id(&cfg, 0x1234, 0x02)
// Assert: rx_addr = {'K','S', 0x12, 0x34}, suffix = 0x02, channel = 97

// Test 5: set_id % 20 = 0 (e.g. set_id = 0x0014 = 20)
// base_ch = 80 + 2*0 = 80; ch_L = 80, ch_R = 81

// Test 6: set_id % 20 = 19 (e.g. set_id = 0x0013 = 19)
// base_ch = 80 + 2*19 = 118; ch_L = 118, ch_R = 119
// Assert: all channels ≤ 125 (NRF24 max channel)

// Test 7: WiFi channel derivation
// set_id % 3 == 0 → wifi_ch = 1
// set_id % 3 == 1 → wifi_ch = 6
// set_id % 3 == 2 → wifi_ch = 11

// Test 8: factory default unchanged with paired_count=0
// Verify that set_id=0 sentinel preserves {'K','a','S','e',0x01} / ch 0x4C for left
//                                  and    {'K','a','S','e',0x02} / ch 0x52 for right
```

**Test group: `rf_pairing_packet`**

```c
// Test 9: rf_encode_pair_req / rf_decode_pair_req round-trip
// Test 10: rf_encode_pair_ack / rf_decode_pair_ack round-trip (check big-endian set_id)
// Test 11: rf_decode_pair_req with wrong type byte → returns false
// Test 12: rf_decode_pair_ack with short buffer → returns false
```

**Test group: `rf_slot_assignment`**

```c
// Test 13: paired_count=0 → assign slot 0x01 on first REQ
// Test 14: paired_count=1 → assign slot 0x02 on second REQ
// Test 15: paired_count=2 → window closes, third REQ ignored
// (pairing state machine is a pure function if extracted into rf_pairing_sm.c)
```

### 10.2 Integration Test (On-Hardware)

1. Flash factory firmware (no NVS pairing) on dongle + 2 halves → verify keyboard works identically to pre-spec (factory default addresses/channels active).
2. Trigger pairing via controller CDC: pair left half → verify `rf.mac_left` written in NVS, half reboots, resumes TX on derived channel.
3. Pair right half → verify `rf.mac_right` written.
4. Verify `rf_driver_init()` log on dongle shows new channel: `radio ch=<derived> addr=KS<id_hi><id_lo>.01 init OK`.
5. Verify keyboard still functions on derived addresses.
6. Flash second KaSe dongle + 2 halves alongside first → verify no cross-talk (keys from set B do not appear on dongle A's HID output).
7. Change layer on dongle → verify e-ink on both halves updates to correct layer name within 2 s.

---

## 11. Out of Scope

- **Encryption / ECDH**: PKT_PAIR_ACK is sent in plaintext; any eavesdropper in range during the 30-second pairing window could capture `set_id` and `dongle_mac`. Encryption requires key exchange protocol (future).
- **Runtime button-only pairing** (no CDC/USB): the dongle requires a CDC trigger. Adding a physical pairing button to the dongle board is a hardware revision.
- **Battery telemetry** on the half: `half_scan_task.c` heartbeat ESP-NOW battery stub (marked `TODO STUB`) is not resolved by this spec.
- **OTA push via ESP-NOW** (Plan 5): the NRF muting strategy from the dongle spec remains applicable to OTA; this spec does not change it.
- **Partial re-pair** (replace one half without re-pairing the other): left as a future `KS_CMD_RF_PAIR_RESET_SLOT` command.
- **Frequency hopping**: explicitly deferred per the dongle spec §4.
