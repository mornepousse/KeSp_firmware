# Chest link v2, S3 side — confirmation bound to an instance, USB mode from the keyboard

Design of 2026-09-29, the same day as the v1 master
(`2026-09-29-chest-link-s3-master-design.md`, merged at `a11e8287`). The v1
bench proved the wire (read path: the left shows `P4 SD`), and showed two gaps:

1. **The chest is inert.** It boots in `USB_MODE_NONE` and nothing can take it
   out: the v1 protocol has no field to request a mode, and the chest's console
   has no power in production (`BOARD_CONSOLE_ACTIONS 0`). The design had
   carried *presence* ("presence comes from the keyboard") and nobody carried
   *selection*. Without a mode, no operation is ever armed, so the confirmation
   path cannot even be exercised.
2. **The confirm retry is keyed on the op code, not on the instance.** A lost
   first write, the op expiring, the host arming a new op with the same code
   within 200 ms: the retry confirms an operation the owner never saw (same
   code, another account).

Protocol v2 (published by Niphar_chest in `docs/LINK_CONTRACT.md` v2 — the
contract is authoritative; the values below are those announced on 2026-09-29
and are re-checked against the published contract before implementation)
closes both.

## 1. Decisions (Mae, 2026-09-29)

- **`K_SEC_CONFIRM` counts only from the LEFT half**, enforced by the firmware,
  not by a usage note: a press coming from a remote column (the right half,
  over the unauthenticated inter-half radio) is ignored — neither the chest nor
  the local `sec_confirm` gate receives it. A note gets lost; the next person
  moving the key for ergonomics would not know what they undo.
- **One key cycles the chest's USB mode**: `K_CHEST_NEXT`, none → storage →
  pgp → otp → fido → oath → none.
- **No physical confirmation to change mode** (Mae and the chest session):
  pressing a key on her own keyboard IS the gesture. Confirmation stays for
  operations (a code out, a delete, a replace).

## 2. Register map v2

Settled with the chest on 2026-09-29 (option (b): the CRC covers everything
before it, contiguously):

| offset | owner | content |
|---|---|---|
| `0x00-0x0B` | chest | unchanged (magic, version **2**, state, pending op, counter) |
| **`0x0C`** | chest | **instance number**, incremented at every arming |
| **`0x0D`** | chest | **ACTIVE USB mode** (same wire values as `0x12`) — added by the chest before any v2 was flashed, version stays 2 |
| **`0x0E-0x0F`** | chest | **CRC16 over `0x00..0x0D`** (little-endian) |
| `0x10` | master | `0x5A` — confirmation |
| **`0x11`** | master | **echo of the instance** read at `0x0C` |
| **`0x12`** | master | **requested USB mode** |
| `0x13` | master | reserved, zero |

The CRC moves (so the version must change): a discontinuous span would force
every implementation to reproduce the same gap, and an uncovered instance would
let one flipped bit cause a refused confirmation nobody could explain. The
chest owns `0x00..0x0F` whole, the master `0x10..0x13` — no shared word.

**Confirmation**: the chest accepts only `0x10 == 0x5A` AND `0x11 == the
instance currently armed`; otherwise it ignores it silently and the counter
does not move.

**Mode wire values** (fixed by the contract, independent of the chest's
internal enum): `0x00` none, `0x01` storage (microSD as MSC), `0x02` pgp
(OpenPGP card, CCID), `0x03` otp (CR-HMAC, HID), `0x04` fido (U2F/CTAP-HID),
`0x05` oath (TOTP, CCID). The chest applies on CHANGE of the value; an unknown
value is refused (it stays in its current mode and logs it).

## 3. Master behaviour

- **Version**: `CHEST_PROTO_VERSION 2`. A v1 chest is BAD_VERSION (refused,
  logged once, `P4?` on screen).
- **Parse** adds `instance` (`0x0C`) and `active_mode` (`0x0D`) to `chest_status_t`; the CRC is read at
  `0x0E` and computed over `0x00..0x0D` — per the contract's parse order and
  vectors.
- **Confirmation write**: ONE WRBUF of 2 bytes at `0x10`: `{0x5A, instance}`,
  the instance taken from the SAME block that showed the op the owner saw
  (the press already carries the op code, `chest_press_matches`); the retry
  rule keeps its shape (one retry after 200 ms, never a third) and now also
  stops if the instance changed. A late retry carries the old instance and
  cannot confirm anything else.
