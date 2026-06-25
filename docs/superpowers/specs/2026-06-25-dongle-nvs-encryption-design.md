# Dongle NVS Encryption (HMAC scheme) — Design

**Goal:** Encrypt the dongle's `nvs` partition so a physical flash dump
(`esptool read_flash`) of the OpenPGP secrets yields ciphertext, not the
plaintext `pgp_keys` / `pgp_pins` / `pgp_dos` / `sec_slots` blobs that were
empirically recovered on 2026-06-25.

**Scope (this spec only):** NVS encryption via the HMAC scheme. Explicitly
**deferred** to a later hardening pass: Secure Boot V2, OTA gating (touch),
full Flash Encryption Release. See "Deferred" below.

## Threat addressed

At-rest extraction: attacker obtains the dongle, dumps flash, reads the private
scalars. NVS-HMAC encryption defeats this (NVS region = XTS-AES ciphertext, key
never leaves the chip). **Not** addressed here: a malicious firmware running on
the same chip can still use the HMAC peripheral to decrypt NVS — that residual
gap is what Secure Boot closes, deferred by decision.

## Mechanism

ESP-IDF HMAC-based NVS encryption (`NVS_SEC_KEY_PROTECT_USING_HMAC`):
- XTS key derived at runtime from an eFuse HMAC key (purpose `HMAC_UP`).
- The HMAC key is **auto-generated on-chip on first boot** (never exists
  off-chip), read-protected in eFuse.
- **No `nvs_keys` partition, no partition-table change.**
- The default `nvs` partition (label `nvs`, namespace `storage`) is encrypted
  transparently; `nvs_flash_init()` auto-selects the secure path.

## Changes

1. **Kconfig** (`sdkconfig.defaults.dongle`):
   ```
   CONFIG_NVS_ENCRYPTION=y
   CONFIG_NVS_SEC_KEY_PROTECTION_SCHEME_USING_HMAC=y   # = NVS_SEC_KEY_PROTECT_USING_HMAC
   CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=0                  # BLOCK_KEY0
   ```
   (Exact symbol names verified against the active IDF during the test build.)
2. **Code:** expected **none** — `nvs_flash_init()` (keymap.c:194) uses the
   default partition and already erases+reinits on failure. Verify on the test
   board that the plaintext→encrypted transition is handled; if the erase-on-
   failure condition doesn't cover the encryption-mismatch error, either widen
   it or do an explicit one-time `nvs_flash_erase()` before first encrypted boot.
3. **One-time NVS erase** before the first encrypted boot (plaintext NVS is
   invalid under encryption). Deterministic; not relying on auto-erase.

## Rollout — TEST BOARD FIRST (sacrificial ESP32-S3)

Ports: **`/dev/ttyUSB1`** (CP210x, flash) · **`/dev/ttyACM1`** (OTA).

1. Build with the encryption config (isolated build dir).
2. Flash test board, erase its NVS, boot → HMAC eFuse auto-burns.
3. **Prove encrypted:** write a recognizable blob, `read_flash 0x9000 0x10000`,
   confirm the blob/`pgp_*`/`keymaps`/`storage` strings are ABSENT and the
   region looks random (vs the cleartext seen on the dongle).
4. **Prove still usable:** NVS read/write round-trips; an OTA update still
   applies (the device stays reflashable — HMAC scheme does not lock download).
5. Only if all green → replicate identically on the dongle.

## Sequencing on the dongle (when Mae is ready)

erase NVS → encrypted boot (HMAC eFuse auto-burns) → re-push keyboard config via
the controller → **then** re-mint the OpenPGP identity → keys are born inside an
encrypted NVS. (NVS-encryption MUST precede the re-mint.)

## Risk / rollback

Only irreversible action = burning one eFuse HMAC key block (read-protected).
The device stays fully reflashable (no `SPI_BOOT_CRYPT_CNT`, JTAG intact). If the
test board reveals any problem, abort before touching the dongle. Brick risk ≈ nil.

## Validation / done criteria

- Test board: NVS region dump = ciphertext (no cleartext config strings), NVS
  read/write works, OTA update applies.
- Dongle (later): same dump-proof, keyboard config restored, identity re-minted
  and signing/SSH live with keys in encrypted NVS.

## Deferred (future hardening spec)

- **Secure Boot V2** — closes the malicious-firmware-decrypts-NVS gap. Note:
  enabling it *after* a full Flash-Encryption-Release burn is hard (no plaintext
  bootloader access); if pursued, prep the signed bootloader while plaintext
  flash access remains. (HMAC-NVS alone does NOT lock flash, so this constraint
  is not yet triggered.)
- **OTA gating** (require a touch to accept an OTA image) — pure software, easy
  to add later via a normal OTA.
- **Full Flash Encryption Release** — encrypt app+bootloader too; irreversible
  lockdown (no plaintext reflash, JTAG off).
