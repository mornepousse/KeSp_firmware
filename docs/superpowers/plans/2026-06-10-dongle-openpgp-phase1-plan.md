# Dongle OpenPGP Card — Phase 1 (minimal viable signing card) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Phase-0 CCID stub into a usable signing card: a Kconfig "security personality" flag (OPENPGP default), `gpg --card-status` shows the card, and `gpg --sign` / git commit signing works end-to-end with a physical touch (`K_SEC_CONFIRM` via `sec_confirm`), using a NIST P-256 ECDSA key imported via `keytocard` that never leaves the dongle.

**Architecture:** The validated Phase-0 stack (CCID class driver `ccid.c` → applet `openpgp_card.c` over `apdu.c`/`openpgp_do.c`) grows: (1) a Kconfig choice selects CCID vs OTP-HID vs nothing in the dongle composite (they are mutually exclusive — S3 FS = 4 data IN endpoints max); (2) the applet serves the **constructed DOs** (0x6E/0x65/0x7A) gpg actually reads; (3) key import via PUT DATA odd-INS (0xDB 3FFF Extended Header List), P-256 ECDSA sign via mbedtls (Ed25519 is **confirmed absent** from the ESP-IDF 5.5 mbedtls fork — checked 2026-06-10: no `MBEDTLS_PSA_BUILTIN_ALG_PURE_EDDSA` anywhere, Curve25519 is ECDH-only); (4) APDU processing moves to a worker task so the UIF touch wait doesn't block `tud_task`, with CCID time-extension frames to keep scdaemon waiting.

**Tech Stack:** C, ESP-IDF 5.5, TinyUSB custom class, mbedtls (ECDSA secp256r1, HW-accelerated MPI), host test harness (`test/`, `-DTEST_HOST`). All dongle-gated. Spec: `docs/superpowers/specs/2026-06-09-dongle-openpgp-card-design.md` (§7 Phase 1, §6b Phase-0 result, §10 open items).

**Decisions ratified (2026-06-10, Mae):**
- Default personality = **OPENPGP** (`./scripts/check.sh` builds the dongle with CCID; CR-HMAC kept behind `KASE_SEC_OTP_HID`).
- Algorithm = **NIST P-256 ECDSA** (Ed25519 verdict above; RSA later — Phase 2).
- Key **import only** (gpg `keytocard`); no on-device keygen in Phase 1.
- UIF for signing (DO 0xD6) **enabled by factory default** — every signature needs a touch.

---

## Prerequisites (host, one-time — same as Phase 0)

- gpg not installed on the machine: `nix-shell -p gnupg` (gives `gpg`/`scdaemon` 2.4.9).
- USB access via existing `99-local.rules` (VID 0x303a). No pcscd. scdaemon internal CCID driver is the default.
- Flash: `idf.py -B build_kase_dongle -p /dev/ttyUSB0 flash` (CH340; app @ 0x20000, NVS preserved).
- **Runtime logs** (the dongle is CONSOLE_NONE): temporarily add `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` + `CONFIG_LOG_DEFAULT_LEVEL_INFO=y` to `sdkconfig.defaults.dongle`, `rm build_kase_dongle/sdkconfig`, rebuild → logs on `/dev/ttyUSB0` (GPIO43/44 free on dongle). **REVERT before committing.**
- scdaemon debug: `~/.gnupg/scdaemon.conf` → `debug-level guru` + `log-file /tmp/scd.log`; `gpgconf --kill scdaemon` between attempts.

## Phase-0 gotchas already learned (do NOT rediscover)

- **Force-link**: any TinyUSB app driver / weak-symbol override needs an `init()` called from force-linked code or the linker drops the object (`ccid_init()` exists for this; check `nm build_kase_dongle/KeSp.elf | grep usbd_app_driver` if USB config asserts).
- **ATR ends `... 90 00 CD`** — TCK 0xCD is mandatory (T=1).
- **Endpoint budget**: CDC(2 IN) + HID-kbd(1 IN) + CCID(1 IN) = 4 IN = the S3 FS maximum. Never add a 5th IN.
- The dongle currently runs its pre-CCID keyboard firmware (daily driver); Task 4 reflashes it.

## File structure

- Modify `main/Kconfig.projbuild` — add `choice KASE_DONGLE_SEC_PERSONALITY` (NONE / OTP_HID / OPENPGP, default OPENPGP).
- Modify `main/CMakeLists.txt` — split the dongle security sources per personality.
- Modify `main/comm/usb/usb_hid.c` — three dongle descriptor variants behind the choice.
- Modify `main/security/openpgp_card.{c,h}` — VERIFY modes 81/82/83, constructed DOs, factory defaults, key import (0xDB), key blob + PIN persistence, DS counter, new hook signature.
- Create `main/security/openpgp_crypto.{c,h}` — P-256 ECDSA sign + selftest (mbedtls, target-only).
- Modify `main/security/ccid.c` — worker task, real hooks (sign → openpgp_crypto, confirm → sec_confirm), time extension.
- Modify `main/main.c` — dongle boot: load/seed the persisted card state.
- Tests: extend `test/test_openpgp_card.c` (+ register nothing new: suite exists).

---

### Task 1: Kconfig security personality + descriptor/CMake gating

**Files:**
- Modify: `main/Kconfig.projbuild`
- Modify: `main/CMakeLists.txt:90-107`
- Modify: `main/comm/usb/usb_hid.c`
- Modify: `sdkconfig.defaults.dongle` (comment only)

- [ ] **Step 1: Add the Kconfig choice.** In `main/Kconfig.projbuild`, after the `KASE_NRF_LINE_TEST` block:

```kconfig
choice KASE_DONGLE_SEC_PERSONALITY
    prompt "Dongle security personality"
    depends on KASE_DEVICE_ROLE_DONGLE
    default KASE_SEC_OPENPGP
    help
        The ESP32-S3 FS USB core has 4 data IN endpoints (ep_in_count=5
        incl. EP0). CDC (2 IN) + HID kbd (1 IN) leave exactly ONE slot,
        so the security interfaces are mutually exclusive:
        NONE    = plain keyboard dongle (CDC + HID only).
        OTP_HID = YubiKey-compatible OTP HID (CR-HMAC, KeePassXC).
        OPENPGP = CCID smartcard (gpg sign / SSH, touch-gated).

config KASE_SEC_NONE
    bool "None (plain keyboard dongle)"

config KASE_SEC_OTP_HID
    bool "YubiKey-compatible OTP HID (CR-HMAC)"

config KASE_SEC_OPENPGP
    bool "OpenPGP smartcard over CCID"

endchoice
```

