# Dongle OpenPGP Smartcard — Design (Phase 2 of the security key)

- **Date**: 2026-06-09
- **Status**: Design (from the 2026-06-09 CCID/OpenPGP feasibility spike), under review
- **Device**: dongle only (`CONFIG_KASE_DEVICE_ROLE_DONGLE`)
- **Scale (honest)**: this is **10–20× the CR-HMAC feature**. Sign-only prototype ≈ 4–6 weeks;
  full card ≈ 3–5 months. Decomposed below into Phase 0 / 1 / 2; each gets its own plan and
  a dedicated implementation session.

## 1. Goal

Make the dongle a **GnuPG-compatible OpenPGP smartcard**: `gpg` can **sign** (git commits,
files), **authenticate over SSH** (via `gpg-agent --enable-ssh-support`), and **decrypt**,
using keys that live on the dongle and never leave it, with a **physical touch-to-approve**
(reusing `K_SEC_CONFIRM` / `sec_confirm`).

Reuses the Plan-1/2 foundation: `sec_store` (NVS key material), `sec_confirm` (keypress gate),
the TinyUSB composite device, and the dongle's USB identity (`303a:4001`).

## 2. Threat model (inherited from Plan 1/2)

Host-malware-only, NOT physical access. Stays 100% reprogrammable (no Secure Boot). Private
keys stored in NVS (plaintext at rest acceptable in this phase; the future eFuse-HMAC KEK —
"Couche 1" — can encrypt at rest without losing reprogrammability). Operations gated by a
physical keypress (OpenPGP UIF → `sec_confirm`). The OpenPGP PINs (PW1/PW3) add the
"something you know" the spec requires.

## 3. Why this is tractable (spike findings that de-risk it)

- 🟢 **No VID whitelist problem** (unlike the YubiKey HID path). GnuPG's **scdaemon internal
  CCID driver** (default in 2.3+) opens devices by **USB class 0x0B**, regardless of VID — it
  only keeps a VID table for *quirks*. So our `303a:4001` works with stock `gpg` + a host
  **udev rule** for access. We target scdaemon-internal and **bypass pcscd/libccid** (whose
  `Info.plist` would otherwise need an entry).
- 🟢 **Touch-to-approve maps cleanly**: OpenPGP card spec 3.x has **UIF Data Objects** (D6 sig,
  D7 dec, D8 auth); GnuPG 2.3+ honors them with a "touch your key" prompt and waits. The applet
  calls `sec_confirm` before the crypto op, returns `0x6985` on timeout. No host changes.
- 🟢 **Reference exists**: **Gnuk** (`src/openpgp.c` for the applet, `src/usb-ccid.c` for CCID
  framing) is the best C reference — but it is coupled to Chopstx (its STM32 RTOS/USB); we port
  the applet + CCID-framing logic and replace the transport/RTOS with TinyUSB + FreeRTOS.

## 4. Architecture

```
gpg / scdaemon (internal CCID driver, class 0x0B) ── USB bulk ──► dongle
                                                                    │
  ┌─────────────────────────────────────────────────────────────────────┐
  │ ccid.c   — TinyUSB custom class (usbd_app_driver_get_cb): CCID class  │
  │            descriptor + bulk IN/OUT framing. Extracts APDUs from      │
  │            PC_to_RDR_XfrBlock, wraps responses in RDR_to_PC_DataBlock.│
  │                              │ APDU bytes ▲ response bytes            │
  │ openpgp_card.c — ISO 7816-4 APDU dispatch + OpenPGP applet:           │
  │   SELECT / GET DATA / VERIFY(PW1,PW3) / PSO:CDS / (P2: DECIPHER,      │
  │   INTERNAL AUTH, GENERATE KEY) / PUT DATA. DO store. PIN state.       │
  │   UIF gate ──► sec_confirm (physical keypress)                       │
  │                              │                                        │
  │ openpgp_crypto.c — sign/decrypt via mbedtls (ed25519/P-256/RSA)       │
  │ sec_store — key material (NVS)                                        │
  └─────────────────────────────────────────────────────────────────────┘
```