- **Mode write**: ONE WRBUF of 1 byte at `0x12`: the wanted mode. Never in the
  same write as the confirmation byte — a mode change must never touch `0x10`.
  The master keeps `wanted_mode`; every read returns the whole 20-byte block,
  master word included, so when `regs[0x12] != wanted_mode` (chest rebooted,
  its shared buffer back to zero) the master rewrites it. Self-healing, no
  extra transaction at rest.
- **`K_CHEST_NEXT`** (new keycode `0x3E01`, in the `K_SEC_*` range): a new
  press cycles `wanted_mode` to the next value (pure `chest_mode_next`), only
  while the chest is present and READY; otherwise ignored. It goes through the
  same lock-free gate pattern as the confirm (key_processor sets, the link task
  writes) — but it is NOT a security gesture and may sit on either half.
- **Presence lost** (USB unplugged): `wanted_mode` back to none.
- **The chest's reclaim of `0x10` rewrites the master's whole word** (driver
  read-modify-write): a `0x12` write landing in those cycles can be overwritten
  stale, silently (no counter for modes). The "read back `0x12`, rewrite if it
  differs" loop closes it; `0x0D` makes it observable.
- **The instance is a counter inside the chest's `sec_confirm`**, incremented
  under lock at arming and published in the same call as the op (not guessed
  from transitions, which a 20 ms poll cannot see twice) — the master reads
  `0x0C` from the same block as the op the owner saw.
- **The chest applies a mode on change of the last APPLIED value** (initialized
  to `0x00` at its boot), not of what it reads — so after a chest reboot the
  master's rewrite of `0x12` is seen as `0 → mode` and applied (confirmed by
  the chest, 2026-09-29).
- **Known limit, accepted by the contract**: `0x12` has no physical
  confirmation (`storage` exposes the microSD, `pgp` loads the private keys in
  RAM). Accepted because writing it requires being the bus master, i.e. being
  inside the left half. The master writes `0x12` only when the wanted mode
  differs from what it reads back — never in a loop; a mode change comes only
  from a new `K_CHEST_NEXT` press or from the self-heal after a chest reboot.

## 4. Left-only confirm

A pure predicate `sec_confirm_from_local(col)`: true when the column is one of
the board's local columns (`col < MATRIX_COLS` on a board whose keymap spans
remote columns, `KEYMAP_COLS > MATRIX_COLS`; always true otherwise). The
`K_SEC_CONFIRM` branch of `key_processor.c` requires it before anything else.
Host tests: a K_SEC_CONFIRM at a remote column with a chest op pending queues
nothing and does not authorize the local gate; the same at a local column
works as before.

## 5. Screen

The third chest line under the logo shows the chest's **ACTIVE** mode read at
`0x0D` — what the host really sees — not the mode the keyboard asked for:
`MSC`, `PGP`, `OTP`, `FIDO`, `OATH` in **upper case when `active == wanted`**;
while they differ (a switch in progress, refused or retrying) it shows the
WANTED mode in **lower case** (`pgp`…) so the owner sees that her request has
not taken yet. Nothing when both are none. 4 UNSCII characters max (35 px).
Pure, tested (`memlcd_lignes_coffre` gains active and wanted modes; both enter
the model diff). The chest session found why this matters: Mae once had a
chest stuck in `none` with nothing saying so — a screen showing the requested
mode would have repeated that silence.

A switch that has not taken after several reads (`0x0D != 0x12`) is visible on
screen (lower case stays); no automatic action beyond the `0x12` self-heal.

## 6. Contract and tests

- The v2 vectors of the contract, copied verbatim into `test_chest_proto`,
  replacing V1..V9; the CRC check value stays.
- New pure units, tests first: parse of the instance; `chest_mode_next`; the
  mode self-heal decision (`chest_mode_needs_write(regs, wanted)`); the confirm
  rule stopping on an instance change; `sec_confirm_from_local`.
- The tripwire brick keeps `chest_gate_press()` in key_processor.c only; the
  mode request (`chest_gate_mode_next()`) is not security-bound and is not
  added to it.
- Bench (`[smoke:Chest link]` updated): `K_CHEST_NEXT` to PGP → `gpg
  --card-status` sees the card; `echo t | gpg --sign` → `SIGN / OK ?`, a press
  signs, no press → `6985`; the same key placed on the right half does NOT
  confirm; unplug → mode back to none, sleep current unchanged.

## 7. Out of scope

The framed data channel (options beyond the mode, ISO selection); the binary
guard for the single caller (after this bench); the USB enumeration delay of
the left (a separate power item).