- [ ] **Step 2: Gate the CMake sources.** In `main/CMakeLists.txt`, replace the dongle block (lines 90–107) with: keep `cdc_dongle_stubs.c`, `cdc_dongle_cmds.c`, `security/sec_store.c`, `comm/cdc/cdc_sec_cmds.c`, `security/cr_crc16.c`, `security/cr_hmac.c` unconditionally dongle (cdc_sec_cmds' SELFTEST KAT references cr_hmac); then:

```cmake
    # OTP transport (YubiKey CR-HMAC personality only)
    if(CONFIG_KASE_SEC_OTP_HID)
        list(APPEND srcs
            "security/otp_proto.c"
            "security/otp_hid.c"
        )
    endif()
    # OpenPGP smartcard personality: CCID class driver + applet + crypto
    if(CONFIG_KASE_SEC_OPENPGP)
        list(APPEND srcs
            "security/ccid.c"
            "security/openpgp_card.c"
            "security/apdu.c"
            "security/openpgp_do.c"
            "security/openpgp_crypto.c"
        )
    endif()
```

(`openpgp_crypto.c` is created in Task 7 — until then leave that line **commented with a `# Task 7` note** so the build stays green.)

- [ ] **Step 3: Three dongle descriptor variants in `usb_hid.c`.** Restructure the `#if CONFIG_KASE_DEVICE_ROLE_DONGLE` regions:
  - Includes (top of file): `#include "otp_proto.h"` under `#if CONFIG_KASE_SEC_OTP_HID`, `#include "ccid.h"` under `#if CONFIG_KASE_SEC_OPENPGP`.
  - Endpoint defines: `EPNUM_OTP_HID 0x84` under `KASE_SEC_OTP_HID`; `EPNUM_CCID_OUT 0x04` / `EPNUM_CCID_IN 0x84` under `KASE_SEC_OPENPGP`. Delete the "DE-RISK EXPERIMENT (uncommitted)" comments — this IS the committed design now; keep one comment stating the 4-IN budget.
  - `otp_hid_report_descriptor[]` under `KASE_SEC_OTP_HID`; `KASE_CCID_DESC`/`KASE_CCID_ITF_DESC` under `KASE_SEC_OPENPGP`.
  - Three config descriptors (the OTP one restored verbatim from git `75f72c6^`):

```c
#if CONFIG_KASE_SEC_OPENPGP
/* 4 interfaces: 0/1 = CDC, 2 = HID kbd/mouse, 3 = CCID (bulk EP4) */
#define TUSB_DESC_TOTAL_LEN_DONGLE \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN + TUD_CCID_DESC_LEN)
static const uint8_t dongle_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUSB_DESC_TOTAL_LEN_DONGLE,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(2, 5, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(hid_report_descriptor), EPNUM_HID, 16, 1),
    KASE_CCID_ITF_DESC(3, 6, EPNUM_CCID_OUT, EPNUM_CCID_IN),
};
#elif CONFIG_KASE_SEC_OTP_HID
/* 4 interfaces: 0/1 = CDC, 2 = HID kbd/mouse, 3 = HID OTP (int EP4) */
#define TUSB_DESC_TOTAL_LEN_DONGLE \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t dongle_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUSB_DESC_TOTAL_LEN_DONGLE,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(2, 5, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(hid_report_descriptor), EPNUM_HID, 16, 1),
    TUD_HID_DESCRIPTOR(3, 6, HID_ITF_PROTOCOL_NONE,
                       sizeof(otp_hid_report_descriptor), EPNUM_OTP_HID, 8, 5),
};
#else /* KASE_SEC_NONE */
/* 3 interfaces: 0/1 = CDC, 2 = HID kbd/mouse */
#define TUSB_DESC_TOTAL_LEN_DONGLE \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t dongle_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, TUSB_DESC_TOTAL_LEN_DONGLE,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(2, 5, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(hid_report_descriptor), EPNUM_HID, 16, 1),
};
#endif
```

  - One dongle string table (string index 6 = the security interface, present in all variants for simplicity):

```c
static const char *dongle_string_descriptor[] = {
    (const char[]){ 0x09, 0x04 },
    MANUFACTURER_NAME, PRODUCT_NAME, SERIAL_NUMBER,
    PRODUCT_NAME " CDC", PRODUCT_NAME " Keyboard",
#if CONFIG_KASE_SEC_OPENPGP
    PRODUCT_NAME " CCID",
#elif CONFIG_KASE_SEC_OTP_HID
    PRODUCT_NAME " OTP",
#endif
};
```

  - HID callbacks: the `instance == 1` OTP branches in `tud_hid_descriptor_report_cb` / `tud_hid_get_report_cb` / `tud_hid_set_report_cb` move under `#if CONFIG_KASE_SEC_OTP_HID`.
  - `tinyusb_hid_init()` dongle branch uses `dongle_string_descriptor` / `dongle_configuration_descriptor` (rename the old `_otp`-suffixed identifiers).
  - `kase_tinyusb_init()`: `ccid_init()` under `#if CONFIG_KASE_SEC_OPENPGP` (keep the force-link comment).

- [ ] **Step 4: Note in `sdkconfig.defaults.dongle`.** Below `CONFIG_TINYUSB_HID_COUNT=2`, add the comment: `# HID_COUNT=2 covers the OTP personality (instance 1). Under OPENPGP/NONE instance 1 is simply unused (a few bytes of RAM).` Do not change the value (it's per-board, the personality is per-build).

- [ ] **Step 5: Build all 3 personalities + full check.**

```bash
source ~/esp/esp-idf/export.sh
./scripts/check.sh --board kase_dongle     # default = OPENPGP — must be green
# OTP personality (throwaway build dir):
idf.py -B build_sec_otp -DBOARD=kase_dongle -DSDKCONFIG=build_sec_otp/sdkconfig reconfigure
sed -i 's/^CONFIG_KASE_SEC_OPENPGP=y/# CONFIG_KASE_SEC_OPENPGP is not set\nCONFIG_KASE_SEC_OTP_HID=y/' build_sec_otp/sdkconfig
idf.py -B build_sec_otp -DBOARD=kase_dongle -DSDKCONFIG=build_sec_otp/sdkconfig build
# NONE personality:
idf.py -B build_sec_none -DBOARD=kase_dongle -DSDKCONFIG=build_sec_none/sdkconfig reconfigure
sed -i 's/^CONFIG_KASE_SEC_OPENPGP=y/# CONFIG_KASE_SEC_OPENPGP is not set\nCONFIG_KASE_SEC_NONE=y/' build_sec_none/sdkconfig
idf.py -B build_sec_none -DBOARD=kase_dongle -DSDKCONFIG=build_sec_none/sdkconfig build
rm -rf build_sec_otp build_sec_none
./scripts/check.sh                         # all 6 boards
```
Expected: 4 successful builds (dongle ×3 + the 5 other boards via check.sh).

- [ ] **Step 6: Commit.**
```bash
git add main/Kconfig.projbuild main/CMakeLists.txt main/comm/usb/usb_hid.c sdkconfig.defaults.dongle
git commit -m "feat(pgp): Kconfig security personality — OPENPGP default, OTP/NONE selectable (Phase 1 Task 1)"
```

---

### Task 2: VERIFY modes 0x81/0x82 (TDD) — gpg sends 81 before signing

**The bug:** the applet treats P2=0x82 as the PW1 gate for PSO:CDS, but gpg verifies CHV1 **with P2=0x81** (PW1-for-signature); P2=0x82 is PW1-for-other-ops. As shipped, `gpg --sign` would VERIFY 81 (→ `6A86` wrong P1P2 today) and PSO:CDS would fail `6982` forever.

**Files:**
- Modify: `main/security/openpgp_card.c`
- Test: `test/test_openpgp_card.c` (suite already registered)

- [ ] **Step 1: Failing tests.** In `test/test_openpgp_card.c`, add (reusing the existing `build_verify`/`select_card`/`sw_of` helpers):

```c
/* VERIFY P2=0x81 (PW1 for signing) is accepted and gates PSO:CDS */
static void test_verify_81_gates_sign(void)
{
    setup_card_no_uif();   /* existing helper pattern: init + select */
    uint8_t rsp[300]; uint16_t rlen;
    uint8_t apdu[64];
    /* VERIFY 0x82 alone must NOT satisfy the signing gate */
    uint16_t n = build_verify(0x82, (const uint8_t *)"123456", 6, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY 82 ok");
    n = build_pso_cds(apdu);                 /* existing helper */
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982, "PSO:CDS still gated after 82 only");
    /* VERIFY 0x81 satisfies it */
    n = build_verify(0x81, (const uint8_t *)"123456", 6, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY 81 ok");
    n = build_pso_cds(apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PSO:CDS passes after 81");
}

/* Wrong PIN on 81 and 82 decrements the SAME retry counter */
static void test_verify_81_82_share_retries(void)
{
    setup_card_no_uif();
    uint8_t rsp[300]; uint16_t rlen; uint8_t apdu[64];
    uint16_t n = build_verify(0x81, (const uint8_t *)"000000", 6, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C2, "81 wrong -> 2 left");
    n = build_verify(0x82, (const uint8_t *)"000000", 6, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C1, "82 wrong -> 1 left (shared)");
}
```
Register both in the suite function (`TEST_RUN(...)`). Adapt the existing PW1 tests that use `0x82` for signing: they must now use `0x81`.

- [ ] **Step 2: Run red.** `cd test && cmake -B build && cmake --build build && ./build/test_runner` → the new tests FAIL (81 returns 6A86 today).

- [ ] **Step 3: Implement.** In `openpgp_card.c`: replace `s_pw1_verified` with two flags `s_pw1_sign_verified` (mode 81) and `s_pw1_user_verified` (mode 82), both backed by the single `s_pw1_retry` counter and the single `s_pw1[]` PIN. VERIFY dispatch: `p2 == 0x81 || p2 == 0x82` → PW1 (set the corresponding flag on success; reset retry to max; both flags cleared on wrong PIN is NOT required — only the counter decrements), `p2 == 0x83` → PW3. PSO:CDS checks `s_pw1_sign_verified`. PUT DATA keeps checking `s_pw3_verified`. SELECT clears all three flags.

- [ ] **Step 4: Run green.** Same command → all tests PASS (existing count + 2).

- [ ] **Step 5: Commit.**
```bash
git add main/security/openpgp_card.c test/test_openpgp_card.c
git commit -m "fix(pgp): VERIFY P2=0x81 is the signing gate gpg actually uses (81/82 modes, shared retries)"
```

---

### Task 3: Factory-default DOs + constructed DOs 6E/65/7A (TDD) — `--card-status` material

**What gpg reads after SELECT (scd/app-openpgp.c):** GET DATA `0x6E` (Application Related Data — **constructed**), `0x65` (Cardholder), `0x7A` (Security support), `0x5F50` (URL), and writes fingerprints via PUT DATA `0xC7..0xC9` / timestamps `0xCE..0xD0` which it reads back **inside 6E** as the concatenated `0xC5` (60 B) / `0xCD` (12 B). Serving only flat DOs returns `6A88` → "no supported card application".

**Files:**
- Modify: `main/security/openpgp_card.{c,h}`
- Test: `test/test_openpgp_card.c`

**Exact factory values (OpenPGP 3.4):**

```c
/* AID 0x4F — 16 bytes. Serial bytes [10..13] patched from efuse MAC on target. */
static const uint8_t FACTORY_AID[16] = {
    0xD2,0x76,0x00,0x01,0x24,0x01,  /* RID + application 01 (OpenPGP) */
    0x03,0x04,                      /* version 3.4 */
    0xFF,0x00,                      /* manufacturer: 0xFF00 (test/unmanaged range) */
    0x00,0x00,0x00,0x00,            /* serial (host default; target patches) */
    0x00,0x00                       /* RFU */
};
/* Historical bytes 0x5F52 — mirrors the ATR's historical bytes */
static const uint8_t FACTORY_HIST[10] = {0x00,0x31,0x84,0x73,0x80,0x01,0x80,0x00,0x90,0x00};
/* Extended capabilities 0xC0 — 10 bytes:
 * byte0 0x34 = key-import(0x20) | PW-status-changeable(0x10) | algo-attrs-changeable(0x04)
 * (no SM, no GET CHALLENGE, no private DOs, no AES, no KDF)
 * bytes6-7 = max special-DO length 0x0100 */
static const uint8_t FACTORY_C0[10] = {0x34,0x00, 0x00,0x00, 0x00,0x00, 0x01,0x00, 0x00,0x00};
/* Algorithm attributes: 0x13 = ECDSA, 0x12 = ECDH; OID = NIST P-256 (1.2.840.10045.3.1.7) */
static const uint8_t FACTORY_C1[9] = {0x13, 0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
static const uint8_t FACTORY_C2[9] = {0x12, 0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
static const uint8_t FACTORY_C3[9] = {0x13, 0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
/* UIF: byte0 0x01=required, byte1 0x20=button. Signing ON by default; dec/auth off. */
static const uint8_t FACTORY_D6[2] = {0x01, 0x20};
static const uint8_t FACTORY_D7[2] = {0x00, 0x20};
static const uint8_t FACTORY_D8[2] = {0x00, 0x20};
```
Plus zeroed `C5` (60 B), `C6` (60 B), `CD` (12 B), `93` DS counter (3 B = {0,0,0}), empty `5B`, `5F2D` = `"en"`, `5F35` = `"9"`, empty `5F50`.

`0xC4` (PW status, 7 B) is **synthesized live**, never stored: `{0x00, 0x10, 0x00, 0x10, s_pw1_retry, 0x00, s_pw3_retry}` (byte0 0x00 = PW1 valid for ONE signature — PSO:CDS clears the 81 flag after each success).

**Constructed-DO layouts** (BER-TLV; len < 128 → 1 byte, 128–255 → `81 xx`):
- `6E` = `4F`(16) ‖ `5F52`(10) ‖ `73{ C0 C1 C2 C3 C4 C5 C6 CD D6 D7 D8 }` — total ≈ 241 B incl. headers. **Assert < 254 in a test** (fits Le=256 short APDU and the CCID dwMaxCCIDMessageLength=271).
- `65` = `5B` ‖ `5F2D` ‖ `5F35`
- `7A` = `93`(3)

- [ ] **Step 1: Failing tests.** Add to `test/test_openpgp_card.c`:

```c
static void test_factory_defaults_aid(void)
{
    openpgp_card_factory_reset();          /* new API (this task) */
    select_card();
    uint8_t rsp[300]; uint16_t rlen; uint8_t apdu[8];
    uint16_t n = build_get_data(0x00, 0x4F, apdu);  /* existing helper or add it */
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 4F ok");
    TEST_ASSERT_EQ(rlen, 16 + 2, "AID is 16 bytes");
    TEST_ASSERT(rsp[0] == 0xD2 && rsp[5] == 0x01, "AID prefix");
    TEST_ASSERT(rsp[6] == 0x03 && rsp[7] == 0x04, "version 3.4");
}

static void test_get_data_6e_constructed(void)
{
    openpgp_card_factory_reset();
    select_card();
    uint8_t rsp[300]; uint16_t rlen; uint8_t apdu[8];
    uint16_t n = build_get_data(0x00, 0x6E, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 6E ok");
    uint16_t dlen = rlen - 2;
    TEST_ASSERT(dlen >= 200 && dlen < 254, "6E fits a short APDU");
    /* first child must be the AID TLV: 4F 10 D2 76 ... */
    TEST_ASSERT(rsp[0] == 0x4F && rsp[1] == 0x10 && rsp[2] == 0xD2, "6E starts with AID TLV");
    /* must contain the 73 discretionary template and a 60-byte C5 */
    TEST_ASSERT(memmem_u8(rsp, dlen, (const uint8_t[]){0xC5, 0x3C}, 2), "C5 len 60 present");
    /* and the live PW-status C4 with retry counters 3/0/3 */
    TEST_ASSERT(memmem_u8(rsp, dlen,
        (const uint8_t[]){0xC4, 0x07, 0x00, 0x10, 0x00, 0x10, 0x03, 0x00, 0x03}, 9),
        "C4 synthesized with live retries");
}

static void test_fingerprint_slices(void)
{
    openpgp_card_factory_reset();
    select_card(); verify_pw3();           /* existing helper */
    uint8_t fp[20]; memset(fp, 0xAB, 20);
    uint8_t rsp[300]; uint16_t rlen; uint8_t apdu[64];
    uint16_t n = build_put_data(0x00, 0xC7, fp, 20, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PUT C7 ok");
    n = build_get_data(0x00, 0xC5, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(rlen, 60 + 2, "C5 is 60 bytes");
    TEST_ASSERT(rsp[0] == 0xAB && rsp[19] == 0xAB && rsp[20] == 0x00,
                "C7 wrote slice [0..19] only");
}

static void test_get_data_65_7a(void)
{
    openpgp_card_factory_reset();
    select_card();
    uint8_t rsp[300]; uint16_t rlen; uint8_t apdu[8];
    uint16_t n = build_get_data(0x00, 0x65, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 65 ok");
    n = build_get_data(0x00, 0x7A, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 7A ok");
    /* 7A = 93 03 xx xx xx */
    TEST_ASSERT(rsp[0] == 0x93 && rsp[1] == 0x03, "7A wraps the DS counter");
}
```
Add tiny local helpers as needed (`build_get_data`, `memmem_u8` naive scan). Register the four tests.

- [ ] **Step 2: Run red.**

- [ ] **Step 3: Implement in `openpgp_card.c`.**
  - New public API in `openpgp_card.h`:
    ```c
    /* Wipe everything to factory state (DOs, PINs, retries, key). Tests + future admin cmd. */
    void openpgp_card_factory_reset(void);
    /* Populate any *missing* factory DOs without touching existing ones (target boot). */
    void openpgp_card_ensure_defaults(void);
    /* Patch the 4-byte serial inside the AID DO (target: from efuse MAC). */
    void openpgp_card_set_serial(const uint8_t serial[4]);
    ```
    `openpgp_card_init(hooks)` now only stores hooks + resets **session** state (selected, verified flags) — it no longer calls `openpgp_do_reset()` nor restores factory PINs (that moves to `openpgp_card_factory_reset()`, which calls init-internal reset + `openpgp_do_reset()` + `openpgp_card_ensure_defaults()`). Update existing tests to call `openpgp_card_factory_reset()` where they relied on init wiping.
  - A static TLV writer:
    ```c
    /* Append tag(1-2 bytes per BER)+len+value; returns new offset or 0 on overflow. */
    static uint16_t tlv_append(uint8_t *buf, uint16_t max, uint16_t off,
                               uint16_t tag, const uint8_t *v, uint16_t n);
    ```
    (two-byte tags like 0x5F52 emit both bytes; len ≥ 128 emits `0x81 len`).
  - GET DATA dispatch: tags `0x6E`, `0x65`, `0x7A` → assemble from the DO store + synthesized `C4`; tag `0xC4` → synthesized; everything else → store lookup as today.
  - PUT DATA slices: tags `C7/C8/C9` write 20-byte slices of stored `C5`; `CE/CF/D0` write 4-byte slices of stored `CD`. Reject wrong lengths with `6A80` (wrong data).
  - `openpgp_card_ensure_defaults()`: for each factory DO above, `if (!openpgp_do_get(tag,...)) openpgp_do_put(tag, ...)`.

- [ ] **Step 4: Run green.** All host tests pass.

- [ ] **Step 5: Commit.**
```bash
git add main/security/openpgp_card.c main/security/openpgp_card.h test/test_openpgp_card.c
git commit -m "feat(pgp): factory DOs + constructed 6E/65/7A + fingerprint slices — card-status material"
```

---

### Task 4: Hardware checkpoint — `gpg --card-status` shows the card

**Files:**
- Modify: `main/main.c` (dongle boot wiring)
- Modify: `main/security/ccid.c` (init order comment only)

- [ ] **Step 1: Boot wiring.** In `main/main.c`, in the dongle branch right after `sec_store_init()` (same safe-mode guard):

```c
#if CONFIG_KASE_SEC_OPENPGP
    if (!safe_mode) {
        extern void openpgp_do_init(void);            /* NVS load of pgp_dos */
        extern void openpgp_card_ensure_defaults(void);
        extern void openpgp_card_set_serial(const uint8_t serial[4]);
        openpgp_do_init();
        openpgp_card_ensure_defaults();
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        openpgp_card_set_serial(&mac[2]);             /* 4 low MAC bytes = card serial */
    }
#endif
```
NOTE the ordering trap: `ccid_drv_init()` (→ `openpgp_card_init(hooks)`) runs inside `tinyusb_driver_install()` during `kase_tinyusb_init()`. The boot code above must run **before** USB init AND `openpgp_card_init` must no longer wipe state (done in Task 3). Verify `kase_tinyusb_init()` is called after this block in `app_main`; if USB init comes first in the current code, move this block before it.

- [ ] **Step 2: Build + flash.**
```bash
./scripts/check.sh --board kase_dongle
idf.py -B build_kase_dongle -p /dev/ttyUSB0 flash
```
(This replaces the daily-driver keyboard firmware — keys must still work after enumeration, it's the same kbd path.)

- [ ] **Step 3: THE CHECKPOINT.**
```bash
nix-shell -p gnupg --run 'gpgconf --kill scdaemon; gpg --card-status'
```
**Expected:** the card prints — `Application ID: D276000124 01 0304 FF00 <serial> 0000`, `Application type: OpenPGP`, `Version: 3.4`, PIN retry counters `3 0 3`, `Signature key: [none]`, `Signature counter: 0`, UIF `Sign=on`. NOT "no supported card application found".
If it fails: `echo -e 'debug-level guru\nlog-file /tmp/scd.log' > ~/.gnupg/scdaemon.conf; gpgconf --kill scdaemon; gpg --card-status; grep -i -A3 'apdu\|error' /tmp/scd.log | tail -50` — usual suspects: a malformed constructed 6E (TLV lengths), the 6E response exceeding 254 B, or a DO gpg requires that we 6A88 (check which GET DATA precedes the failure in the log). Iterate on Task 3 encodings; any fix gets a host test.

- [ ] **Step 4: Smoke + commit.** Type on the keyboard (halves → dongle → HID OK), check `/dev/ttyACM*` CDC enumerates.
```bash
git add main/main.c main/security/ccid.c
git commit -m "feat(pgp): card boot wiring (NVS DOs + defaults + MAC serial) — gpg --card-status shows the card"
```

---

### Task 5: Key import — PUT DATA odd-INS 0xDB 3FFF Extended Header List (TDD)

**What gpg `keytocard` sends for ECDSA P-256:** `00 DB 3F FF <Lc>` with data = `4D <len>` wrapping: `B6 00` (or `B6 03 84 01 01`) for the signature slot, `7F48 <len>` template (`92 <len>` = private-key length, possibly `99 <len>` = public-key length), `5F48 <len>` = concatenated key material (private scalar `d`, 32 B, optionally followed by the public point). Then PUT DATA `C7` (fingerprint) + `CE` (timestamp) — already handled (Task 3).

**Files:**
- Modify: `main/security/openpgp_card.{c,h}`
- Test: `test/test_openpgp_card.c`

- [ ] **Step 1: Failing tests.**

```c
/* Minimal ECDSA P-256 extended header list: B6 00, 7F48 with 92=32, 5F48 = d */
static uint16_t build_key_import(const uint8_t d[32], uint8_t *buf)
{
    uint8_t tpl[64]; uint16_t o = 0;
    tpl[o++]=0xB6; tpl[o++]=0x00;
    tpl[o++]=0x7F; tpl[o++]=0x48; tpl[o++]=0x02; tpl[o++]=0x92; tpl[o++]=0x20;
    tpl[o++]=0x5F; tpl[o++]=0x48; tpl[o++]=0x20; memcpy(&tpl[o], d, 32); o+=32;
    uint16_t n = 0;
    buf[n++]=0x00; buf[n++]=0xDB; buf[n++]=0x3F; buf[n++]=0xFF;
    buf[n++]=(uint8_t)(o+2);
    buf[n++]=0x4D; buf[n++]=(uint8_t)o; memcpy(&buf[n], tpl, o); n+=o;
    return n;
}

static void test_key_import_requires_pw3(void)
{
    openpgp_card_factory_reset(); select_card();
    uint8_t d[32]; memset(d, 0x11, 32);
    uint8_t apdu[96], rsp[300];
    uint16_t n = build_key_import(d, apdu);
    uint16_t rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982, "import without PW3 refused");
    TEST_ASSERT(!openpgp_card_key_is_set(), "no key stored");
}

static void test_key_import_ok_and_sign_uses_it(void)
{
    openpgp_card_factory_reset(); select_card(); verify_pw3();
    uint8_t d[32]; for (int i = 0; i < 32; i++) d[i] = (uint8_t)i;
    uint8_t apdu[96], rsp[300];
    uint16_t n = build_key_import(d, apdu);
    uint16_t rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import ok");
    TEST_ASSERT(openpgp_card_key_is_set(), "key stored");
    /* the sign hook now receives d — fake hook records it */
    verify_pw1_sign();                 /* VERIFY 81 helper from Task 2 */
    disable_uif();                     /* helper: PW3 + PUT DATA D6 = {00,20} */
    n = build_pso_cds(apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "sign ok");
    TEST_ASSERT(memcmp(g_fake_sign_last_d, d, 32) == 0, "hook got the imported scalar");
}

static void test_sign_without_key_6a88(void)
{
    openpgp_card_factory_reset(); select_card(); verify_pw1_sign(); disable_uif();
    uint8_t apdu[64], rsp[300];
    uint16_t n = build_pso_cds(apdu);
    uint16_t rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A88, "no key -> referenced data not found");
}

/* DS counter increments on each successful signature and shows in 7A */
static void test_ds_counter_increments(void)
{
    /* import + sign twice, then GET DATA 7A and assert 93 03 00 00 02 */
}
```

- [ ] **Step 2: Hook signature change.** In `openpgp_card.h`, the sign hook becomes:

```c
typedef struct {
    bool (*sign)(const uint8_t d[32],            /* private scalar (P-256) */
                 const uint8_t *hash, uint16_t n,
                 uint8_t *out, uint16_t *out_n);
    int  (*confirm)(void);
} openpgp_card_hooks_t;
```
Update the fake hooks in the test (record `d` into `g_fake_sign_last_d`) and the Phase-0 stubs in `ccid.c` (just add the parameter; still return false — real crypto in Task 7/8). Run red.

- [ ] **Step 3: Implement in `openpgp_card.c`.**
  - Key blob + accessor:
    ```c
    typedef struct { uint8_t set; uint8_t algo; uint8_t d[32]; } pgp_key_t;  /* algo 0x13 */
    static pgp_key_t s_key;
    bool openpgp_card_key_is_set(void);   /* declare in .h */
    static bool key_persist(void);        /* NVS "pgp_key" under #ifndef TEST_HOST,
                                             true no-op on host — openpgp_do.c pattern */
    ```
  - INS `0xDB` with P1P2=0x3FFF (PW3-gated like 0xDA): walk the BER-TLV: expect `4D`, inside it require a `B6` (else `6A80` — only the Sig slot exists in Phase 1), find `7F48`, read the `92` length (must be 32, else `6A80`), find `5F48`, copy the first 32 bytes into `s_key.d`, set `algo=0x13/set=1`, persist, return `9000`. Ignore trailing public-key bytes in `5F48`. Tolerate `B6 03 84 01 01` (CRT with key-ref).
  - PSO:CDS: before the PIN check, `if (!s_key.set) return 6A88`. Pass `s_key.d` to the hook. On success: clear `s_pw1_sign_verified` (C4 byte0=0x00 semantics) and increment the 3-byte DS counter DO `0x93` (big-endian) via `openpgp_do_put`.
  - `openpgp_card_factory_reset()` also clears `s_key` (+ persist).

- [ ] **Step 4: Run green.** `./scripts/check.sh --host-only`.

- [ ] **Step 5: Commit.**
```bash
git add main/security/openpgp_card.c main/security/openpgp_card.h main/security/ccid.c test/test_openpgp_card.c
git commit -m "feat(pgp): key import via PUT DATA 0xDB 3FFF (extended header list) + DS counter + keyed sign hook"
```

---

### Task 6: PIN persistence + CHANGE REFERENCE DATA (TDD logic, NVS on target)

**Why:** PINs, retry counters and the key must survive reboot/reflash (app-only flash preserves NVS). Retry counters persisted on every decrement = no pull-the-plug brute-force reset.

**Files:**
- Modify: `main/security/openpgp_card.{c,h}`
- Test: `test/test_openpgp_card.c`

- [ ] **Step 1: Failing tests.**

```c
/* CHANGE REFERENCE DATA (INS 0x24): old PIN + new PIN concatenated */
static void test_change_pw1(void)
{
    openpgp_card_factory_reset(); select_card();
    uint8_t apdu[64], rsp[300];
    /* 00 24 00 81 0C "123456" "654321" */
    const uint8_t chg[] = {0x00,0x24,0x00,0x81,0x0C,
        '1','2','3','4','5','6','6','5','4','3','2','1'};
    uint16_t rlen = openpgp_card_apdu(chg, sizeof(chg), rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "change PW1 ok");
    uint16_t n = build_verify(0x81, (const uint8_t *)"123456", 6, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C2, "old PIN now wrong");
    n = build_verify(0x81, (const uint8_t *)"654321", 6, apdu);
    rlen = openpgp_card_apdu(apdu, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "new PIN ok");
}

static void test_change_pw_rules(void)
{
    openpgp_card_factory_reset(); select_card();
    uint8_t rsp[300];
    /* wrong old PIN -> 63Cx + decrement */
    const uint8_t bad[] = {0x00,0x24,0x00,0x81,0x0C,
        '0','0','0','0','0','0','6','5','4','3','2','1'};
    uint16_t rlen = openpgp_card_apdu(bad, sizeof(bad), rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C2, "wrong old PIN decrements");
    /* new PIN too short (<6 for PW1) -> 6A80, no decrement */
    const uint8_t shrt[] = {0x00,0x24,0x00,0x81,0x0B,
        '1','2','3','4','5','6','1','2','3','4','5'};
    rlen = openpgp_card_apdu(shrt, sizeof(shrt), rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80, "new PIN too short refused");
}
```
(PW3 variant: P2=0x83, min length 8.) Register; run red.

- [ ] **Step 2: Implement.** INS `0x24`, P1=0x00, P2=0x81/0x83: data = old‖new. Old-PIN check uses the same compare+retry logic as VERIFY (blocked → `6983`). Split: PW1 old length = current `s_pw1_len` — since lengths vary, match old by trying `s_pw1_len` as the split point; reject if `lc - s_pw1_len` < 6 (PW1) / < 8 (PW3) or > 16 → `6A80` (checked **before** comparing, no decrement on a malformed frame). On success store the new PIN+len, reset retries, clear verified flags, persist.
  Persistence: `static bool pin_persist(void)` saving `{s_pw1, s_pw1_len, s_pw1_retry, s_pw3, s_pw3_len, s_pw3_retry}` as NVS blob `"pgp_pins"` (`#ifndef TEST_HOST`, host no-op); `openpgp_card_load(void)` (new, declared in .h) loads `"pgp_pins"` + `"pgp_key"` — called from the Task-4 boot block in `main.c` (add the call: `openpgp_card_load();` after `openpgp_do_init();`). Call `pin_persist()` on every retry decrement, successful verify (retry reset), and PIN change.

- [ ] **Step 3: Run green.** `./scripts/check.sh --host-only`, then `./scripts/check.sh --board kase_dongle` (the NVS code now compiles on target).

- [ ] **Step 4: Commit.**
```bash
git add main/security/openpgp_card.c main/security/openpgp_card.h main/main.c test/test_openpgp_card.c
git commit -m "feat(pgp): CHANGE REFERENCE DATA + NVS persistence of PINs/retries/key (pgp_pins, pgp_key)"
```

---

### Task 7: `openpgp_crypto.c` — P-256 ECDSA sign (mbedtls, target-only)

**Files:**
- Create: `main/security/openpgp_crypto.{c,h}`
- Modify: `main/CMakeLists.txt` (uncomment the Task-1 line)

No host test (mbedtls not in the host harness — same status as `cr_hmac.c`); correctness is covered by an on-device KAT self-check + the Task 9 oracle (`gpg --verify`).

- [ ] **Step 1: Header.**

```c
/* main/security/openpgp_crypto.h — ECDSA P-256 signing for the OpenPGP applet (dongle). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Sign `hash` (any length 20..64; mbedtls truncates per FIPS 186) with private
 * scalar d (32 B, big-endian). Output: raw r||s, exactly 64 bytes (NOT DER —
 * the OpenPGP card format). Returns false on bad key / RNG / mbedtls error. */
bool openpgp_crypto_p256_sign(const uint8_t d[32],
                              const uint8_t *hash, uint16_t hash_len,
                              uint8_t sig[64]);

/* Sign-and-verify roundtrip with a fixed key+hash. Call once at boot; logs on fail. */
bool openpgp_crypto_selftest(void);
```

- [ ] **Step 2: Implementation.**

```c
/* main/security/openpgp_crypto.c */
#include "openpgp_crypto.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include <string.h>

static const char *TAG = "pgp_crypto";

/* mbedtls RNG callback backed by the HW TRNG (WiFi/BT not required for
 * esp_random on S3: the RF subsystem seeds SAR; entropy adequate per IDF docs
 * when WiFi enabled — the dongle has WiFi on for ESP-NOW). */
static int rng_cb(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

bool openpgp_crypto_p256_sign(const uint8_t d[32],
                              const uint8_t *hash, uint16_t hash_len,
                              uint8_t sig[64])
{
    if (!d || !hash || !sig || hash_len < 20 || hash_len > 64)
        return false;

    mbedtls_ecp_group grp;
    mbedtls_mpi r, s, dd;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&s); mbedtls_mpi_init(&dd);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1)) break;
        if (mbedtls_mpi_read_binary(&dd, d, 32)) break;
        /* reject d == 0 or d >= n (invalid scalar) */
        if (mbedtls_mpi_cmp_int(&dd, 0) <= 0 ||
            mbedtls_mpi_cmp_mpi(&dd, &grp.N) >= 0) break;
        if (mbedtls_ecdsa_sign(&grp, &r, &s, &dd, hash, hash_len,
                               rng_cb, NULL)) break;
        if (mbedtls_mpi_write_binary(&r, sig, 32)) break;
        if (mbedtls_mpi_write_binary(&s, sig + 32, 32)) break;
        ok = true;
    } while (0);

    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s);
    mbedtls_mpi_lset(&dd, 0);  /* scrub the scalar copy */
    mbedtls_mpi_free(&dd);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool openpgp_crypto_selftest(void)
{
    /* fixed scalar + fixed "hash"; verify the signature against d*G */
    static const uint8_t d[32] = {
        0xC9,0xAF,0xA9,0xD8,0x45,0xBA,0x75,0x16,0x6B,0x5C,0x21,0x57,0x67,0xB1,0xD6,0x93,
        0x4E,0x50,0xC3,0xDB,0x36,0xE8,0x9B,0x12,0x7B,0x8A,0x62,0x2B,0x12,0x0F,0x67,0x21,
    };  /* RFC 6979 A.2.5 test key */
    static const uint8_t h[32] = {
        0xAF,0x2B,0xDB,0xE1,0xAA,0x9B,0x6E,0xC1,0xE2,0xAD,0xE1,0xD6,0x94,0xF4,0x1F,0xC7,
        0x1A,0x83,0x1D,0x02,0x68,0xE9,0x89,0x15,0x62,0x11,0x3D,0x8A,0x62,0xAD,0xD1,0xBF,
    };  /* SHA-256("sample") */
    uint8_t sig[64];
    if (!openpgp_crypto_p256_sign(d, h, 32, sig)) {
        ESP_LOGE(TAG, "selftest: sign failed");
        return false;
    }
    mbedtls_ecp_group grp; mbedtls_ecp_point Q;
    mbedtls_mpi r, s, dd;
    mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&s); mbedtls_mpi_init(&dd);
    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1)) break;
        if (mbedtls_mpi_read_binary(&dd, d, 32)) break;
        if (mbedtls_ecp_mul(&grp, &Q, &dd, &grp.G, rng_cb, NULL)) break;
        if (mbedtls_mpi_read_binary(&r, sig, 32)) break;
        if (mbedtls_mpi_read_binary(&s, sig + 32, 32)) break;
        ok = (mbedtls_ecdsa_verify(&grp, h, 32, &Q, &r, &s) == 0);
    } while (0);
    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s); mbedtls_mpi_free(&dd);
    mbedtls_ecp_point_free(&Q); mbedtls_ecp_group_free(&grp);
    ESP_LOGI(TAG, "selftest: %s", ok ? "PASS" : "FAIL");
    return ok;
}
```

- [ ] **Step 3: Enable in CMake + build.** Uncomment `"security/openpgp_crypto.c"` in `main/CMakeLists.txt`. `./scripts/check.sh --board kase_dongle` → green (mbedtls already in `priv_requires`).

- [ ] **Step 4: Commit.**
```bash
git add main/security/openpgp_crypto.c main/security/openpgp_crypto.h main/CMakeLists.txt
git commit -m "feat(pgp): openpgp_crypto — ECDSA P-256 raw r||s via mbedtls + boot KAT selftest"
```

---

### Task 8: CCID worker task + real hooks (sign, sec_confirm UIF) + time extension

**The problem this solves:** `ccid_dispatch` runs inside `tud_task`. PSO:CDS with UIF blocks up to 15 s waiting for the touch — blocking `tud_task` stalls CDC + HID for the whole wait. Move `PC_to_RDR_XfrBlock` processing to a worker task; keep the trivial messages (PowerOn/Off/SlotStatus) inline. While the worker waits for the touch, send CCID **time-extension** frames (RDR_to_PC_DataBlock with `bStatus=0x80`, `bError=BWT multiplier`) so scdaemon keeps waiting.

**Files:**
- Modify: `main/security/ccid.c`

- [ ] **Step 1: Worker task + deferred response.**

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "sec_confirm.h"
#include "openpgp_crypto.h"

#define CCID_CONFIRM_SLOT   0xF0u   /* sec_confirm slot id for PGP (outside sec_store 0-3) */
#define CCID_WTX_PERIOD_MS  1500u

static TaskHandle_t      s_worker;
static SemaphoreHandle_t s_msg_ready;       /* binary: a XfrBlock awaits processing */
static volatile bool     s_busy;            /* one APDU in flight (bMaxCCIDBusySlots=1) */
static uint8_t  s_cur_slot, s_cur_seq;
static uint16_t s_resp_len;                 /* final response length in s_in_buf */
static uint8_t  s_wtx_buf[CCID_HDR_LEN];    /* separate buffer: WTX while s_in_buf builds */
static uint8_t  s_rhport;

/* --- called on the USB task via usbd_defer_func --- */
static void ccid_send_final_cb(void *param)
{
    (void)param;
    if (!usbd_edpt_xfer(s_rhport, s_ep_in, s_in_buf, s_resp_len))
        ESP_LOGE(TAG, "IN queue failed");
}
static void ccid_send_wtx_cb(void *param)
{
    (void)param;
    if (usbd_edpt_busy(s_rhport, s_ep_in)) return;  /* previous frame still in flight */
    memset(s_wtx_buf, 0, sizeof(s_wtx_buf));
    s_wtx_buf[0] = RDR_TO_PC_DATA_BLOCK;
    s_wtx_buf[5] = s_cur_slot;
    s_wtx_buf[6] = s_cur_seq;
    s_wtx_buf[7] = 0x80;   /* bmCommandStatus=10b: time extension requested */
    s_wtx_buf[8] = 0x02;   /* bError = BWT multiplier */
    usbd_edpt_xfer(s_rhport, s_ep_in, s_wtx_buf, CCID_HDR_LEN);
}

/* Public for the confirm hook: request more time from the host. */
static void ccid_request_time_extension(void)
{
    usbd_defer_func(ccid_send_wtx_cb, NULL, false);
}

static void ccid_worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_msg_ready, portMAX_DELAY);
        uint32_t dwLength = (uint32_t)s_out_buf[1] | ((uint32_t)s_out_buf[2] << 8);
        uint16_t apdu_n = openpgp_card_apdu(&s_out_buf[CCID_HDR_LEN],
                                            (uint16_t)dwLength,
                                            &s_in_buf[CCID_HDR_LEN],
                                            CCID_BUF_SZ - CCID_HDR_LEN);
        s_resp_len = ccid_fill_header(RDR_TO_PC_DATA_BLOCK, s_cur_slot, s_cur_seq, apdu_n);
        /* If a WTX frame is mid-flight, the IN xfer_cb fires soon and the EP
         * frees; retry briefly. */
        for (int i = 0; i < 100; i++) {
            if (!usbd_edpt_busy(s_rhport, s_ep_in)) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        usbd_defer_func(ccid_send_final_cb, NULL, false);
        s_busy = false;
    }
}
```
In `ccid_dispatch`: the `PC_TO_RDR_XFR_BLOCK` case becomes — clamp `dwLength` as today, save `s_cur_slot`/`s_cur_seq`/`s_rhport`, set `s_busy = true`, store the clamped length back into `s_out_buf[1..2]`, `xSemaphoreGive(s_msg_ready)`, and **return without re-priming OUT** (the OUT EP re-primes from `xfer_cb(s_ep_in)` after the *final* response — add `if (s_busy) return true;` guard in the IN branch of `ccid_drv_xfer` so a delivered WTX frame does NOT re-prime OUT early; only the final response does). Create the task + semaphore in `ccid_init()` (`xTaskCreate(ccid_worker, "ccid", 4096, NULL, 5, &s_worker)` — once, guard against double-init).

- [ ] **Step 2: Real hooks.** Replace the Phase-0 stubs:

```c
static bool dongle_sign(const uint8_t d[32], const uint8_t *hash, uint16_t n,
                        uint8_t *out, uint16_t *out_n)
{
    if (!openpgp_crypto_p256_sign(d, hash, n, out))
        return false;
    *out_n = 64;
    return true;
}

/* UIF gate: arm sec_confirm, poll for the touch, keep the host waiting with
 * WTX frames. Runs on the ccid worker task (NOT tud_task). */
static int dongle_confirm(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    sec_confirm_arm(CCID_CONFIRM_SLOT, now);
    uint32_t last_wtx = now;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        now = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t slot;
        sec_confirm_state_t st = sec_confirm_poll(now, &slot);
        if (st == SEC_CONFIRM_AUTHORIZED && slot == CCID_CONFIRM_SLOT) return 1;
        if (st == SEC_CONFIRM_TIMEDOUT) return 2;
        if (now - last_wtx >= CCID_WTX_PERIOD_MS) {
            ccid_request_time_extension();
            last_wtx = now;
        }
    }
}