**Units (all dongle-gated):**
- `main/security/ccid.{c,h}` — CCID transport. One responsibility: USB bulk ↔ APDU bytes.
- `main/security/openpgp_card.{c,h}` — the applet: APDU parse/dispatch, DO storage, PIN state,
  UIF gating. The bulk of the work; keep DO store + PIN + dispatch as separable internals.
- `main/security/openpgp_crypto.{c,h}` — thin crypto wrappers (sign/decrypt per algorithm).
- Reuse `sec_store` (extend for key blobs) + `sec_confirm` (UIF) + `usb_hid.c`/descriptors
  (add the CCID interface to the composite).

## 5. Host integration

- Default path: **scdaemon internal CCID driver** (no pcscd). `gpg --card-status` enumerates it.
- Host setup: a **udev rule** granting access to `303a:4001` (analogous to the hidraw rule;
  on NixOS `services.udev.extraRules` + `systemctl restart systemd-udevd`).
- SSH: `gpg-agent --enable-ssh-support`; the Auth key (Phase 2) is the SSH identity.

## 6. Phase 0 — de-risk CCID enumeration (~1 day, do FIRST)

The #1 unknown: does scdaemon accept our class-0x0B device at VID 0x303a? Smallest possible
firmware to answer it:
- Add a minimal **CCID interface** to the composite: class 0x0B + the 54-byte CCID class
  descriptor (correct `dwFeatures`/`bMaxCCIDBusySlots`/clock/data-rate — cross-check vs USB
  CCID Rev 1.1 §4 and a real YubiKey descriptor) + bulk IN/OUT.
- Implement just enough CCID messages: `PC_to_RDR_IccPowerOn` → return a fixed **ATR**;
  `PC_to_RDR_GetSlotStatus` → `RDR_to_PC_SlotStatus` (card present); `PC_to_RDR_XfrBlock` →
  reply to `SELECT (AID D27600012401)` with `0x9000` + a stub AID DO.
- **Success criterion**: `gpg --card-status` (or `scdaemon` debug log) shows the reader and
  selects the applet — proving scdaemon talks to our VID via class. If it needs a udev rule,
  document it; if it needs pcscd Info.plist, fall back to forcing the internal driver.
- This is the start of `ccid.c` + the SELECT path of `openpgp_card.c`.

## 7. Phase 1 — minimal viable signing card (≈4–6 weeks)

- **CCID transport** (`ccid.c`): full bulk framing — IccPowerOn/Off, GetSlotStatus, XfrBlock,
  `bSeq` matching, `bStatus`/`bError` reporting. Short-APDU exchange (no T=1). ⚠️ **Extended-
  length APDUs** likely needed (GnuPG advertises extended length; even ECC public-key reads can
  exceed 256B) — declare extended capabilities and handle `Lc`/`Le` extended forms.
- **Applet** (`openpgp_card.c`): `SELECT`, `GET DATA` (essential DOs: AID 4F, hist bytes,
  extended capabilities/length, algo attrs C1/C2/C3, fingerprints C5, key info, UIF D6/D7/D8,
  cardholder/url/login), `VERIFY` PW1(0x82)/PW3(0x83) + `CHANGE REFERENCE DATA`, **`PSO:COMPUTE
  DIGITAL SIGNATURE`** with the **Sig key slot only**, and `PUT DATA` to **import** the key +
  set fingerprints/algo attrs. (No on-device `GENERATE KEY` yet — keys imported from host.)
