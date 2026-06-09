# KaSe Dongle — Hardware Security Key (HMAC-SHA1 Challenge-Response)

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