static const openpgp_card_hooks_t s_dongle_hooks = {
    .sign = dongle_sign, .confirm = dongle_confirm,
};
```
`ccid_drv_init()` passes `s_dongle_hooks`. Also call `openpgp_crypto_selftest()` once from `ccid_init()` (log-only; don't brick the interface on FAIL).

- [ ] **Step 3: Build + flash + quick check.** `./scripts/check.sh --board kase_dongle`, flash, `gpg --card-status` still prints the card (regression check on the worker refactor: every CCID exchange now crosses the task boundary). Typing on the keyboard during a `--card-status` works (tud_task never blocks).

- [ ] **Step 4: Commit.**
```bash
git add main/security/ccid.c
git commit -m "feat(pgp): CCID worker task + WTX time extension + real sign/sec_confirm hooks (UIF touch)"
```

---

### Task 9: End-to-end — `keytocard`, touch, signed git commit + docs

**Files:**
- Modify: `docs/superpowers/specs/2026-06-09-dongle-openpgp-card-design.md` (§7 result)
- Modify: `docs/SECURITY_KEY.md` (OpenPGP section)

- [ ] **Step 1: Prerequisite — map `K_SEC_CONFIRM`.** The confirm key (0x3E00) must be on a half's keymap (KeSp_controller, or CDC `KS_CMD_KEYMAP_*`). Verify by arming a CR-style confirm or just proceed — the UIF wait window is 15 s.

- [ ] **Step 2: Import a signing key.** In `nix-shell -p gnupg`:

```bash
gpg --card-status                                   # card present
gpg --expert --full-generate-key                    # ECC (sign only) -> NIST P-256 -> no expiry
gpg --edit-key <KEYID>                              # then: keytocard -> Signature key -> PW3 (12345678)
gpg --card-status                                   # Signature key shows the fingerprint
```
Watch for: `keytocard` exercises PUT DATA `0xDB 3FFF` + `C7` + `CE`. On failure, scdaemon log (`/tmp/scd.log`) shows the offending APDU + our SW. Fix → host test → reflash.

- [ ] **Step 3: Sign with touch.**

```bash
echo test | gpg --clearsign      # gpg prompts PW1 (123456), then BLOCKS -> touch K_SEC_CONFIRM
gpg --verify                     # on the produced output: Good signature
```
Timeout path: wait 15 s without touching → gpg fails with "conditions of use not satisfied" (6985), card still alive (next sign works).

- [ ] **Step 4: Signed git commit.**

```bash
git config --local user.signingkey <KEYID>
git commit --amend -S --no-edit          # on a scratch branch, or a scratch repo
git log --show-signature -1              # Good signature
```
Reboot the dongle (replug) → `gpg --card-status` still shows fingerprints + a nonzero signature counter (NVS persistence proof). PIN retry counters survive too.

- [ ] **Step 5: Smoke test.** Keyboard typing (halves → HID), KeSp_controller CDC connect, `kase-sec` still provisions (CDC SEC commands personality-independent). No regression on V2: `./scripts/check.sh` (6 boards).

- [ ] **Step 6: Record + docs + commit.**
  - Spec: add **§7b Phase 1 — RESULT** (what passed, gotchas found, SW quirks scdaemon showed).
  - `docs/SECURITY_KEY.md`: OpenPGP personality section — Kconfig flag, default PINs (and that they were changed), keytocard procedure, UIF behavior, host requirements (none beyond udev), CR-HMAC now behind `KASE_SEC_OTP_HID`.
```bash
git add docs/ && git commit -m "docs(pgp): Phase 1 result — gpg --sign + signed git commit with touch, e2e validated"
```

---

## Risks / open points carried into execution

1. **scdaemon's exact GET DATA sequence** — Task 4 is the oracle; the 6E assembly is where it'll bite. Budget an iteration loop there (host test each fix).
2. **WTX handling by scdaemon's internal driver** — believed supported (`ccid-driver.c` handles time extension); if not, fallback = respond to PSO:CDS only after touch *without* WTX and rely on scdaemon's default bulk timeout (~13 s in some builds) — shrink `SEC_CONFIRM_TIMEOUT_MS` if needed.
3. **`usbd_edpt_busy` race between WTX and final response** — mitigated by the retry loop + the `s_busy` guard on OUT re-prime; if frames interleave wrongly, serialize by skipping WTX entirely (simplest degraded mode).
4. **gpg may PUT DATA C1 during keytocard** (algo attrs, since C0 declares them changeable) — generic DA path stores it; harmless. If gpg instead *requires* them immutable-and-matching, our factory C1 already matches P-256.
5. **`esp_random` entropy** — WiFi is enabled on the dongle (ESP-NOW), so the TRNG is properly seeded. If ESP-NOW ever gets disabled, revisit (use `mbedtls_ecdsa_sign_det` / RFC 6979 deterministic instead — also removes the RNG dependency entirely; consider it opportunistically in Task 7 if `MBEDTLS_ECDSA_DETERMINISTIC` is on in the dongle sdkconfig).

## Self-review notes (done while writing)

- **Spec §7 coverage:** CCID transport hardening (worker, WTX) → Task 8; essential DOs + constructed → Task 3; VERIFY/CHANGE REFERENCE DATA → Tasks 2/6; PSO:CDS sig-slot-only → Task 5; PUT DATA import → Task 5; algorithm fallback P-256 → verdict in header + Task 7; PIN state → Tasks 2/6; UIF → Tasks 3 (D6 default ON)/8; key store → Task 5 (dedicated `pgp_key` blob, not a sec_store slot — different provisioning flow); end state (`--card-status` + `gpg --sign` + touch) → Tasks 4/9. Phase-0 open design decision (OTP vs CCID coexistence) → Task 1 (build-flag mutual exclusion, ratified).
- **Type consistency:** hook signature changes once (Task 5) and `ccid.c` is updated in the same task; `openpgp_card_factory_reset/ensure_defaults/set_serial/load/key_is_set` declared where introduced and used consistently after.
- **No placeholders:** every code step carries real code; the two hardware checkpoints (Tasks 4, 9) are explicitly iterate-with-oracle spikes with diagnosis commands, mirroring the Phase-0 method that worked.
- **Order respects the ratified sequence:** flag (1) → card visible (2-4) → crypto+sign (5-9).
