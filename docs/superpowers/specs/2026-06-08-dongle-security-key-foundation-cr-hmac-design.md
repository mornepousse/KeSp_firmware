# Dongle Security Key — Foundation + Phase 1 (Challenge-Response HMAC-SHA1)

- **Date**: 2026-06-08
- **Status**: Design approved, spec under review
- **Branch**: `dongle-firmware`
- **Device**: dongle only (`CONFIG_KASE_DEVICE_ROLE_DONGLE`)
- **Repo strategy**: firmware stays in this monorepo (dongle-gated module); host
  provisioning UI lives in the existing controller repo (`KeSp_controller`). No new repo.

## 1. Goal

Turn the KaSe dongle into a **hardware security device** for the laptop it plugs
into, using its privileged position (trusted HID actor + always-on + isolated from the
host OS). This spec covers the **shared foundation** + **Phase 1: YubiKey-compatible
HMAC-SHA1 challenge-response** (works natively with KeePassXC, `pam_yubico`,
`ykchalresp`).

Phases 2 (SSH/GPG smartcard) and 3 (FIDO2/passkeys) are **out of scope** here and
sketched only as a roadmap (§10) — they reuse this foundation.

## 2. Threat model (decided)

**In scope: host malware. Out of scope: physical access to the powered laptop.**

Consequences:
- The dongle is a *"keys isolated from the host OS"* device, **not** a tamper-proof
  token. A flash dump (physical) reveals secrets — explicitly accepted.
- Therefore: **no Secure Boot, no flash encryption, no eFuse lockdown** in this phase.
  The dongle stays **100% reprogrammable** (it must evolve). Secure Boot remains a
  deferred, optional one-way door (see §8).
- Secrets live in NVS. Plaintext is acceptable under this model; a light AES wrap with
  a device-held key is applied as hygiene (and as the seam for a future eFuse-HMAC KEK,
  §8).

### What CR-HMAC actually protects (honest)
Challenge-response is **host-initiated** — the secret never leaves the device; the
response is challenge-specific. Its native protection is **"something you have" /
portability**: it protects a KeePass DB copied to *another* machine without the dongle.
A YubiKey-without-touch does **not** stop same-host malware (malware can query the
device exactly like KeePassXC).

**This is why require-keypress is mandatory here (§5.4)** — it restores user-presence
and closes the same-host-malware gap, which is precisely the in-scope threat.

## 3. Non-goals

- No physical-tamper resistance (Secure Boot / flash-enc) in this phase.
- No TOTP typer / password typer in this phase (separate parked design; shares the
  secret store, can be layered later).
- No host-software dependency for the *core* CR path — it must work with stock
  KeePassXC/`pam_yubico`. The controller app is needed only for **provisioning**.

## 4. Architecture overview

```
                      USB (to laptop)
   ┌──────────────────────────────────────────────────────┐
   │  Composite device (TinyUSB)                            │
   │   - CDC (existing)  ── provisioning (write-only) ──┐   │
   │   - HID kbd+mouse (existing)                       │   │
   │   - HID "OTP" (NEW) ── YubiKey CR protocol ────┐   │   │
   └────────────────────────────────────────────────┼───┼──┘
                                                     │   │
   ┌─────────────────────────────────────────────────────────┐
   │ DONGLE firmware (main/security/, dongle-gated)            │
   │                                                           │
   │  sec_store ── secrets blob (NVS, write-only CDC)          │
   │     ▲ provision (CDC 0xC0..)        │ read secret (local) │
   │     │                               ▼                     │
   │  cdc_sec_cmds                  cr_hmac (mbedtls HMAC-SHA1) │
   │                                     ▲                     │
   │  otp_hid (YubiKey HID proto) ───────┤ challenge/response  │
   │     │ pends response                ▼                     │
   │  sec_confirm ◄── K_SEC_CONFIRM keypress (from half, NRF)  │
   └───────────────────────────────────────────────────────────┘
                                          ▲
                              half keyboards (NRF24 uplink, existing)
```

## 5. Components

### 5.1 Secret store — `main/security/sec_store.{c,h}` (dongle-gated)

- One NVS blob in `STORAGE_NAMESPACE` ("storage"), via the existing
  `nvs_save_blob_with_total()` / `nvs_load_blob_with_total()` pattern
  (`main/.../nvs_utils.h`), modeled on `combo`/`key_stats`.
- Fixed-size slot array (start with **N = 4** CR slots; sized to grow):
  ```c
  typedef struct {
      uint8_t  type;        /* SEC_SLOT_EMPTY | SEC_SLOT_HMAC_SHA1 */
      uint8_t  flags;       /* bit0 = require_keypress (forced on, see §5.4) */
      uint8_t  secret_len;  /* HMAC key length (typ. 20) */
      uint8_t  reserved;
      char     label[16];   /* non-secret; readable via list-labels */
      uint8_t  secret[64];  /* HMAC key material */
  } sec_slot_t;             /* persisted as a fixed array sec_slot_t[N] */
  ```
