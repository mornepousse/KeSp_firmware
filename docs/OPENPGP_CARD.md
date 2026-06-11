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

## 0. Première configuration — tutoriel pas à pas (FR)

> Validé en live le 2026-06-11 (gpg 2.4.9 via `nix-shell`). Refais exactement ces étapes pour
> installer **ta vraie identité** sur le dongle. Compte ~15 min. Sections 3-10 = la référence détaillée.

**Prérequis — un pinentry dans nix-shell.** `nix-shell -p gnupg` n'embarque PAS de pinentry, donc
gpg ne peut pas demander tes PINs. On en ajoute un (une seule fois) :

```bash
mkdir -p ~/.gnupg && chmod 700 ~/.gnupg
nix-shell -p gnupg pinentry-curses --run \
  'echo "pinentry-program $(command -v pinentry-curses)" > ~/.gnupg/gpg-agent.conf; gpgconf --kill gpg-agent'
```
Désormais lance gpg ainsi : `nix-shell -p gnupg pinentry-curses --run 'gpg ...'`.

**Étape 1 — vérifier la carte.**
```bash
nix-shell -p gnupg --run 'gpgconf --kill scdaemon; gpg --card-status'
```
Tu dois voir `Application type: OpenPGP`, `Key attributes: nistp256 cv25519 nistp256`, PINs `3 0 3`.
Si tu vois "No such device", relance (`gpgconf --kill all` puis re-`gpg --card-status`) — le CCID
se réveille parfois au 2e essai.

**Étape 2 — changer les PINs (defaults `123456`/`12345678` publics).**
```bash
nix-shell -p gnupg pinentry-curses --run 'gpg --card-edit'
```
Puis : `admin` → `passwd` → `1` (PW1 user, actuel `123456`, ≥ 6 car.) → `3` (PW3 admin, actuel
`12345678`, ≥ 8 car.) → `q`. Garde `gpg/card>` ouvert pour l'étape 3.
⚠️ Retiens-les : 3 essais faux sur **les deux** PINs = carte bloquée (récupérable seulement par
factory-reset, qui efface les clés).

**Étape 3 — générer ton identité SUR la carte.** Toujours dans `gpg/card>` (`admin` actif) :
```
generate
```
- **« Make off-card backup of encryption key? »** → `n` (les clés ne quittent jamais le dongle ;
  caveat : sans backup, si le dongle meurt tu perds le déchiffrement d'anciens messages — OK pour
  une identité dev git/SSH).
- PIN Admin, validité (`0` = pas d'expiration), puis **Real name / Email / Comment**.
- 👉 **TOUCHE REQUISE** : à la fin, gpg auto-signe ton certificat avec la clé de **signature**, et
  l'UIF Sign est ON → **presse `K_SEC_CONFIRM` sur ta moitié dans les 15 s** quand l'écran/agent
  attend. Sans la touche, la génération échoue (6985).
- Résultat attendu : `public and secret key created and signed.` et `gpg -K` montre `sec>` + deux
  `ssb>` avec `Card serial no.` (les 3 clés sont sur la carte).

**Étape 4 — signature git.**
```bash
FPR=$(nix-shell -p gnupg --run 'gpg --list-keys --with-colons' | awk -F: '/^fpr/{print $10; exit}')
git config --global user.signingkey $FPR
git config --global commit.gpgsign true
git config --global gpg.program "$(command -v gpg || echo gpg)"
```
Chaque `git commit` demandera PW1 + **une touche** (UIF Sign ON). Vérifie : `git log --show-signature`.

**Étape 5 — SSH (GitLab/GitHub).**
```bash
grep -q enable-ssh-support ~/.gnupg/gpg-agent.conf || echo enable-ssh-support >> ~/.gnupg/gpg-agent.conf
gpgconf --kill gpg-agent
export SSH_AUTH_SOCK=$(gpgconf --list-dirs agent-ssh-socket)   # mets-le dans ta conf home NixOS
nix-shell -p gnupg openssh --run 'gpg --card-status >/dev/null; ssh-add -L'
```
Copie la ligne `ecdsa-sha2-nistp256 … cardno:…` dans GitLab → *SSH Keys*. Test : `ssh -T git@gitlab.com`.

**Voilà.** Tu as une identité dev complète (sign + decrypt + SSH) née sur le dongle. Pour repartir
de zéro un jour : `gpg --card-edit` → `admin` → `factory-reset` (efface tout, PINs par défaut).

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

- **Reset scdaemon between attempts** (it caches card state):

  ```bash
  gpgconf --kill scdaemon
  ```

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
  `sdkconfig.defaults.dongle`:

  ```
  CONFIG_ESP_CONSOLE_UART_DEFAULT=y
  CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
  ```

  then `rm build_kase_dongle/sdkconfig` and rebuild → logs appear on the CH340 at
  `/dev/ttyUSB0`. **REVERT both lines before committing** (production keeps the console off so
  the matrix GPIOs stay free).

---

## What's proven / what needs your finger

Everything above is validated on hardware. The one thing that cannot be automated and requires
a human: the **physical `K_SEC_CONFIRM` keypress**. `sec_confirm` is armed only during a live
gated op and authorized only by a real NRF keypress from a paired half — there is no software
path to bypass it. This is the design.
