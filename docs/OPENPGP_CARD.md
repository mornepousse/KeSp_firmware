# KaSe Dongle — OpenPGP Smartcard

The dongle enumerates as a **GnuPG-compatible OpenPGP smartcard** (CCID class 0x0B, USB
`303a:4001`). It is the *"dev identity" basket*: git commit signing, SSH authentication, and
decryption, using ECC keys that **live on the dongle and never leave it**. Every operation can
be gated by a **physical touch** — a `K_SEC_CONFIRM` keypress on a paired keyboard half that
malware cannot inject.

> **Threat model:** host malware only, NOT physical access. Keys are stored plaintext in NVS
> (the dongle is fully reprogrammable — no Secure Boot, no secure element — a deliberate
> decision; see the design spec §2b). The one un-bypassable gate is the physical touch.
> Validated on hardware (gpg 2.4.9 / scdaemon). Full result + gotchas:
> `docs/superpowers/specs/2026-06-09-dongle-openpgp-card-design.md` §7b (Phase 1) and §8b
> (Phase 2).

---

## 0. First-time setup — step-by-step tutorial

> Validated live on 2026-06-11 (gpg 2.4.9). Follow these exact steps to install **your real
> identity** on the dongle. Takes ~15 min. Sections 3-10 are the detailed reference.
>
> 🚀 **Shortcut:** `scripts/kase-pgp-setup.sh setup` is a guided wizard that chains everything
> (CCID detection + recovery, PINs, generate with a touch reminder, git signing, SSH/GitLab key,
> and `… reset` to start over). It NEVER sees your PINs (pinentry) and cannot press the touch
> button in your place. The manual steps below remain the reference / the fallback.

**Prerequisite — native gpg (NixOS, recommended).** For daily use, gpg + the agent (scdaemon
card driver, pinentry, SSH agent) should be **declarative** in your config, not via `nix-shell`.
On NixOS:

```nix
# modules/apps/gnupg.nix  (imported by your hosts)
programs.gnupg.agent = {
  enable = true;
  enableSSHSupport = true;                 # exposes the card's AUTH key to ssh + sets SSH_AUTH_SOCK
  pinentryPackage = pkgs.pinentry-gnome3;  # Wayland/Hyprland GUI; or pinentry-curses in a terminal
};
environment.systemPackages = [ pkgs.gnupg ];
# DO NOT enable services.pcscd: scdaemon uses its own internal CCID driver.
# A udev rule for VID 303a is required (non-root access to the USB CCID node).
```
`nh os switch`, then **open a fresh login shell** (`exec zsh -l` or re-log the session) so that
`SSH_AUTH_SOCK` gets set. Check: `echo "$SSH_AUTH_SOCK"` is non-empty.
After that, all the commands below are **direct** `gpg` (no more `nix-shell`).

> Not native yet? One-off fallback: `nix-shell -p gnupg pinentry-curses --run 'gpg ...'` after
> `echo "pinentry-program $(command -v pinentry-curses)" > ~/.gnupg/gpg-agent.conf`. But for daily
> use, go native as above.

**Step 1 — check the card.**
```bash
gpgconf --kill scdaemon; gpg --card-status
```
You should see `Application type: OpenPGP`, `Key attributes: nistp256 cv25519 nistp256`, PINs `3 0 3`.
If you see "No such device", retry (`gpgconf --kill all` then `gpg --card-status` again) — the CCID
sometimes wakes up on the 2nd try.

**Step 2 — change the PINs (public defaults `123456`/`12345678`).**
```bash
gpg --card-edit
```
Then: `admin` → `passwd` → `1` (PW1 user, current `123456`, ≥ 6 chars) → `3` (PW3 admin, current
`12345678`, ≥ 8 chars) → `q`. Keep `gpg/card>` open for step 3.
⚠️ Remember them: 3 wrong attempts on **both** PINs = card locked (recoverable only by
factory-reset, which erases the keys).

