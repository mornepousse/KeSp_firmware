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

## 1b. Why this device, not just the YubiKey (rationale, 2026-06-09)

The owner already has a **YubiKey 5 NFC**, which on its own covers FIDO2, OpenPGP, PIV and
CR-HMAC. So on raw capability the dongle is redundant. The deliberate reason to build OpenPGP
here anyway is **credential separation — "not all eggs in one basket":**

- **YubiKey 5 NFC = the "web / accounts" basket** (FIDO2 web 2FA, NFC for phone). Portable; you
  carry it. This stays its job — **FIDO2 is NOT built on the dongle** (redundant + the dongle's
  internal form factor can't be a portable authenticator).
- **Dongle = the "dev identity" basket** (OpenPGP: SSH auth + git commit signing). Always present
  (M.2 in the laptop), confirmed by a keystroke on the split (`K_SEC_CONFIRM`). A sedentary key
  you don't want to carry — the form factor fits.

This makes the two tokens **independent**: a lost/broken/compromised YubiKey does not also take
down SSH/git, and vice versa; no single point of failure; no single-vendor dependency (the
dongle firmware is yours/auditable). **CR-HMAC** (the old KeePassXC use case) is dropped — the
owner moved to ProtonPass, which doesn't use HMAC challenge-response; it stays in-tree behind a
future build flag, not deleted.

**Honest caveat (accepted):** the dongle is *less hardened* than the YubiKey — no secure element,
NVS keys plaintext at rest, fully reprogrammable (no Secure Boot). It defends the dev identity
well against **host malware** (non-extractable over USB + physical-touch gate), but **not**
against an attacker with physical access who desolders the flash. This matches the inherited
threat model (host-malware-only). A future eFuse-HMAC KEK ("Couche 1") can encrypt the NVS at
rest without losing reprogrammability.

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

## 6b. Phase 0 — RESULT (2026-06-09): ✅ PASS

De-risked on hardware (dongle flashed via CH340, `gpg`/`scdaemon` 2.4.9 via `nix-shell -p gnupg`,
UART0 console temporarily enabled on GPIO43/44 to read TinyUSB logs). **The #1 unknown is
resolved: scdaemon's internal CCID driver enumerates our `303a:4001` device by USB class 0x0B,
selects it as a reader, powers it on, negotiates T=1 params, and exchanges APDUs — the OpenPGP
`SELECT (AID D27600012401)` returns `0x9000`.** No VID whitelist issue, no udev rule beyond the
existing `99-local.rules`, no pcscd. The Dell's built-in Broadcom `0A5C:5843` reader coexists
(scdaemon enumerates all CCID readers; ours is reader 2).

Three real defects were found and fixed during bring-up (the descriptor/applet host-foundation
was correct; these were integration bugs):
1. **Linker dead-strips `ccid.c.obj`.** Its only export was the *weak* `usbd_app_driver_get_cb`
   override; with nothing referencing it, the linker kept the empty weak default in
   `libtinyusb.a` → 0 app drivers → `process_set_config` assert (no driver claims the CCID
   interface) → device never configured. **Fix:** `ccid_init()` called from the force-linked
   `kase_tinyusb_init()` pulls the object in so the strong symbol wins.
2. **ESP32-S3 FS DFIFO can't fit a 5th IN endpoint.** Adding CCID's 64-byte bulk-IN alongside
   CDC-notif + CDC-in + HID-kbd + HID-OTP overflows the device TX-FIFO RAM (`dfifo_alloc`
   assert in `dcd_edpt_open`). **The dongle cannot expose CDC + HID-kbd + HID-OTP + CCID at
   once.** For the de-risk, OTP-HID was dropped and CCID placed on EP4 (4 IN endpoints, fits).
   → **OPEN DESIGN DECISION (Phase 1): OTP-HID (CR-HMAC) vs CCID coexistence** — options: drop
   OTP-HID (OpenPGP supersedes it), make them mutually exclusive (build flag / runtime), or drop
   CDC when CCID is active. Supersedes spec §10 item 6.
3. **ATR was missing the mandatory TCK.** The T=1/T=15 ATR had no checksum byte →
   `update_param_by_atr failed: -1`. **Fix:** appended `TCK = XOR(T0..last) = 0xCD`.

Remaining (Phase 1, not Phase 0): the applet returns `6A88` to `GET DATA 4F` because the DO
store is empty, so `gpg --card-status` prints "no supported card application found" *after* a
successful SELECT. Populating a default AID DO (0x4F) + the essential DOs makes `--card-status`
print the card. This is Phase-1 applet work (§7), with its own host tests.

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

## 7b. Phase 1 — RESULT (2026-06-10): ✅ signing card works

Validated on hardware (dongle flashed via CH340, `gpg`/`scdaemon` 2.4.9 via `nix-shell -p gnupg`,
ESP32-S3 `303a:4001`). Full end-to-end signing pipeline working.

**What passed:**
- `gpg --card-status` shows the full card: Application ID `D276000124010304FF00<serial>`,
  type OpenPGP, version 3.4, Key attributes nistp256×3, PIN retry 3/0/3, Max PIN lengths 16/0/16,
  UIF Sign=on, Signature counter.
- `gpg ... keytocard` imports a NIST P-256 signing key (PUT DATA 0xDB extended header list);
  the card stores the private scalar + fingerprint. `gpg -K` shows `sec>` with "Card serial no."
- `gpg --clearsign` (PSO:CDS): reaches the card, prompts PW1, arms the UIF touch gate. WTX frames
  keep scdaemon waiting up to 15s. **Without touch:** returns `6985` at exactly T+15s (no
  intermediate USB timeout — WTX held the host). **With touch (`K_SEC_CONFIRM` keypress on a
  paired half):** returns a valid ECDSA-P256 signature.
- `gpg --verify` confirms Good signature. On-device KAT passes on boot (RFC 6979 §A.2.5 P-256
  vector incl. public key Ux/Uy).
- PINs (PW1/PW3) survive reboot and app-reflash (NVS); retry counters persist on every decrement
  (anti pull-the-plug brute-force). `CHANGE REFERENCE DATA` (INS 0x24) supported.

**Hardware-discovered gotchas and fixes (5):**

1. **6E/constructed-DO multi-packet bulk-IN response of exactly N×64 bytes needs a USB ZLP.**
   Without a zero-length packet to terminate the transfer, libusb/scdaemon hangs 5s waiting. Fixed
   in `ccid.c`: send a ZLP after the last full 64-byte packet.
2. **`READ PUBLIC KEY` (INS 0x47, P1=0x81) is required by `gpg keytocard` for imported keys.**
   gpg needs to read the card's public key to build the local `sec>` card-backed stub. Without this
   command, gpg silently signs with the local key — the import appears to succeed but the card is
   never used (no touch, no UIF). Fix: implement 0x47/P1=0x81 by deriving Q=d·G via mbedTLS and
   returning the uncompressed point in TLV.
3. **Any `GET DATA` returning `6A88` aborts scdaemon's LEARN chain.** If the applet returns
   "not found" for any DO queried during card-status enumeration, the LEARN aborts and CHV status
   never reaches gpg ("PIN lengths 0 0 0"). Fix: serve empty-but-valid TLV for all DOs that gpg
   queries — specifically login DO 0x5E and button DO 0x7F74.
4. **ATR requires the TCK byte.** T=1/T=15 ATR without the checksum caused
   `update_param_by_atr failed: -1` in scdaemon. Fix: TCK = XOR(T0..last) = 0xCD (also captured
   in §6b; this is a T=1 spec requirement, not a scdaemon quirk).
5. **CCID custom class driver must be force-linked.** `usbd_app_driver_get_cb` in `ccid.c` is a
   weak-symbol override; without a reference, the linker keeps the empty default from
   `libtinyusb.a` → CCID interface never claimed → `process_set_config` assert. Fix:
   `ccid_init()` called from force-linked `kase_tinyusb_init()` (also captured in §6b — the
   same fix, re-confirmed on Phase 1 firmware).

**Deferred to Phase 2:**
- Decrypt slot (cv25519/X25519, `PSO:DECIPHER`).
- Auth slot (`INTERNAL AUTHENTICATE` → SSH via `gpg-agent --enable-ssh-support`).
- On-card key generation (`GENERATE ASYMMETRIC KEY PAIR`, INS 0x47 P1=0x80).
- RSA support.

**One permanent manual gate (by design):** the physical touch cannot be automated or bypassed in
software. `sec_confirm` is armed only during a live PSO:CDS and authorized only by a real
`K_SEC_CONFIRM` keypress over NRF from a paired half. A software-only path to signing does not
exist — this is the point.

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
