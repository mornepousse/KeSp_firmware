# Dongle slot 2 — the mouse contract

What a device must do on the air to be *the mouse* of a KeSp dongle. Written
so that a firmware that shares **no code** with this repository (the
Conchodytes rewrite in Rust `esp-hal`, decided 2026-09-21) can be built and
tested against it. Everything below is what `kase_dongle` actually implements
today (`main/comm/rf/rf_rx_task.c`, `rf_driver.c`, `rf_pairing.c`,
`rf_packet.h`, `rf_slot.h`); the reference transmitter is the `MOUSE` role of
this firmware (`main/app/mouse_task.c`, `main/comm/rf/mouse_relay_tx.c`), which
stays in the tree until the new firmware pairs and clicks, then goes.

Golden vectors, produced by the C encoders (not by hand,
`scripts/gen_mouse_slot_vectors.sh`): `docs/contracts/mouse_slot_vectors.json`. A port's first test is to reproduce
every byte of that file.

## 1. Radio layer (nRF24L01+, Enhanced ShockBurst)

Both ends run the same register set; a mismatch is not an error, it is
silence.

| Register | Value | Meaning |
|---|---|---|
| `RF_SETUP` (0x06) | `0x06` | **1 Mbps**, 0 dBm. Not 2 Mbps (was, audit M1), not 250 kbps (clones lack it) |
| `SETUP_AW` (0x03) | `0x03` | 5-byte addresses |
| `CONFIG` | `EN_CRC \| CRCO` | **2-byte CRC**; PTX = `0x3E`, PRX = `0x3F` (all IRQ masks off, PWR_UP) |
| `EN_AA` / `EN_RXADDR` | `0x01` / `0x01` | pipe 0 only, auto-ACK on |
| `FEATURE` (0x1D) | `0x06` | `EN_DPL \| EN_ACK_PAY` — dynamic payload length; ACK payloads enabled (the mouse never uses them, but the chip config must match) |
| `DYNPD` (0x1C) | `0x01` | DPL on pipe 0 |
| `SETUP_RETR` (0x04) | **mouse `0x00`**, dongle `0x1F` | see §4 — the mouse does **zero** retransmission |

PTX rule of ESB: `TX_ADDR` **and** `RX_ADDR_P0` carry the same 5-byte address,
otherwise the ACK is never received. Payloads are 1..32 bytes. Transmit =
`W_TX_PAYLOAD`, CE high ≥ 10 µs, then poll `STATUS` for `TX_DS` (bit 5, ACK
received) or `MAX_RT` (bit 4); on `MAX_RT`, `FLUSH_TX` or the FIFO stays
blocked; clear both bits by writing `0x30` to `STATUS`. Timings: 1.5 ms from
power-down to standby (the reference driver waits 2 ms, 5 ms after power-on
for clones), 130 µs standby → active.

⚠ An ESB ACK proves the *dongle's chip* matched address and channel. It does
not prove the dongle's software did anything with the frame.

## 2. Addresses and channels

Two regimes. Which one applies is decided by the mouse's own persistent
storage (§3): unpaired → factory, paired → derived.

**Factory (unpaired) — slot 2:** address `"KaSe"` + `0x02` = `4b 61 53 65 02`,
channel `0x52` (2482 MHz). The dongle listens here only when its own NVS holds
no pairing; a dongle that has ever paired listens on the derived address, so an
unpaired mouse in front of a paired dongle is simply deaf — pair it.

**Paired — derived from `set_id`:**

```
set_id  = CRC-16/CCITT-FALSE(dongle WiFi-STA MAC, 6 bytes)   poly 0x1021, init 0xFFFF, no reflect, no xor-out
          0x0000 and 0xFFFF are sentinels ("unpaired") → replaced by 0x0001
address = 'K' 'S' (set_id >> 8) (set_id & 0xFF) slot          slot = 0x02 for the mouse
base_ch = 80 + 2 * (set_id % 20)                              80..118, even
channel = base_ch + 1                                         (slot 1 = base_ch, slot 2 = base_ch + 1)
```

The mouse does not compute the CRC itself: `set_id` arrives in the pairing
ACK (§3). Check value of the CRC: `"123456789"` → `0x29B1`. Worked example in
the vectors file (bench dongle `ac:a7:04:18:82:24` → `set_id 0x9044`, address
`4b 53 90 44 02`, channel `0x69`).