**Step 3 — generate your identity ON the card.** Still inside `gpg/card>` (`admin` active):
```
generate
```
- **"Make off-card backup of encryption key?"** → `n` (the keys never leave the dongle;
  caveat: without a backup, if the dongle dies you lose decryption of old messages — fine for
  a dev git/SSH identity).
- Admin PIN, validity (`0` = no expiration), then **Real name / Email / Comment**.
- 👉 **TOUCH REQUIRED**: at the end, gpg auto-signs your certificate with the **signature**
  key, and UIF Sign is ON → **press `K_SEC_CONFIRM` on your half within 15 s** when the
  pinentry/agent is waiting. Without the touch, generation fails (6985).
- Expected result: `public and secret key created and signed.` and `gpg -K` shows `sec>` + two
  `ssb>` with `Card serial no.` (all 3 keys are on the card).

**Step 4 — git signing.**
```bash
FPR=$(gpg --list-keys --with-colons | awk -F: '/^fpr/{print $10; exit}')
git config --global user.signingkey $FPR
git config --global commit.gpgsign true
git config --global gpg.program gpg
```
Every `git commit` will ask for PW1 + **a touch** (UIF Sign ON). Verify with: `git log --show-signature`.

**Step 5 — SSH (GitLab/GitHub).** With `enableSSHSupport = true`, `SSH_AUTH_SOCK` is already set
(fresh login shell) — no manual export needed.
```bash
ssh-add -L
```
Copy the `ecdsa-sha2-nistp256 … cardno:…` line into GitLab → *SSH Keys*. Test: `ssh -T git@gitlab.com`.
- `ssh-add -L` says "has no identities"? The card does not have an AUTH key yet (redo step 3),
  or `SSH_AUTH_SOCK` is not set (fresh login shell).
- "Could not open a connection to your authentication agent" = `SSH_AUTH_SOCK` missing →
  `export SSH_AUTH_SOCK="$(gpgconf --list-dirs agent-ssh-socket)"; gpgconf --launch gpg-agent`, then
  fix persistence (login shell / gpg-agent module). If another agent (gnome-keyring) is hogging
  the variable, start gnome-keyring with `--components=secrets,pkcs11` (without `ssh`).

**That's it.** You now have a complete dev identity (sign + decrypt + SSH) born on the dongle. To
start over one day: `gpg --card-edit` → `admin` → `factory-reset` (wipes everything, default PINs).

---

## 1. What the card is

Three OpenPGP key slots, each its own algorithm and touch policy:

| Slot | OpenPGP role | Algorithm | Used for | Factory touch (UIF) |
|------|--------------|-----------|----------|---------------------|
| **SIG** | Signature | `nistp256` (P-256 ECDSA) | git commit signing, file signing | **ON** |
| **DEC** | Decryption | `cv25519` (X25519 ECDH) | `gpg --decrypt` | off |
| **AUT** | Authentication | `nistp256` (P-256 ECDSA) | SSH login | off |

Keys can be **generated on the card** (born on the dongle, never exist off it) or imported from
the host with `keytocard`. The card's purpose is **dev identity** (git + SSH), kept separate
from the YubiKey's "web/accounts" basket — losing or compromising one does not affect the other.

---

## 2. Host setup

- **gpg 2.3+** ships scdaemon's **internal CCID driver**, which opens CCID devices by USB class
  0x0B regardless of VID. So: **no pcscd, no libccid, no VID-whitelist patch.**
- The existing `99-local.rules` udev rule already grants access to `303a:4001` — **no new rule
  needed**. On NixOS, if you need it explicitly, add it via `services.udev.extraRules` and
  `systemctl restart systemd-udevd`.
- If gpg is absent (NixOS): `nix-shell -p gnupg`.
- The Dell's built-in Broadcom reader (`0A5C:5843`) coexists — scdaemon enumerates all CCID
  readers; the KaSe dongle appears as an additional one.

Verify the card is seen:

```bash
gpg --card-status
#   Application ID ...: D276000124010304FF00<serial>
#   Key attributes ...: nistp256 cv25519 nistp256
#   PIN retry counter : 3 0 3
#   UIF setting ......: Sign=on
```

