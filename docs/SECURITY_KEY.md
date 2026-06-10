# KaSe Dongle — Hardware Security Key

The dongle supports two security interfaces, selected at build time. They are **mutually
exclusive**: the ESP32-S3 Full-Speed USB controller has a 4-IN-endpoint budget; CCID
bulk-IN and OTP-HID-IN together overflow it.

| Build flag | Interface | Status |
|---|---|---|
| `CONFIG_KASE_SEC_OPENPGP` *(default)* | OpenPGP smartcard — CCID (§2 below) | ✅ validated |
| `CONFIG_KASE_SEC_OTP_HID` | CR-HMAC / YubiKey-compat OTP HID (§1 below) | built, pending HW validation |
| `CONFIG_KASE_SEC_NONE` | No security interface | — |

---

## 1. CR-HMAC / OTP-HID — KeePassXC challenge-response

The dongle can act as a **YubiKey-compatible HMAC-SHA1 challenge-response token**,
gated by a **physical keypress** on a keyboard half. This unlocks, for example, a
KeePassXC database configured with a challenge-response key.

- **Threat model:** host malware only (NOT physical access). Secrets live on the dongle,
  isolated from the host OS. See "Security model" below.
- **Status:** firmware built end-to-end (Plan 2). Requires **hardware validation** + a
  small **KeePassXC VID patch** before it works with stock clients (both below).
- Design: `docs/superpowers/specs/2026-06-08-dongle-security-key-foundation-cr-hmac-design.md`
  and plans `2026-06-08-dongle-security-foundation-plan.md` / `2026-06-09-dongle-cr-hmac-otp-hid-plan.md`.

---

## How it works

```
KeePassXC ──challenge──► dongle OTP HID (feature reports, EP0)
                           └► otp_proto: reassemble 70-byte frame, check CRC-16, read slot
                              └► sec_confirm: ARM, hold "touch required"
   you press K_SEC_CONFIRM on a half ──NRF──► dongle engine ──► sec_confirm_authorize()
                              └► cr_hmac_sha1(slot secret, challenge) ──► 20-byte response
KeePassXC ◄──response── dongle   → database unlocks
```

- The secret **never leaves the dongle**; the response is an HMAC of KeePassXC's challenge.
- No response is produced until you **physically press `K_SEC_CONFIRM`** (this is the
  YubiKey "require-touch" behaviour — KeePassXC waits up to 256 s with a "Touch" prompt).

### Slot mapping
The dongle stores up to **4 secret slots** (idx 0–3). The YubiKey challenge-response
"slots" map to the first two:

| KeePassXC slot | YubiKey cmd | Dongle store slot (kase-sec `--slot`) |
|----------------|-------------|---------------------------------------|
| Slot 1         | `0x30` (HMAC1) | **0** |
| Slot 2         | `0x38` (HMAC2) | **1** |

So: provision dongle slot **0** → select **Slot 1** in KeePassXC; dongle slot **1** → **Slot 2**.

---

## 1. Provision a secret (host → dongle)

