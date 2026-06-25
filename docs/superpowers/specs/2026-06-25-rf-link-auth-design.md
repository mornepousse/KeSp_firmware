# RF Link Authentication (NRF24 half ↔ dongle) — Design

**Goal:** Authenticate the NRF24 link so the dongle accepts input only from the
genuine paired halves. Closes the over-the-air attacks found in the 2026-06-25
pentest: arbitrary keystroke injection (MouseJack-class) and forgery/replay of
`K_SEC_CONFIRM` (remote OpenPGP touch-gate bypass). See
`docs/superpowers/specs/` pentest notes and the RF security memory.

**Non-goals:** confidentiality of keystrokes (not encrypted — only authenticated;
a sniffer learns what is typed but cannot inject/forge). ESP-NOW channel hardening
(separate, low severity). Pairing-packet authentication (pairing bootstraps trust;
see §6).

## Threat model

Over-the-air attacker in radio range, knows the (publicly derivable) NRF address.
Can sniff and transmit arbitrary NRF frames. Must NOT be able to: inject keys,
inject mouse movement, force `K_SEC_CONFIRM`, or replay captured frames.

## Architecture

### 1. Shared key (per set)

- A 32-byte random key, **one per keyboard set**, generated once at setup.
- Provisioned over USB into all three devices; **never transmitted over RF**:
  - **Dongle:** a CDC binary command (`KS_CMD_RF_KEY_SET`) writes it to the
    dongle's (now encrypted) NVS, namespace `storage`, key `rf_link_key`.
  - **Halves:** the half firmware currently exposes only CDC stubs
    (`cdc_half_stubs.c`). Add a **minimal provisioning CDC** on the half: when
    USB-connected it accepts the same `KS_CMD_RF_KEY_SET` and stores the key in
    its NVS. This is the only new transport surface on the half.
- A setup script (`scripts/kase-rf-provision.sh`) generates the key and pushes
  it to the dongle + each half in turn. Re-runnable; idempotent per device.
- Storage note: half NVS is plaintext (no flash-enc on halves) → the key is
  readable by someone with physical possession of a half. Accepted: an attacker
  holding your half already controls a trusted endpoint. Dongle key is in
  encrypted NVS.

### 2. Authenticated packet format

Input-bearing packets — `PKT_TYPE_KEY` (0x1), `PKT_TYPE_HEARTBEAT` (0x2),
`PKT_TYPE_TRACKPAD` (0x3) — gain an authentication trailer appended after the
existing body:

```
[ existing body ][ counter:3 (LE) ][ tag:8 ]
tag = HMAC_SHA1( key, session_nonce ‖ byte0 ‖ body ‖ counter )[0..7]  (trunc 8 B)
```

- `session_nonce` (4 B) is NOT transmitted in the data packet — both sides hold
  it from the link-up handshake (§3). Only `counter` + `tag` ride along.
- Sizes after the trailer (NRF24 max payload 32 B): KEY 3→14, HEARTBEAT 9→20,
  TRACKPAD 9→20. All fit.
- `byte0` (type+flags) is covered by the MAC so an attacker cannot change a
  release into a press or flip the type.
- NRF24 must use a payload length that carries the trailer. Dynamic payload
  length (DPL) is already feasible; if fixed-length is in use, bump it to ≥20.

### 3. Anti-replay (session nonce + counter)

Replaces the persisted-epoch idea from the verbal design — a session nonce is
cleaner (no NVS counter persistence, and a cold dongle reboot cannot be exploited).

- **Link-up handshake.** When the link (re)establishes, the dongle generates a
  fresh random `session_nonce` (4 B, `esp_fill_random`) and delivers it to the
  half in an **authenticated** control packet `PKT_TYPE_AUTH_INIT` (0x4):
  `[type][nonce:4][tag:8]`, `tag = HMAC_SHA1(key, "AINIT" ‖ nonce)[0..7]`. The
  half verifies the tag before adopting the nonce.
- **Per-session counter.** The half keeps a `counter` (uint24, RAM) that resets
  to 0 each new session and increments per authenticated data packet. The dongle
  tracks the last accepted counter per half; accepts only strictly-increasing
  (small forward window for NRF auto-retransmit/reorder). Equal/lower → drop.
- **Why this is replay-proof across reboots:**
  - A capture from a *previous* session carries an old `session_nonce` baked into
    its tag → fails verification under the current nonce. Dead on replay.
  - Within a session, the monotonic counter rejects duplicates.
  - Dongle cold reboot → new link-up → new nonce → every prior capture is dead.
  - Half reboot → re-handshake → new nonce, counter from 0, consistent.