---

## 3. PINs

| PIN | Default | Retry | Gates |
|-----|---------|-------|-------|
| **User PW1** | `123456` | 3 | sign (PSO:CDS), decrypt, SSH auth |
| **Admin PW3** | `12345678` | 3 | key import (`keytocard`), on-device `generate`, config (`PUT DATA`) |

(The middle `0` in the `3 0 3` retry triple is the optional **reset code (RC)**, which is
disabled.) PINs and retry counters persist across reboots and app-reflashes (NVS); each failed
verify decrements the counter immediately, so pulling the plug does not reset a brute-force
attempt.

**STRONGLY advised: change both PINs before any real use.**

```bash
gpg --card-edit
  gpg/card> admin
  gpg/card> passwd
  #   1 = change PW1 (user), 3 = change PW3 (admin)
  gpg/card> quit
```

---

## 4. Generating keys on-device

The recommended path — keys are **born on the dongle and never exist anywhere else** (there is
no off-card backup, and that is the point):

```bash
gpg --card-edit
  gpg/card> admin
  gpg/card> generate
  #   → "Make off-card backup of encryption key?"  →  n   (no backup = the point)
  #   → Enter Admin PIN: 12345678  (or your changed PW3)
  #   → key validity / real name / email / comment
```

This fills all three slots (SIG/DEC/AUT) with freshly generated ECC keys and builds the local
card-backed stubs. `gpg -K` then shows `sec>` (the `>` means card-backed) with a "Card serial
no.".

**Alternative — import an existing key** with `keytocard` (this **moves** the key off the host;
the private key is removed from your keyring and lives only on the card afterward):

```bash
gpg --edit-key <KEYID>
  gpg> keytocard
  #   → choose the slot (1 = Signature, 2 = Encryption, 3 = Authentication)
  #   → Enter Admin PIN
  gpg> save
```

> `keytocard` and `generate` require the card to answer `READ PUBLIC KEY` (INS 0x47 P1=0x81);
> the firmware implements this. Without it gpg would silently fall back to the local key (no
> touch, no card use) and give no error.

---

## 5. Git commit signing

```bash
git config --global user.signingkey <KEYID>     # the SIG-slot key
git config --global commit.gpgsign true
git commit -m "message"                          # signs automatically
```

gpg prompts for **PW1** (User PIN), then — because the SIG slot ships with touch **ON** — the
card arms the touch gate and waits up to **15 s**. **Press `K_SEC_CONFIRM`** on a paired half
within the window → signature returned. No touch within 15 s → `gpg: signing failed:
Conditions of use not satisfied` (SW 6985). There is no intermediate USB timeout — WTX frames
hold scdaemon for the full 15 s.

Verify:

```bash
git log --show-signature        # "Good signature"
```

---

## 6. SSH authentication

The AUT slot is your SSH identity (`ecdsa-sha2-nistp256`, accepted by GitLab/GitHub/OpenSSH).

1. Enable ssh support in `~/.gnupg/gpg-agent.conf`:

   ```
   enable-ssh-support
   ```

2. Point SSH at gpg-agent's socket (in your shell rc / NixOS home config):

   ```bash
   export SSH_AUTH_SOCK=$(gpgconf --list-dirs agent-ssh-socket)
   gpgconf --launch gpg-agent
   ```

3. Read the public key and add it to GitLab/GitHub:

   ```bash
   ssh-add -L
   #   ecdsa-sha2-nistp256 AAAA... cardno:<serial>
   ```

4. First use prompts **PW1**; if you turned UIF on for the AUT slot (`uif 3 on`), it also
   prompts for a touch. Test:

   ```bash
   ssh -T git@gitlab.com
   ```

> **NixOS note:** make the socket persistent by setting `programs.gpg-agent.enableSshSupport =
> true` (home-manager) — it exports `SSH_AUTH_SOCK` and launches the agent for you, so you do
> not hand-export it per shell.

---

## 7. Touch control (UIF)

