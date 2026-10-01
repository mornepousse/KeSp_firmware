# Request to the chest — a cancel for a pending confirmation (v3.1)

From: KeSp (left half, S3 master), 2026-09-30. For: Niphar_chest (`docs/LINK_CONTRACT.md`).

**Status: answered and implemented (2026-10-01).** The chest froze `0xC5` at
`0x38` + echo of the armed instance at `0x39` — contract §5 "Cancelling a
prompt", vectors V17 (cancel), V18 (stale echo, ignored), V19 (the block
after: op 0, label empty, instance and `0x11` unchanged), Niphar_chest
`3da17cc`; still protocol version 3. KeSp side on branch `oath-type-code`:
`chest_cancel_*` (chest_proto), `chest_gate_oath_key` (PREV/NEXT cancel during
a prompt, no cursor move), `write_cancel` (chest_link). Bench: smoke item 13.

## Why

Bench, 2026-09-30 (Mae): after `K_OATH_CODE` the full-screen prompt (op + the
chest's label + PRESS) cannot be left without waiting for the chest's
`SEC_CONFIRM_TIMEOUT_MS` (15 s). The keyboard must NOT hide the prompt on its
own: the operation stays armed in the chest, and a later `K_SEC_CONFIRM` would
confirm something no longer on screen. The only honest exit is a real cancel,
decided by the chest.

## Proposal

A master write that can only REFUSE, never authorize:

- **Where:** the confirm word, `0x38` (value) + `0x39` (instance echo) — the
  same WRBUF the keyboard already uses for `0x5A`.
- **Value:** `0xC5` ("cancel"; any value ≠ `0x5A` that the chest reserves is
  fine — please pick and freeze it).
- **Rule:** when the chest reads `0xC5` with an echo equal to the armed
  instance, it drops the pending operation exactly as a timeout does
  (`sec_confirm` → IDLE, pending op → 0, NO segment, `0x11` unchanged), and the
  host gets the same refusal as on timeout (e.g. `6985` for OpenPGP/OATH).
  Echo ≠ armed instance → ignored (a stale cancel must not kill a newer
  prompt). The chest reclaims `0x38` as it does after `0x5A`.
- **Confirm count (`0x08`-`0x0B`):** unchanged by a cancel (it counts relayed
  confirmations) — or a separate cancel counter if you want the bench to see
  it; your call.
- **Vectors:** one block before/after a cancel (pending op 7 → 0, `0x11`
  unchanged), and one stale-echo cancel ignored — so both sides pin the same
  bytes, as for V1-V16.

## What the keyboard will do

- Keys: during a prompt, `K_OATH_PREV` / `K_OATH_NEXT` (already "leave"
  keys: they hide a shown code) send the cancel for the displayed instance,
  once per new press. From either half — a cancel only refuses.
- The prompt disappears when the chest clears the pending op (the existing rule
  "op → 0 without `0x11` moving = dead request"), not optimistically.
- Nothing changes for `K_SEC_CONFIRM` (left half only, `0x5A` + echo).

Please answer with the frozen value, the contract section and the vectors
commit; KeSp implements its side against them.