Use **`kase-sec`** (separate Rust repo: <https://gitlab.com/harrael/kase-sec>). It speaks
the dongle's CDC binary protocol (`KS_CMD_SEC_*` 0xC0–0xC2, see `docs/CDC_BINARY_PROTOCOL.md`).
Secrets are **write-only** — there is no read-back command.

```bash
# Build
git clone git@gitlab.com:harrael/kase-sec.git && cd kase-sec && cargo build --release

# Find the dongle's CDC port (the one whose FEATURES include the dongle role) — e.g. /dev/ttyACM1
# Provision a 20-byte HMAC-SHA1 secret into dongle slot 0 (→ KeePassXC Slot 1):
kase-sec --port /dev/ttyACM1 provision --slot 0 --label keepass --secret 303132333435363738393a3b3c3d3e3f40414243 --format hex
#   or from a base32 seed:
kase-sec --port /dev/ttyACM1 provision --slot 0 --label keepass --secret JBSWY3DPEHPK3PXP... --format base32

kase-sec --port /dev/ttyACM1 list            # shows idx / type / label (never the secret)
kase-sec --port /dev/ttyACM1 clear --slot 0  # wipe a slot
```

Map a `K_SEC_CONFIRM` (keycode `0x3E00`) onto a key of one of your halves (via the
controller / keymap) — that is the physical authorize key.

---

## 2. Patch KeePassXC to recognise the dongle (one-time)

Stock KeePassXC and `ykpers` **filter by Yubico's USB Vendor ID `0x1050`**. The KaSe dongle
uses VID `0x303a` (Espressif) / PID `0x4001`, so it must be added to KeePassXC's whitelist —
a small change, with upstream precedent (OnlyKey was added the same way, commit `e4326fb`).
We do **not** spoof Yubico's VID (USB-IF / trust).

KeePassXC keeps two arrays in `src/keys/drivers/YubiKeyInterfaceUSB.cpp`:
`vids[] = {YUBICO_VID, ONLYKEY_VID}` and a `pids[] = {YUBIKEY_PID, …, ONLYKEY_PID}`,
iterated by `yk_open_key_vid_pid(vids, …, pids, …)`. Adding the KaSe VID to `vids[]`
and the KaSe PID to `pids[]` makes it open our dongle. (The `*_VID`/`*_PID` constants
live in the bundled `thirdparty/ykcore/ykcore.h`.)

A ready-to-apply patch is in this repo:

```bash
# from a KeePassXC source checkout:
git apply /path/to/KaSe_firmware/docs/keepassxc-kase-vid.patch
#   or, if line numbers drifted on your version:
patch -p1 --fuzz=3 < /path/to/KaSe_firmware/docs/keepassxc-kase-vid.patch
# then build KeePassXC.
```

The patch adds (verbatim):
```diff
-static const int vids[] = {YUBICO_VID, ONLYKEY_VID};
+#define KASE_DONGLE_VID 0x303a
+#define KASE_DONGLE_PID 0x4001
+static const int vids[] = {YUBICO_VID, ONLYKEY_VID, KASE_DONGLE_VID};
 ...
-                           ONLYKEY_PID};
+                           ONLYKEY_PID,
+                           KASE_DONGLE_PID};
```

Build KeePassXC from the patched source (downstream patch or upstream PR). For
`ykchalresp` / `pam_yubico`, the analogous whitelist is in `ykpers` `ykcore/ykcore.c`
(`yubico_pids[]`) — add `0x4001` there if you use those tools.

---

## 3. Configure KeePassXC

1. Run the **patched** KeePassXC with the dongle plugged in.
2. Database ▸ Database Security ▸ *Add additional protection* ▸ **Challenge-Response**.
3. Pick the slot matching your provisioned dongle slot (dongle slot 0 → **Slot 1**).
4. KeePassXC issues a challenge; it shows **"Touch"** and waits → **press `K_SEC_CONFIRM`**
   on your half → the database unlocks. The same happens on every unlock thereafter.

Cross-check (optional, also needs the ykpers VID patch):
```bash
ykchalresp -1 -x <hex-challenge>   # press K_SEC_CONFIRM when it waits
```

---

## Security model (what this does / doesn't protect)

- **Protects against:** host malware exfiltrating the secret (it is write-only over CDC,
  never read back) and malware *triggering* a response (the response requires a physical
  `K_SEC_CONFIRM` keypress that malware cannot inject) — and a KeePass DB copied to another
  machine without the dongle.
- **Does NOT protect against:** physical access to the powered, unlocked laptop, or a flash
  dump of the dongle (secrets are stored unencrypted at rest in this phase — explicitly out
  of scope). A future eFuse-HMAC KEK can add at-rest encryption without losing
  reprogrammability; Secure Boot (tamper resistance) is deferred.
- HMAC-**SHA1** is used because the YubiKey/KeePassXC ecosystem requires it; only SHA-1's
  *collision* resistance is broken (signatures), **not** HMAC-SHA1, which remains secure.

---

## Hardware validation checklist (the remaining gate)

The firmware **builds** but real USB behaviour can only be confirmed on the device:

1. **Reflash properly so `CONFIG_TINYUSB_HID_COUNT=2` takes effect** — it is set in
   `sdkconfig.defaults.dongle`; do `idf.py -B build_kase_dongle reconfigure` (or a clean
   build / full flash) so the 2nd HID interface enumerates.
2. Confirm the dongle still enumerates its **keyboard + mouse + CDC** as before, **plus** a
   new HID interface ("KaSe Dongle OTP").
3. On-target HMAC-SHA1 known-answer test (RFC 2202 case 1: key=0x0b×20, "Hi There" →
   `b617318655057264e28bc0b6fb378c8ef146be00`) — Plan 2 Task 5, optional.
4. Provision a slot with `kase-sec`, then unlock a CR-protected KeePassXC DB (patched) and
   verify the **press-to-confirm** flow and the unlock.
5. Optionally `ykchalresp` cross-check against a software HMAC-SHA1 of the same challenge.

---

## 2. OpenPGP Smartcard — SSH + git signing

### What it is

The dongle enumerates as a **GnuPG-compatible OpenPGP smartcard** (CCID class 0x0B, USB
`303a:4001`). gpg can sign git commits and files using a NIST P-256 private key that lives
on the dongle and never leaves it. Every sign operation is gated by a **physical touch**
(`K_SEC_CONFIRM` keypress on a paired half).

**Credential separation rationale:** the dongle is the *"dev identity" basket* (OpenPGP:
git commit signing; SSH auth in Phase 2). The YubiKey 5 NFC remains the *"web/accounts"
basket* (FIDO2 web 2FA). Losing or compromising one does not affect the other.

**Threat model:** host malware only, NOT physical access. The private key is non-extractable
over USB and every sign requires a physical keypress that malware cannot inject. Keys are
stored plaintext in NVS at rest — the dongle is fully reprogrammable (no Secure Boot, no
secure element). A future eFuse-HMAC KEK ("Couche 1") can add at-rest encryption without
losing reprogrammability, but this is deferred.

**Status:** ✅ validated on hardware (gpg 2.4.9 / scdaemon, ESP32-S3 `303a:4001`). See
`docs/superpowers/specs/2026-06-09-dongle-openpgp-card-design.md §7b` for the full
hardware-validated result including the five gotchas discovered during bring-up.

---

### Host requirements

- **gpg 2.3+** (ships scdaemon's internal CCID driver). No pcscd, no libccid, no VID
  whitelist patch needed — scdaemon opens CCID devices by USB class 0x0B.
- The existing `99-local.rules` udev rule already grants access to `303a:4001`. No new
  rule needed.
- If gpg is absent on NixOS: `nix-shell -p gnupg`.
- The Dell's built-in Broadcom reader (`0A5C:5843`) coexists — scdaemon enumerates all
  CCID readers; the KaSe dongle appears as an additional reader.

---

### Provisioning (exact, validated commands)

```bash
# 1. Generate a P-256 signing key (skip if you already have one)
gpg --expert --full-generate-key
#   → ECC and ECC
#   → Existing DSA or ECDH curve → enter:  nistp256
#   → Sign only (no encryption subkey needed for this slot)
#   → Set expiry, name, email as desired

# 2. Import the signing key onto the card
gpg --edit-key <KEYID>
  gpg> keytocard
  #   → (1) Signature key
  #   → Enter Admin PIN: 12345678
  gpg> save

# 3. Verify
gpg --card-status
#   Signature key:   <fingerprint>   ← confirms key is on card
gpg -K
#   sec>  nistp256/...   ← ">" means card-backed; "Card serial no." shown
```

> `keytocard` requires the card to respond to `READ PUBLIC KEY` (INS 0x47 P1=0x81) — the
> firmware implements this by deriving Q=d·G via mbedTLS. Without it, gpg silently falls
> back to the local key (no touch, no card use) and gives no error.

---

### Default PINs

| PIN | Default | Used for |
|-----|---------|----------|
| User PW1 | `123456` | Signing (PSO:CDS) |
| Admin PW3 | `12345678` | Key import (keytocard / PUT DATA) |

PINs persist across reboots and app-reflashes (NVS). Retry limit: 3 for PW1, 3 for PW3.
**Change them before use:**

```bash
gpg --card-edit
  gpg/card> admin
  gpg/card> passwd
  #   → change PIN (PW1) and Admin PIN (PW3)
  gpg/card> quit
```

---

### Signing and the touch gate

**File / arbitrary data:**
```bash
echo x | gpg -u <KEYID> --clearsign
```
gpg prompts for PW1 (User PIN), then the card arms the touch gate and waits up to 15s.
**Press `K_SEC_CONFIRM`** on a paired half within 15s → signature returned. No touch within
15s → `gpg: signing failed: Conditions of use not satisfied` (SW 6985). No intermediate
USB timeout — WTX (time-extension) frames hold scdaemon for the full 15s.

**git commit signing:**
```bash
git config user.signingkey <KEYID>
git config commit.gpgsign true
git commit -S -m "message"   # or just `git commit` with gpgsign=true
```
gpg is invoked by git; the same PW1 prompt + touch flow applies. `git log --show-signature`
confirms "Good signature".

---

### SSH (Phase 2 — not yet)

The Authentication slot (`INTERNAL AUTHENTICATE` → SSH via `gpg-agent --enable-ssh-support`)
is roadmap. It shares the same CCID transport and UIF gate; only the applet command and key
slot are new. Tracked in the spec §8.

---

### What's proven / what needs your finger

Everything listed above is validated on hardware. The one thing that cannot be automated
and requires a human: the **physical `K_SEC_CONFIRM` keypress**. `sec_confirm` is armed
only during a live PSO:CDS call and is authorized only by a real NRF keypress from a
paired half — there is no software path to bypass it. This is the design.