Each slot has its own User-Interaction-Flag DO (D6 sign / D7 decrypt / D8 auth). Factory
default: **Sign = on, Decrypt = off, Auth = off.**

```bash
gpg --card-edit
  gpg/card> admin
  gpg/card> uif 1 on      # 1 = signature  (D6)
  gpg/card> uif 2 on      # 2 = decryption (D7)
  gpg/card> uif 3 on      # 3 = authentication (D8)
  #   ... or "off" to disable
```

The "touch" is a **`K_SEC_CONFIRM` keypress on a paired keyboard half** (mapped via the
controller/keymap). When a UIF-gated op runs, gpg shows a touch prompt and the card waits up to
**15 s**; no keypress → the op fails with **6985** ("Conditions of use not satisfied").

---

## 8. Factory reset

```bash
gpg --card-edit
  gpg/card> admin
  gpg/card> factory-reset
```

This issues TERMINATE + ACTIVATE: it **wipes all keys** in every slot and **restores the
default PINs** (`123456` / `12345678`).

> **Security note (from the audit).** A host adversary who exhausts **both** PIN retry counters
> (6 failed VERIFY total) can call TERMINATE and **destroy** all keys. This is OpenPGP spec
> behavior (§7.2.16, "Life cycle status") — the same on YubiKey and Nitrokey. It is a
> **denial** primitive, not an exfiltration one: TERMINATE destroys but **cannot read out** any
> key, and the touch gate makes exfiltration impossible regardless. A wiped card just means
> regenerate on-device.

> ⚠️ **Hard-won lesson (2026-06-25).** Keys generated **on-device with no backup** are a single
> point of failure: **any** factory reset = total, unrecoverable loss. An on-card identity was
> destroyed by running `kase-pgp-setup.sh reset` against the **live** card while testing the
> wizard (`KASE_YES=1 reset` / `echo oui | reset`). Two rules:
> 1. **Never run `reset` (or `gpg factory-reset`, or the raw `00E60000`/`00440000`) against a card
>    that holds a real identity** unless you have a backup and mean it. The wizard now **refuses**
>    to wipe a card that has a signature key unless you type its serial on the tty — `KASE_YES` /
>    piped `oui` are ignored when keys are present (blank cards still reset freely for CI).
> 2. For a recoverable identity, **generate off-card with an encrypted backup on offline media,
>    then `keytocard`** — you keep daily on-card isolation *and* a recovery path. On-card-only is
>    the most *isolated* but the most *brittle* (no recovery). Choose with eyes open.

---

## 9. Quantum posture

1. **The card keys are classical** (P-256 / X25519) because **no gpg OpenPGP card format
   supports PQC in 2026** — same as every commercial token (YubiKey, Nitrokey, Gnuk).
   ML-DSA/ML-KEM are not yet in the gpg card path; classical ECC is the only viable choice.

2. **Exposure:**

   | Slot | Algo | Exposure |
   |------|------|----------|
   | Signature / Auth | P-256 | needs a *live* quantum attacker at use time (does not exist; est. 2035+) — **low** |
   | Decryption | X25519 | harvest-now-decrypt-later — **accepted** for a dev-identity key (git+SSH, not long-lived confidentiality) |

3. **Rotation plan:** the day gpg ships PQC card algorithms, **regenerate on-device** (§4) —
   **nothing is fused**, the dongle stays reprogrammable.

4. **The SSH "quantum" server warnings are NOT this card.** They concern the OpenSSH **session
   key exchange (KEX)** — a transport layer separate from the card auth key. Fix it host-side
   in `~/.ssh/config`:

   ```
   Host *
       KexAlgorithms sntrup761x25519-sha512@openssh.com,curve25519-sha256
   ```

See the design spec **§2c** for the full posture.

---

## 10. Troubleshooting

### 10.0 Daily use (the only thing to remember)

Once the identity is installed (§0), you never redo ANY of what follows. Day to day:

- **Signed commit**: `git commit …` → type your User PIN (cached for a while) **+ press
  `K_SEC_CONFIRM`** (the key you mapped, e.g. the former ESC of half_left) within 15 s.