**Rendezvous (pairing only):** address `"KSPR"` + `0xFF` = `4b 53 50 52 ff`,
channel `0x28` (2440 MHz, inside WiFi territory on purpose: seconds, short
range).

⚠ **Known issue, inherited, not to be copied silently.** Derived channels run
80..119, i.e. 2480..2519 MHz, and the ISM band ends at 2483.5 MHz — the same
`rf_slot.h` that documents the ceiling. The chip transmits up to 2525 MHz and
the link works (the keyboard has typed on it for weeks); it is a compliance
problem, not a functional one. Fixing it changes the derivation for every
device of a set at once (dongle + both halves + mouse) and forces a re-pair,
so it is a firmware-wide decision, tracked here rather than fixed in passing.
A new implementation must implement the rule *as is* to talk to today's
dongles.

## 3. Pairing

The dongle opens a window on request from the host (`scripts/kesp_cdc.py
/dev/ttyACM0 pair`, CDC command `KS_CMD_RF_PAIR_START`; `pair 1` forgets
previous peers first). Window: **120 s**, closes early once two peers are
paired. During the window the dongle's radio 1 sits in PRX on the rendezvous
address/channel.

Mouse side, one attempt (the reference does up to **20**, 200 ms apart):

1. PTX on the rendezvous: `RF_CH = 0x28`, `TX_ADDR = RX_ADDR_P0 = 4b 53 50 52 ff`.
2. Send `PAIR_REQ v2`, 9 bytes: `F0` · own WiFi-STA MAC (6, as read from
   eFuse, `esp_read_mac(ESP_MAC_WIFI_STA)` order) · `02` (declared slot) · `02`
   (`RF_DEV_MOUSE`). The declared slot wins on the dongle
   (`rf_pairing_resolve_slot`): pairing order does not matter, the mouse
   always gets slot 2.
3. Switch to **PRX on the same rendezvous address/channel**, `FLUSH_RX`, CE
   high, and wait up to **300 ms** for one payload.
4. Expect `PAIR_ACK`, 10 bytes: `E0` · `set_id` **big-endian** (2) · dongle
   WiFi-STA MAC (6) · slot (1). Anything else: log and retry.
5. Persist `set_id`, `slot`, dongle MAC; from then on (the reference reboots)
   run on the derived address/channel of §2, PTX.

Dongle side, for the record: it de-duplicates on MAC (a re-pair of the same
mouse returns the same slot without bumping its peer count), **persists
before acknowledging** — no ACK means the pairing was not recorded, do not
assume success from the ESB-level ACK of step 2 — and sends the `PAIR_ACK` as
a one-shot PTX excursion on the rendezvous, returning to PRX immediately.
The ESB ACK of step 2 therefore says "a dongle is on the rendezvous", the
`PAIR_ACK` payload says "and it recorded you".

Persistent state the mouse needs, and nothing else: `set_id` (u16), `slot`
(u8), dongle MAC (6 bytes, informative). The reference keeps them in NVS
namespace `rf`, keys `set_id` / `slot` / `mac_dongle`; the new firmware may
store them anywhere it likes — nothing reads them but itself.

## 4. Operating: the HID report

One frame type, 6 bytes, PTX on the paired address/channel:

```
byte 0  0x50      PKT_TYPE_HIDREPORT (0x5 << 4), flags nibble 0
byte 1  0x01      RF_HID_SUB_MOUSE
byte 2  buttons   bit0 left, bit1 right, bit2 middle (standard boot-mouse order)
byte 3  dx        int8, two's complement, counts since the previous frame
byte 4  dy        int8
byte 5  wheel     int8, detents since the previous frame
```

The dongle decodes it and calls `hid_send_mouse(buttons, dx, dy, wheel)` on
its USB HID — **no engine, no filtering, no acceleration** on the dongle side:
what is in the frame is what the host sees. Vectors: `50 01 01 05 fd 00` =
left button, dx +5, dy −3.

Rules the reference learned on the bench (2026-08-26), each one a defect if
ignored:

- **`SETUP_RETR = 0x00`, zero retransmission.** dx/dy are *relative*, hence
  not idempotent: a retry whose ACK alone was lost is applied twice by the
  dongle and the cursor jumps (measured: 903 sent, 1018 accepted with ARC=1).
  A lost frame costs one period of gesture; the next frame hides it.