- **Algorithm**: prefer **ed25519** (modern GnuPG default, 32-byte key fits `sec_store`) **iff**
  ESP-IDF mbedTLS exposes Ed25519 sign (OPEN ITEM §10) — else **NIST P-256 ECDSA** (HW-accel,
  confirmed). RSA last (bigger keys need a larger key blob than `sec_store`'s 64-byte secret).
- **PIN**: PW1/PW3 state + retry counters in the applet (separate from `sec_confirm`).
- **UIF / touch**: if UIF-for-sig (D6) enabled, `PSO:CDS` calls `sec_confirm_arm` + waits for
  `K_SEC_CONFIRM`; `0x6985` on timeout. Reuses the exact gate validated for CR-HMAC.
- **Key store**: extend `sec_store` (or a parallel `pgp_store`) to hold a Sig-key blob
  (algo id + private scalar + fingerprint). ECC ≤64B fits the current slot; document RSA later.

**End state**: `gpg --card-status` shows the card; `gpg --sign` (and git commit signing) work,
prompting a touch; the private key never leaves the dongle.

## 8. Phase 2 — complete the card (roadmap, ≈3–4 weeks)

Decryption slot (cv25519 / X25519 ECDH `PSO:DECIPHER`), Authentication slot (`INTERNAL
AUTHENTICATE` → SSH via gpg-agent), on-device **`GENERATE ASYMMETRIC KEY PAIR`**, the full DO
set, and RSA support (with the larger key blob). Each reuses the Phase-1 CCID + applet skeleton.

## 9. Testing (TDD, host-side where pure)

Host-testable pure logic (the bulk of the bug surface), with the existing harness:
- **CCID message framing**: parse a `PC_to_RDR_XfrBlock` header, extract the APDU; build a
  `RDR_to_PC_DataBlock`; `bSeq`/`bStatus`/`bError` fields; chained/oversized handling.
- **ISO 7816-4 APDU parse**: CLA/INS/P1/P2, short + extended `Lc`/`Le`.
- **DO get/put encoding** + the DO store.
- **Applet state machine**: PIN verify state + retry counter, command routing, UIF gating
  (arm→authorize→respond / timeout→6985) — `sec_confirm` injected like the otp_proto hooks.
On hardware: the crypto (sign/verify against `gpg`), USB enumeration, end-to-end `gpg --sign`.

## 10. Open items to resolve (before/early in implementation)

1. **Ed25519 in ESP-IDF mbedTLS** — confirm sign support in the 5.5 fork; if absent, bundle
   TweetNaCl/libsodium Ed25519 (~1.5k LOC). Determines Phase-1 algorithm choice.
2. **CCID class descriptor correctness** — scdaemon is finicky; validate `dwFeatures` etc. vs
   USB CCID Rev 1.1 + a real YubiKey descriptor (Phase 0 oracle).
3. **scdaemon accepts VID 0x303a via class** — confirm in Phase 0 (high confidence, unproven).
4. **Extended-length APDU** support — likely required by GnuPG; design `ccid.c`/parse for it.
5. **Key-blob storage for RSA** — `sec_store`'s 64-byte secret fits ECC, not RSA; size a larger
   blob (or `pgp_store`) when RSA lands (Phase 2).
6. **USB endpoint budget** — CCID needs bulk IN + bulk OUT (+ optional interrupt). Confirm it
   fits alongside CDC + HID(kbd/mouse) + HID(OTP) on ESP32-S3 FS.

## 11. Module layout (dongle-gated)

```
main/security/
  ccid.{c,h}            # TinyUSB CCID custom class (bulk ↔ APDU)
  openpgp_card.{c,h}    # applet: APDU dispatch, DOs, PIN, UIF gate
  openpgp_crypto.{c,h}  # sign/decrypt via mbedtls (+ Ed25519 lib if needed)
  (reuse sec_store, sec_confirm)
main/comm/usb/usb_hid.c # add CCID interface to the composite descriptor
```

## 12. References (from the spike)

USB CCID Rev 1.1 (usb.org); OpenPGP Application 3.4.1 (gnupg.org/ftp/specs); GnuPG
scdaemon manual + `scd/app-openpgp.c` + `scd/ccid-driver.c`; Gnuk (`src/openpgp.c`,
`src/usb-ccid.c`); SmartPGP; Nitrokey opcard-rs; ESP-IDF mbedTLS docs + EdDSA issue
mbedtls#2452 / PR#5800; ESP32-S3 DS peripheral (NOT usable — eFuse OTP); GnuPG UIF T4158.
```