- **SSH / push**: transparent — the card handles the auth (PIN if prompted).

Everything else in this section is **"just in case"** — most of the failures below only
happen during **initial setup**.

### 10.1 Common failures — symptom → cause → fix

| You see… | Cause | Fix |
|---|---|---|
| `gpg: No pinentry` | gpg-agent has no pinentry program (empty config, or a garbage-collected nix-shell path) | Install a **persistent** one: `nix profile install nixpkgs#pinentry-curses` then `echo "pinentry-program $(command -v pinentry-curses)" >> ~/.gnupg/gpg-agent.conf && gpgconf --kill gpg-agent`. Permanent/clean = via the NixOS module (§0). |
| `^M` when you confirm (Enter does not go through) | Terminal stuck in **raw mode** (left behind by an interrupted `expect`/`gpg --card-edit` session) | `stty sane` (type it even if you see `^M`). If it persists: `reset` ⏎ or a new terminal. |
| ssh: `Could not open a connection to your authentication agent` | `SSH_AUTH_SOCK` not set in this shell | Fresh login shell (`exec zsh -l`) if the `enableSSHSupport` module is active; otherwise `export SSH_AUTH_SOCK="$(gpgconf --list-dirs agent-ssh-socket)" && gpgconf --launch gpg-agent`. |
| ssh-add: `The agent has no identities` | No AUTH key on the card (yet), or card not read | Generate the identity (§0/§4), or run `gpg --card-status` once to wake the card. *(normal message on a blank card.)* |
| `No such device` / `OpenPGP card not available` | CCID interface wedged, often after a burst of operations | `gpgconf --kill all` then retry 1-2 times (the CCID wakes up). Persistent wedge → **unplug and replug the dongle**. |
| `Conditions of use not satisfied` after a reset | scdaemon has a stale view of the card | `gpgconf --kill all` then `gpg --card-status`. |
| `factory-reset` → `This command is not supported by this card` | The firmware does not advertise the "life-cycle" flag to gpg (known limitation) | Reset via **raw APDU** (locks both PINs → TERMINATE `00E60000` → ACTIVATE `00440000`) or `scripts/kase-pgp-setup.sh reset`. |
| **Timeout** on a **passphrase** during `generate` | This is the off-card backup (the pinentry times out) | At `generate`, answer **`n`** to *"Make off-card backup?"* (no backup = no passphrase = no timeout). |
| **Timeout** on the **touch** during `generate` | The 15 s UIF gate window was exceeded | Set up without pressure: `gpg --card-edit → admin → uif 1 off` BEFORE `generate`, then `uif 1 on` afterward. The touch remains mandatory for daily use. |
| `Key generation failed` / keys on the card but `General key info: [none]` | A `generate` failed (backup/touch) → orphaned keys with no keyring entry | Reset (raw APDU above) then regenerate cleanly (`backup n`, `uif off`). |
| `pubkey_encrypt failed: Invalid object` | Firmware bug fixed (double `0x40` cv25519 prefix) | Update the firmware (≥ commit `ab105aa6`) and regenerate. |

### 10.2 Clean card reset (the raw command that always works)

gpg's `factory-reset` does not work on this card → deterministic reset via raw APDU (independent
of the PINs: locks both, then TERMINATE + ACTIVATE):

```bash
gpg-connect-agent /hex "scd serialno" \
  "scd apdu 00A4040006 D27600012401" \
  "scd apdu 0020008106 303030303030" "scd apdu 0020008106 303030303030" "scd apdu 0020008106 303030303030" \
  "scd apdu 0020008308 3030303030303030" "scd apdu 0020008308 3030303030303030" "scd apdu 0020008308 3030303030303030" \
  "scd apdu 00E60000" "scd apdu 00440000" /bye
gpgconf --kill all      # then: gpg --card-status  (should show 0 keys, PINs 3 0 3)
```
(or more simply: `./scripts/kase-pgp-setup.sh reset`)

### 10.3 Diagnostic tools