- **Refund, never resend.** When `TX_DS` does not come, add the frame's
  dx/dy/wheel back into the accumulators; the next frame carries the sum
  (a sum of displacements is a displacement). Buttons are absolute: keep
  sending the current state until it is acknowledged.
- **±127 saturation carries over.** Clamp each frame to int8 and keep the
  remainder for the next one; count saturations, they size the report rate.
- **Rate: motion at 250 Hz during a gesture, silence at rest.** The reference
  ticks at 1 kHz (buttons sampled every tick, a click goes out on the tick it
  changes) and reads the sensor every 4th tick. 1 kHz motion frames saturated
  the chain (≈130 frames/s actually delivered) and moved in bursts; 125 Hz
  clipped at 40 cm/s. Send only when `dx || dy || wheel || buttons changed`,
  plus one frame at start-up so the dongle sees a first state.
- The 1 Mbps air time of a 6-byte frame + ACK is ≈ 250 µs; `rf_driver_send`
  blocks for one attempt only (ARC=0), which fits inside a 1 ms tick.

## 5. Link supervision on the dongle — what the mouse must expect

Per slot, independent of the keyboard slot:

| Silence on slot 2 | Dongle action |
|---|---|
| > **2000 ms** (`RF_REARM_SILENCE_MS`) | rewrites its PRX config for radio 2 (watchdog against a wedged chip); at most once per 2 s. Invisible to the mouse |
| > **2500 ms** (`RF_LINK_LOST_MS`) | declares the slot lost, sends **one** zeroed mouse report to the host (`RF_SAFE_RELEASE_BUTTONS`); the slot comes back on the next frame |

Consequence — and the reference does **not** handle it yet, which is a
latent bug worth fixing in the rewrite: a button **held without motion for
more than 2.5 s** (a slow drag, a long press) is released by the dongle. The
firmware-wide rule (`CLAUDE.md`, "emit on change and release on silence don't
compose") applies: *silent at rest, kept alive as long as something is held*
— while any button is down, repeat the current report at least every
`RF_STATUS_PERIOD_MS` = **1000 ms** (dx = dy = wheel = 0, same buttons). A
repeated all-zero-motion frame is idempotent, so this is safe with ARC = 0.

The dongle ignores frame types it does not expect on this slot; there is no
STATUS/battery frame for the mouse today (the `PKT_TYPE_STATUS` path is
keyboard-only). A future battery report is a contract change: add it here
first.

## 6. Test plan for a new implementation

1. **Bytes**: encode the five frames of the vectors file byte-for-byte;
   compute `set_id` and the derived address/channel for the bench dongle.
2. **Chip**: read back `CONFIG / EN_AA / EN_RXADDR / SETUP_AW / RF_CH /
   RF_SETUP / FEATURE / DYNPD` after init and compare to §1 (the reference
   logs `verify OK/FAIL` with the diff — worth copying).
3. **Air, unpaired**: dongle with `pair 1` window open → `PAIR_ACK` received,
   `set_id` persisted, `kesp_cdc.py /dev/ttyACM0 pairs` lists the mouse's MAC
   on slot 2.
4. **Air, paired**: a move produces motion on the host, three clicks map to
   the three buttons, wheel scrolls; `kesp_cdc.py rfstat` shows frames on the
   mouse slot and no re-press artefacts.
5. **Hold**: press-and-hold 5 s without moving → the button stays down on the
   host (that is §5 done right, and the reference fails it).
6. **Loss**: walk out of range with a button held → the host sees the release
   within ~2.5 s (dongle safe action).

## 7. Files this contract is derived from

`main/comm/rf/rf_packet.h` (frames), `rf_slot.h` (channel plan, timings, slot
loss), `rf_pairing.h/.c` (derivation, rendezvous), `rf_driver.c` (register
set, transmit sequence), `rf_rx_task.c` (dongle behaviour),
`main/comm/rf/mouse_relay_tx.c` and `main/app/mouse_task.c` (reference
transmitter). Locked by host tests `test_rf_channel_plan`,
`test_rf_status_cadence`, `test_rf_packet`, `test_rf_pairing`, and
`test_mouse_slot_vectors` (the bytes of §2-§4 against the encoders). The JSON
is regenerated by `scripts/gen_mouse_slot_vectors.sh` (same encoders, host
build). When one of those files changes a wire byte or a timing, this
document, the test and the vectors change in the same commit.