- **Crypto pluggable (Couche 0):** the blob is wrapped/unwrapped through a thin
  `sec_crypto_seal()/unseal()` seam. Phase-1 implementation = identity or AES with a
  static device key (hygiene). The seam lets a future eFuse-HMAC KEK (§8) drop in
  without touching callers.
- **API:** `sec_store_load()`, `sec_store_set_slot(idx, type, label, secret, len)`,
  `sec_store_clear_slot(idx)`, `sec_store_get_secret(idx, out, &len)` **(internal,
  firmware-only — never exposed over CDC)**, `sec_store_label(idx)`.

### 5.2 CDC provisioning commands — `main/comm/cdc/cdc_sec_cmds.c` (dongle-gated)

New `KS_CMD_SEC_*` in the **free 0xC0 range**, registered in the dongle table
(`cdc_dongle_cmds.c` pattern, `ks_register_binary_commands`):

| Cmd | ID | Payload (req) | Resp | Notes |
|-----|----|--------------|------|-------|
| `KS_CMD_SEC_SET_SLOT`   | 0xC0 | `[idx:u8][type:u8][label:16][secret_len:u8][secret...]` | OK/err | **write-only** |
| `KS_CMD_SEC_CLEAR_SLOT` | 0xC1 | `[idx:u8]` | OK/err | |
| `KS_CMD_SEC_LIST`       | 0xC2 | — | `[count:u8][{idx,type,label}...]` | labels only, **never secrets** |

**Invariant I1 (audited):** no command returns secret key material. `KS_CMD_SEC_LIST`
returns labels/types only. There is deliberately **no get-secret command**.

### 5.3 YubiKey-compatible OTP HID interface — `main/security/otp_hid.{c,h}`

- Add a **second HID interface** to the composite descriptor (`main/comm/usb/usb_hid.c`,
  `usb_descriptors`/`tusb_config.h`). Budget is fine (≈4/16 endpoints used). The YubiKey
  OTP HID transports via **feature reports over EP0** (`SET_REPORT`/`GET_REPORT`), so no
  new interrupt IN endpoint is strictly required; confirm the minimal TinyUSB HID
  interface config during impl.
- Emulate the **YubiKey OTP HID frame protocol** for HMAC-SHA1 challenge-response:
  host writes a slot command frame (HMAC-SHA1 CR slot), CR engine computes the response,
  host reads it back. The exact frame layout (status flag polling, 64-byte write frame,
  20-byte response, slot command bytes, and the **"touch required" semantics**
  KeePassXC honors) MUST be pinned against the `yubikey-personalization` / KeePassXC
  sources during implementation (see §9 open items).
- `otp_hid` does **not** read secrets or compute crypto itself — it parses the frame,
  asks `sec_confirm` to gate, then calls `cr_hmac`.

### 5.4 require-keypress (MANDATORY) — `main/security/sec_confirm.{c,h}`

A challenge does **not** produce a response until the user physically presses an
authorization key on a **half** keyboard. This is the core security control under the
malware-only model.

- New keycode **`K_SEC_CONFIRM = 0x3E00`** (free slot in the advanced range; rides the
  existing `process_advanced_key` dispatch and avoids `int16_t` sign pitfalls of
  ≥0x8000). Mapped on the halves' keymaps by the user.
- Flow:
  1. `otp_hid` receives a CR challenge → calls `sec_confirm_arm(idx, challenge)` →
     state = PENDING. The HID layer reports **"touch required / busy"** to the host
     (YubiKey semantics; KeePassXC then waits and prompts the user).
  2. The user presses `K_SEC_CONFIRM` on a half → it arrives via NRF
     (`rf_rx_task` → `MATRIX_STATE` → `build_keycode_report`). Detection hook in
     `process_advanced_key`/`process_matrix_changes` (`main/input/key_processor.c`)
     calls `sec_confirm_authorize()`.
  3. `sec_confirm` releases the gate → `cr_hmac` computes the response → `otp_hid`
     returns it.
  4. **Timeout** (e.g. 15 s) with no keypress → request dropped, host sees timeout/error.
- **Invariant I2 (revised & audited):** a CR response is emitted **only** after a
  physical `K_SEC_CONFIRM` from a half within the window. No CDC command and no
  matrix-test/inject path can authorize (audit: `KS_CMD_MATRIX_TEST` streams coords and
  the engine is skipped in test mode; confirm `K_SEC_CONFIRM` cannot be injected via any
  CDC handler).
- The dongle is buttonless/captive, so the "touch" is a key on the user's own keyboard —
  natural, and unique to a keyboard dongle.

### 5.5 CR engine — `main/security/cr_hmac.{c,h}`

- `cr_hmac_sha1(secret, secret_len, challenge, chal_len, out20)` — **pure**, via
  `mbedtls_md_hmac` (HMAC-SHA1). mbedtls is linkable with no new dependency.