- **DoS note:** replaying an old `AUTH_INIT` to the half feeds it a stale nonce;
  the dongle still uses its current nonce → data tags mismatch → link re-inits.
  An annoyance, not an injection. (Bounded: the dongle re-issues AUTH_INIT on a
  run of auth failures.)

### 4. Enforcement

- `rf_rx_task` verifies the trailer before applying any input. A packet with a
  bad tag or a non-advancing `(epoch, counter)` is dropped (counted in
  `pkt_dup`/a new `pkt_auth_fail` stat) and never reaches `MATRIX_STATE` /
  `sec_confirm`.
- If no `rf_link_key` is provisioned yet (fresh device / migration), the dongle
  refuses authenticated processing and logs a clear warning — input is gated
  until provisioning. (A keyboard with no provisioned key does not type — this
  is the intended fail-closed behavior; the setup script provisions before use.)

### 5. Components / files

- `main/security/rf_auth.c` + `.h` — **pure logic, host-tested**: `rf_auth_tag()`
  (compute/verify truncated HMAC-SHA1 over the framed bytes), `rf_auth_check()`
  (tag + monotonic `(epoch,counter)` gate), `rf_auth_seal()` (TX side). No
  ESP-IDF deps (HMAC via `cr_hmac.c`, already host-buildable).
- `main/comm/rf/rf_packet.h` — extend encoders/decoders to append/parse the
  trailer; bump max packet size.
- `main/comm/rf/rf_rx_task.c` — call `rf_auth_check()` in `drain_radio()` before
  `hb_apply_key` / matrix / trackpad application.
- `main/comm/rf/half_scan_task.c` (+ heartbeat/trackpad TX) — call
  `rf_auth_seal()` before `rf_driver` TX; manage the half's epoch/counter.
- Half CDC provisioning: un-stub a minimal CDC in `cdc_half_stubs.c` (or a new
  `cdc_half_provision.c`) handling `KS_CMD_RF_KEY_SET`.
- Dongle: `KS_CMD_RF_KEY_SET` handler in `cdc_dongle_cmds.c` / `cdc_sec_cmds.c`.
- `scripts/kase-rf-provision.sh` — generate + push the key to all three.
- NVS key: `rf_link_key` (32 B) in namespace `storage` (dongle + each half). No
  counter/epoch persistence — the session nonce (§3) makes it unnecessary.
- `PKT_TYPE_AUTH_INIT` (0x4) handshake: dongle TX on link-up (in `rf_rx_task` /
  the dongle's heartbeat path), half RX + adopt nonce (in `half_scan_task`).

### 6. Pairing & flag day

- Pairing packets (`PKT_PAIR_REQ`/`ACK`) stay unauthenticated — they only
  establish set_id/slot, grant no input capability, and predate key provisioning.
  A hostile pair-req can still DoS a pairing slot (existing finding, out of scope).
- **Flag day:** all three devices must run the new firmware AND share the same
  provisioned key. A mixed set (old half + new dongle) fails closed (no key/format
  mismatch → dropped). The setup script + a firmware-version check make this
  explicit.

## Testing

- Host tests (`test/test_rf_auth.c`, parallel-safe, like `test_heartbeat.c`):
  - tag correctness (known key/nonce/body → known tag), tamper rejection (flip
    any body/counter/nonce byte → verify fails),
  - monotonic gate: accept increasing counter, reject equal/lower, forward window,
  - session-nonce: a tag valid under nonce A fails under nonce B (replay across
    sessions), AUTH_INIT tag verify,
  - seal→check round-trip.
- HW validation (later, with the flag-day flash): provision all three, confirm
  normal typing works; confirm a replayed captured frame (counter ≤ live) is
  dropped; confirm an unprovisioned half fails closed.

## Risks / open items

- **Half CDC**: the only genuinely new transport. Keep it minimal and
  provisioning-only (no general command surface) to avoid widening the half's
  attack surface.
- **NRF payload length**: confirm DPL vs fixed; ensure ACK payloads unaffected.
- **Counter persistence vs wear**: epoch is one write/boot; counter never
  persisted. Acceptable.
- **Latency**: HMAC-SHA1 per packet is sub-millisecond on ESP32; negligible on
  the scan→HID path and on half battery.