- **Reset scdaemon between two attempts** (it caches the card's state): `gpgconf --kill scdaemon`
  (or `gpgconf --kill all` to relaunch everything).

- **Verbose scdaemon log** — in `~/.gnupg/scdaemon.conf`:

  ```
  debug-level guru
  log-file /tmp/scd.log
  ```

- **`No secret key` on decrypt with a reused GNUPGHOME** — a stale `shadowed-private-key` stub
  from an earlier failed import. It is host state, not a firmware bug: use a **pristine
  GNUPGHOME** (`rm -rf $H && mkdir -p $H && chmod 700 $H`) or re-run `keytocard`.

- **"Card removed" after an APDU storm** (e.g. right after a factory-reset's NVS writes) — the
  CCID interface dropped momentarily; recover with:

  ```bash
  gpgconf --kill all
  ```

- **Firmware-side logs** (the dongle is `CONSOLE_NONE` by default). Temporarily add to
  `boards/kase_dongle/sdkconfig.defaults`:

  ```
  CONFIG_ESP_CONSOLE_UART_DEFAULT=y
  CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
  ```

  then `rm build_kase_dongle/sdkconfig` and rebuild → logs appear on the CH340 at
  `/dev/ttyUSB0`. **REVERT both lines before committing** (production keeps the console off so
  the matrix GPIOs stay free).

---

## 11. Security posture — at-rest, integrity, RF link (2026-06-25)

A security pass hardened the dongle on three axes. Status below; some items are
configured + validated on a spare ESP32-S3 and await the (irreversible) dongle
rollout.

**At-rest — NVS encryption (HMAC scheme).** Without it, a flash dump
(`esptool read_flash 0x9000 0x10000`) yields the private keys in cleartext
(empirically confirmed). With it, the `nvs` partition is XTS-AES ciphertext, the
key derived from an eFuse HMAC key auto-generated on-chip (read-protected, never
leaves the part). Config in `boards/kase_dongle/sdkconfig.defaults`; validated on the spare
board (entropy 7.89, 0 cleartext strings). Spec:
`docs/superpowers/specs/2026-06-25-dongle-nvs-encryption-design.md`.

**Integrity — Secure Boot V2 (RSA-3072).** Only firmware signed with the project
key boots; a malicious/unsigned image is rejected (validated on the spare board:
corrupted app → bootloader reset loop). Forces a partition-table reflow
(bootloader >0x8000 → table at 0x10000, `partitions_dongle.csv`). ⚠️ The signing
key (`secure_boot_signing_key.pem`, gitignored) signs all future firmware —
**back it up offline; losing it = no more dongle updates.** Burning it
(`SECURE_BOOT_EN` + Secure Download Mode) is **irreversible**.

**Sequencing.** NVS-encryption + Secure Boot are flashed together (full flash,
reflowed partitions). First boot burns the eFuses. Then re-push keyboard config
and **re-mint the OpenPGP identity** so the keys are born inside an encrypted,
signed device. Generate keys **off-card with an encrypted offline backup, then
`keytocard`** — recoverable, unlike the no-backup on-card identity that was lost
on 2026-06-25 (see §8).

**RF link authentication.** The NRF24 link (halves → dongle) is being
authenticated (per-set shared key + truncated HMAC-SHA1 + session-nonce
anti-replay) to close over-the-air keystroke injection and remote `K_SEC_CONFIRM`
forgery (which would bypass the touch gate). Spec:
`docs/superpowers/specs/2026-06-25-rf-link-auth-design.md`.

**Reset safety.** `scripts/kase-pgp-setup.sh reset` now refuses to wipe a card
that holds a signature key unless you type its serial on the tty (`KASE_YES` /
piped `oui` are ignored when keys are present) — the guard that would have
prevented the 2026-06-25 identity loss.

---

## What's proven / what needs your finger

Everything above is validated on hardware. The one thing that cannot be automated and requires
a human: the **physical `K_SEC_CONFIRM` keypress**. `sec_confirm` is armed only during a live
gated op and authorized only by a real NRF keypress from a paired half — there is no software
path to bypass it. This is the design.