- **Note on SHA-1:** only SHA-1 *collision resistance* is broken (signatures/certs).
  **HMAC-SHA1 remains secure** and is the algorithm the YubiKey/KeePassXC ecosystem
  requires — compatibility mandates it.

## 6. Data flows

**Provision (host → dongle, one-time per secret):**
controller app (`KeSp_controller`) → `KS_CMD_SEC_SET_SLOT` (write-only) → `sec_store`
seals + persists to NVS.

**Challenge-response (the unlock path):**
KeePassXC → OTP HID challenge → `otp_hid` → `sec_confirm_arm` (PENDING, host told
"touch required") → user presses `K_SEC_CONFIRM` on a half → NRF → engine →
`sec_confirm_authorize` → `cr_hmac_sha1(secret, challenge)` → 20-byte response → OTP HID
→ KeePassXC unlocks.

## 7. Security invariants (must hold; covered by tests + audit)

- **I1 — secrets are write-only over CDC.** No command returns key material.
- **I2 — a CR response requires a physical `K_SEC_CONFIRM` from a half** within the
  timeout. No CDC/matrix-test/inject path can authorize.
- **I3 — secrets never typed/echoed anywhere.** CR returns an HMAC of a host challenge,
  never the secret.

## 8. Reprogrammability & crypto pluggability

- Phase 1: fully open/reflashable. `sec_crypto_seal/unseal` seam = Couche 0 (identity or
  static-key AES).
- **Couche 1 (later, still reprogrammable):** swap the seam to derive an AES KEK from the
  **eFuse-HMAC peripheral** (key the CPU can't read) → at-rest secrets become chip-bound
  / dump-resistant. Burning one eFuse key block is irreversible but does **not** enable
  Secure Boot and does **not** lock firmware updates.
- **Couche 2 (deferred one-way door):** Secure Boot v2 + flash encryption → tamper-proof,
  but signed-only updates + brick risk + signing-key custody. Out of scope; revisit once
  the firmware is mature.

## 9. Open items to validate during implementation

1. **Exact YubiKey OTP HID CR protocol** — frame layout, slot command bytes for
   HMAC-SHA1 CR, status/CRC, and the **"touch required" handshake** KeePassXC expects.
   Pin against `yubikey-personalization` + KeePassXC source. (Highest-risk unknown.)
2. **TinyUSB 2nd HID interface** — minimal descriptor/config (feature-report-only vs an
   IN endpoint), `CFG_TUD_HID` count, report-id routing alongside the existing kbd+mouse
   HID.
3. **`sec_confirm` hook point** — confirm whether `K_SEC_CONFIRM` is best intercepted in
   `process_advanced_key` or `process_matrix_changes`, and that it is consumed (not
   emitted as a keystroke).
4. **AES device key** for Couche-0 hygiene wrap — where the static key lives (and that it
   provides no real protection, only future-proofs the seam).

## 10. Roadmap (future phases, reuse this foundation)

- **Phase 2 — SSH/GPG hardware key:** emulate an OpenPGP/CCID smartcard interface; sign
  with on-device keys (RSA via the DS peripheral, or Ed25519 in software). Reuses
  `sec_store` + `sec_confirm`.
- **Phase 3 — FIDO2/passkeys:** CTAP2 over a FIDO HID interface; per-credential keys.
  Largest effort; reuses `sec_store` + `sec_confirm` (user presence already solved).

## 11. Testing (host-side, TDD — per project norm)

All pure logic gets a host test in `test/` before implementation:
- `cr_hmac_sha1` vs RFC 2202 HMAC-SHA1 vectors **and** known YubiKey CR vectors.
- OTP HID frame parse/format (round-trip a known challenge frame).
- CDC `SEC_SET_SLOT`/`CLEAR`/`LIST` parse + bounds checks + I1 (no-secret-readback).
- `sec_confirm` state machine: arm → authorize → release; arm → timeout → drop;
  authorize-without-arm = no-op; double-arm handling.
- `sec_store` seal/unseal round-trip; slot bounds.

## 12. Module layout (all dongle-gated in `main/CMakeLists.txt`)

```
main/security/
  sec_store.{c,h}      # NVS secret blob + pluggable crypto seam
  cr_hmac.{c,h}        # pure HMAC-SHA1 (mbedtls)
  otp_hid.{c,h}        # YubiKey OTP HID protocol emulation
  sec_confirm.{c,h}    # pending-CR state machine + K_SEC_CONFIRM gate
main/comm/cdc/
  cdc_sec_cmds.c       # KS_CMD_SEC_* provisioning (write-only)
```
Gated under `if(CONFIG_KASE_DEVICE_ROLE_DONGLE)` like `cdc_dongle_cmds.c`.
`K_SEC_CONFIRM` added to `main/input/key_definitions.h` (0x3E00).
