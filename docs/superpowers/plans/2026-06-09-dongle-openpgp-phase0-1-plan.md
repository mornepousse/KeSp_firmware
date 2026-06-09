# Dongle OpenPGP Card — Phase 0 (CCID de-risk) + Phase 1 foundation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax. **Run in a fresh session** — this plan was written at the end of a very long session; the CCID work is the finickiest USB code in the project and deserves clean context.

**Goal:** (Phase 0) Prove GnuPG's scdaemon enumerates our dongle as a CCID reader at VID `0x303a` via USB class `0x0B` — eliminating the #1 unknown. (Phase 1 foundation) Build the host-testable pure logic (APDU parser, OpenPGP Data-Object store, applet state machine) that the signing card will use.

**Architecture:** A custom TinyUSB CCID class (no built-in support → `usbd_app_driver_get_cb`) carries APDUs to an OpenPGP applet. Phase 0 stands up the CCID transport + a stub applet (answer SELECT) just enough for scdaemon to see the card. Phase 1 builds the applet's pure logic (parse/DO/state-machine) host-tested, with crypto + full USB wiring deferred to Phase 1-integration / Phase 2.

**Tech Stack:** C, ESP-IDF 5.5, TinyUSB custom class, mbedtls (later), the host test harness. All dongle-gated (`CONFIG_KASE_DEVICE_ROLE_DONGLE`). Spec: `docs/superpowers/specs/2026-06-09-dongle-openpgp-card-design.md`.

---

## Prerequisites (host, one-time)

- **GnuPG is NOT installed on the dev machine.** Use an ephemeral shell for testing:
  `nix-shell -p gnupg` (gives `gpg`/`gpg-agent`/`scdaemon` without a NixOS config change).
- **USB access:** the dongle's USB device is already `0666`/`plugdev` via the existing
  `99-local.rules` (`SUBSYSTEM=="usb", ATTRS{idVendor}=="303a", ...`), so scdaemon's libusb
  CCID driver can open it. No new udev rule needed for Phase 0.
- **Force scdaemon's internal CCID driver** (avoid pcscd/libccid Info.plist issues — pcscd isn't
  even installed here): create `~/.gnupg/scdaemon.conf` with `disable-ccid` UNSET (internal is
  default) and, if needed, `debug-level guru` + `log-file /tmp/scd.log` for diagnosis.
- **Flashing:** `idf.py -B build_kase_dongle -p /dev/ttyUSB0 flash` (CH340; NVS preserved).
  Dongle CDC = `/dev/ttyACM1`.

## File structure

- Create `main/security/ccid.{c,h}` — TinyUSB CCID custom class: class descriptor + bulk
  framing (CCID message header parse, XfrBlock→APDU, response→DataBlock). One responsibility:
  USB bulk ↔ APDU bytes.
- Create `main/security/openpgp_card.{c,h}` — applet: APDU dispatch, DO store, PIN state, UIF
  gate. Phase 0 = stub (SELECT → 0x9000 + AID). Phase 1 grows the pure logic.
- Create `main/security/apdu.{c,h}` — pure ISO 7816-4 APDU parse (host-tested).
- Create `main/security/openpgp_do.{c,h}` — pure DO get/put store (host-tested).
- Modify `main/comm/usb/usb_hid.c` — add the CCID interface to the dongle composite descriptor
  (`hid_cdc_otp_configuration_descriptor`, the dongle path selected in `tinyusb_hid_init()`),
  register the CCID class driver, bump interface/endpoint counts.
- Modify `main/CMakeLists.txt` — add the new sources (dongle-gated). Tests in `test/`.

---

## PHASE 0 — CCID de-risk (spike: hardware-iterated, not pure TDD)

> Phase 0 answers "does scdaemon see us via class 0x0B at VID 0x303a". The CCID descriptor and
> bulk framing are finicky and validated against `gpg --card-status`, not host tests. Expect a
> build→flash→test→fix loop. The code below is the concrete starting point.

### Task 0.1: CCID class descriptor + interface in the composite

**Files:** Modify `main/comm/usb/usb_hid.c` (dongle config descriptor + endpoint numbers).

- [ ] **Step 1: Add the CCID functional descriptor + interface.** After the OTP HID interface in
  `hid_cdc_otp_configuration_descriptor[]`, append a CCID interface (interface number 4) and its
  54-byte CCID class descriptor. Allocate `EPNUM_CCID_OUT = 0x05`, `EPNUM_CCID_IN = 0x85` (verify
  the ESP32-S3 FS endpoint budget: CDC EP1/EP2, HID EP3, OTP-HID EP4, CCID EP5 — 5 IN + 2 OUT,
  within FS limits; if tight, drop the OTP-HID interrupt EP or share). Bump `TUD_CONFIG_DESCRIPTOR`
  interface count 4→5 and update the total-length macro.

```c
/* CCID functional descriptor (USB CCID Rev 1.1 §5.1) — single slot, T=1, short+extended APDU.
 * Cross-check every field against the spec and a real reader (e.g. a YubiKey) before trusting. */
#define KASE_CCID_DESC \
    0x36, 0x21,                 /* bLength=54, bDescriptorType=0x21 (CCID) */ \
    0x10, 0x01,                 /* bcdCCID = 1.10 */ \
    0x00,                       /* bMaxSlotIndex = 0 (one slot) */ \
    0x07,                       /* bVoltageSupport = 5.0/3.0/1.8 */ \
    0x02, 0x00, 0x00, 0x00,     /* dwProtocols = T=1 */ \
    0xA0, 0x0F, 0x00, 0x00,     /* dwDefaultClock = 4000 kHz */ \
    0xA0, 0x0F, 0x00, 0x00,     /* dwMaximumClock = 4000 kHz */ \
    0x00,                       /* bNumClockSupported = 0 (only default) */ \
    0x80, 0x25, 0x00, 0x00,     /* dwDataRate = 9600 */ \
    0x80, 0x25, 0x00, 0x00,     /* dwMaxDataRate = 9600 */ \
    0x00,                       /* bNumDataRatesSupported = 0 */ \
    0xFE, 0x00, 0x00, 0x00,     /* dwMaxIFSD = 254 */ \
    0x00, 0x00, 0x00, 0x00,     /* dwSynchProtocols = 0 */ \
    0x00, 0x00, 0x00, 0x00,     /* dwMechanical = 0 */ \
    0x40, 0x08, 0x04, 0x00,     /* dwFeatures = auto-conf(0x40)|auto-voltage|short+ext APDU(0x40000)|TPDU? — see note */ \
    0x0F, 0x01, 0x00, 0x00,     /* dwMaxCCIDMessageLength = 271 (short) — raise for extended */ \
    0xFF,                       /* bClassGetResponse = echo */ \
    0xFF,                       /* bClassEnvelope = echo */ \
    0x00, 0x00,                 /* wLcdLayout = none */ \
    0x00,                       /* bPINSupport = 0 (no pinpad; PIN over APDU) */ \
    0x01                        /* bMaxCCIDBusySlots = 1 */
```
> NOTE: `dwFeatures` is the field scdaemon is pickiest about. For a software card the common
> choice is "Short and Extended APDU level exchange" (`0x00040000`) plus automatic
> parameter/clock/baud/voltage negotiation bits. If scdaemon rejects the reader, this field is
> the first suspect — compare against `lsusb -v` of a working YubiKey and the CCID spec §5.1.

- [ ] **Step 2: Build.** `source ~/esp/esp-idf/export.sh && ./scripts/check.sh --board kase_dongle`
  → host green + `Build kase_dongle OK`. (No behavior yet; just descriptor compiles.) Also build
  `kase_v2` to confirm the dongle-gated change didn't break keyboards.

- [ ] **Step 3: Commit.** `git add main/comm/usb/usb_hid.c && git commit -m "feat(pgp): add CCID interface descriptor to dongle composite (Phase 0)"`

### Task 0.2: Minimal CCID class driver (bulk framing)

**Files:** Create `main/security/ccid.{c,h}`; register it in `usb_hid.c` via the TinyUSB
application driver hook.

- [ ] **Step 1: Implement the class driver skeleton.** `ccid.c` provides
  `const usbd_class_driver_t *ccid_app_driver(uint8_t *count)` returning a driver with
  `open`/`control_xfer_cb`/`xfer_cb` callbacks, and TinyUSB picks it up via
  `usbd_app_driver_get_cb()` (implement that function to return the CCID driver, or merge with an
  existing app-driver hook). The driver claims the CCID interface, opens the bulk IN/OUT
  endpoints, and on bulk OUT reassembles a CCID message (10-byte header + payload), dispatches by
  `bMessageType`, and queues a response on bulk IN. Minimal message handling:
  - `PC_to_RDR_IccPowerOn` (0x62) → `RDR_to_PC_DataBlock` (0x80) carrying a fixed **ATR**
    (e.g. `3B DA 18 FF 81 B1 FE 75 1F 03 00 31 84 73 80 01 80 00 90 00 ...` — use a minimal valid
    T=1 ATR; or copy a known OpenPGP card ATR and adapt).
  - `PC_to_RDR_IccPowerOff` (0x63) → `RDR_to_PC_SlotStatus` (0x81).
  - `PC_to_RDR_GetSlotStatus` (0x65) → `RDR_to_PC_SlotStatus` (0x81), bStatus=0 (present, active).
  - `PC_to_RDR_XfrBlock` (0x6F) → pass the APDU to `openpgp_card_apdu(in, len, out, &out_len)`,
    wrap result in `RDR_to_PC_DataBlock` (0x80), echo `bSeq`.
  Keep buffers static (no malloc). The header layout, `bSeq` echo, and `bStatus`/`bError` fields
  must follow USB CCID Rev 1.1 §6.1–6.2.

- [ ] **Step 2: Stub applet** in `openpgp_card.c`: `openpgp_card_apdu()` answers `SELECT`
  (CLA=00 INS=A4 P1=04, data = AID `D2 76 00 01 24 01 ...`) with status `0x90 0x00` and (optional)
  the AID DO; everything else → `0x6D00` (instruction not supported). Enough for scdaemon to
  select the applet.

- [ ] **Step 3: Build + flash.** `./scripts/check.sh --board kase_dongle` green, then
  `idf.py -B build_kase_dongle -p /dev/ttyUSB0 flash`.

- [ ] **Step 4: THE DE-RISK TEST.** With the dongle enumerated:
  ```bash
  nix-shell -p gnupg --run 'gpg --card-status'
  # if it fails, capture scdaemon debug:
  nix-shell -p gnupg --run 'echo "debug-level guru" > ~/.gnupg/scdaemon.conf; echo "log-file /tmp/scd.log" >> ~/.gnupg/scdaemon.conf; gpgconf --kill scdaemon; gpg --card-status; cat /tmp/scd.log'
  ```
  **Expected (success):** scdaemon opens the reader and `gpg --card-status` prints a card (even
  if mostly empty/stub). **This proves VID 0x303a is accepted via class 0x0B.** If scdaemon
  doesn't see the reader: inspect `/tmp/scd.log` — usual causes are a malformed CCID descriptor
  (fix `dwFeatures`/lengths in Task 0.1) or a bad CCID message reply (fix framing). Iterate.

- [ ] **Step 5: Commit + record the verdict** in the spec (Phase 0 result).
  `git add main/security/ccid.* main/security/openpgp_card.* main/CMakeLists.txt main/comm/usb/usb_hid.c && git commit -m "feat(pgp): minimal CCID class driver + SELECT stub — scdaemon enumerates the card (Phase 0)"`

---

## PHASE 1 — host-testable applet foundation (TDD, parallelizable with Phase 0)

These units are pure logic with no USB/crypto dependency, so they get clean TDD coverage in the
host harness (`test/`, `test_framework.h`) and can be built before the transport is solid.

### Task 1.1: ISO 7816-4 APDU parser (`apdu.c`)

**Files:** Create `main/security/apdu.{c,h}`; Test `test/test_apdu.c`.

- [ ] **Step 1: Failing test** — `test/test_apdu.c`. Parse a command APDU into
  `{cla, ins, p1, p2, lc, data*, le}`, handling case 1 (no data, no Le), case 2 (Le only),
  case 3 (Lc+data), case 4 (Lc+data+Le), and extended-length (3-byte Lc/Le).
```c
#include "test_framework.h"
#include "apdu.h"
static void test_case3_short(void)
{
    /* 00 A4 04 00 06 D2 76 00 01 24 01  → SELECT, Lc=6, no Le */
    uint8_t b[] = {0x00,0xA4,0x04,0x00,0x06,0xD2,0x76,0x00,0x01,0x24,0x01};
    apdu_t a;
    TEST_ASSERT(apdu_parse(b, sizeof(b), &a), "case3 parses");
    TEST_ASSERT_EQ(a.ins, 0xA4, "ins"); TEST_ASSERT_EQ(a.lc, 6, "lc");
    TEST_ASSERT_EQ(a.data[0], 0xD2, "data0"); TEST_ASSERT_EQ(a.le, 0, "no le");
}
static void test_extended_le(void)
{
    /* 00 CA 00 6E 00 00 00 → GET DATA, extended Le=0 (=65536) */
    uint8_t b[] = {0x00,0xCA,0x00,0x6E,0x00,0x00,0x00};
    apdu_t a;
    TEST_ASSERT(apdu_parse(b, sizeof(b), &a), "ext Le parses");
    TEST_ASSERT_EQ(a.lc, 0, "no lc"); TEST_ASSERT(a.le_present, "le present");
}
void test_apdu(void)
{
    TEST_SUITE("apdu parser");
    TEST_RUN(test_case3_short);
    TEST_RUN(test_extended_le);
}
```
- [ ] **Step 2: Register + run red** (add to `test/CMakeLists.txt` + `test_main.c`). FAIL.
- [ ] **Step 3: Implement `apdu.{c,h}`** — `typedef struct { uint8_t cla,ins,p1,p2; uint16_t lc; const uint8_t *data; uint32_t le; bool le_present; } apdu_t;` and `bool apdu_parse(const uint8_t *buf, uint16_t len, apdu_t *out)` covering the 4 ISO cases + extended length (first Lc byte 0x00 ⇒ 3-byte extended fields). Return false on malformed.
- [ ] **Step 4: Run** → PASS. **Step 5: Commit** `feat(pgp): ISO7816-4 APDU parser + host tests`.

### Task 1.2: OpenPGP Data-Object store (`openpgp_do.c`)

**Files:** Create `main/security/openpgp_do.{c,h}`; Test `test/test_openpgp_do.c`.

- [ ] **Step 1: Failing test** — store/fetch DOs by tag (e.g. AID 0x4F, fingerprints 0xC5, algo
  attrs 0xC1/2/3, UIF 0xD6/7/8, name 0x5B, url 0x5F50). Test: put a DO, get it back; get an unset
  DO returns not-found; bounds on max DO size.
```c
#include "test_framework.h"
#include "openpgp_do.h"
static void test_put_get(void)
{
    openpgp_do_reset();
    uint8_t fp[20]; memset(fp, 0xAB, 20);
    TEST_ASSERT(openpgp_do_put(0xC5, fp, 20), "put C5");
    const uint8_t *v; uint16_t n;
    TEST_ASSERT(openpgp_do_get(0xC5, &v, &n), "get C5");
    TEST_ASSERT_EQ(n, 20, "len"); TEST_ASSERT(memcmp(v, fp, 20) == 0, "value");
    TEST_ASSERT(!openpgp_do_get(0x5B, &v, &n), "unset DO not found");
}
void test_openpgp_do(void)
{
    TEST_SUITE("openpgp DO store");
    TEST_RUN(test_put_get);
}
```
- [ ] **Step 2: Register + run red.** FAIL.
- [ ] **Step 3: Implement** a fixed-table DO store (tag→bytes, fixed-size backing buffers; NVS
  persistence behind `#ifndef TEST_HOST` like `sec_store`). `bool openpgp_do_put(uint16_t tag, const uint8_t *v, uint16_t n)`, `bool openpgp_do_get(uint16_t tag, const uint8_t **v, uint16_t *n)`, `void openpgp_do_reset(void)`.
- [ ] **Step 4: Run** → PASS. **Step 5: Commit** `feat(pgp): OpenPGP DO store + host tests`.

### Task 1.3: Applet state machine (PIN + dispatch + UIF gate)

**Files:** Create/extend `main/security/openpgp_card.{c,h}`; Test `test/test_openpgp_card.c`.

- [ ] **Step 1: Failing test** — drive APDUs through `openpgp_card_apdu()` with injected hooks
  (a fake "sign" and a fake "confirm", mirroring the otp_proto pattern). Assert: SELECT → 0x9000;
  PSO:CDS before VERIFY(PW1) → `0x6982` (security status not satisfied); after VERIFY ok and with
  UIF enabled, PSO:CDS arms the confirm and only returns the signature once authorized; wrong PIN
  decrements the retry counter and returns `0x63Cx`.
```c
#include "test_framework.h"
#include "openpgp_card.h"
/* fake hooks injected via openpgp_card_init(&hooks) — sign returns a canned sig;
   confirm_state returns 0 pending / 1 authorized */
... (full test: build APDUs with apdu helpers, feed, assert status words + gating) ...
void test_openpgp_card(void)
{
    TEST_SUITE("openpgp applet");
    TEST_RUN(test_select_ok);
    TEST_RUN(test_sign_requires_pin);
    TEST_RUN(test_sign_uif_gate);
    TEST_RUN(test_pin_retry_counter);
}
```
- [ ] **Step 2: Register + run red.** FAIL.
- [ ] **Step 3: Implement** the applet dispatch over `apdu_parse` + `openpgp_do`, with PW1/PW3
  state + retry counters, the command table (SELECT / GET DATA / VERIFY / CHANGE REFERENCE DATA /
  PSO:CDS / PUT DATA), and the UIF gate calling the injected `confirm` hook (returns `0x6985`/`0x6982`
  on deny/unauth). Crypto (PSO:CDS) is an injected hook here; the real mbedtls wiring is Phase
  1-integration (target). Keep the dispatch + status-word logic pure and host-tested.
- [ ] **Step 4: Run** → PASS. **Step 5: Commit** `feat(pgp): OpenPGP applet state machine (dispatch+PIN+UIF) + host tests`.

---

## Phase 1-integration & Phase 2 (target, separate session)

After Phase 0 proves enumeration and Phase 1 host logic is green: wire `ccid.c` →
`openpgp_card_apdu` → real crypto (`openpgp_crypto.c`, mbedtls — **verify Ed25519 support first;
else NIST P-256, else TweetNaCl**) → `sec_store` (key blob) → `sec_confirm` (real UIF gate);
import a key via PUT DATA; `gpg --sign` end-to-end with a touch. Then Phase 2 (decrypt/auth/keygen,
RSA key blob, full DOs) per the spec.

## Self-review notes (done while writing)

- **Spec coverage:** spec §6 (Phase 0) → Tasks 0.1–0.2; §4 units → file structure; §7 applet
  commands → Task 1.3; §9 testing (APDU/DO/SM host-tested) → Tasks 1.1–1.3; §10 open items echoed
  (Ed25519, CCID descriptor `dwFeatures`, extended-length, endpoint budget, gpg-not-installed).
- **Spike honesty:** Phase 0 is explicitly a hardware-iterated spike (CCID descriptor + framing
  can't be host-TDD'd); concrete starting code given + the `gpg --card-status` oracle. Phase 1
  pure logic IS clean TDD.
- **Type consistency:** `apdu_t`, `apdu_parse`, `openpgp_do_put/get/reset`, `openpgp_card_apdu`,
  `openpgp_card_init(hooks)` used consistently; `ccid.c` calls `openpgp_card_apdu`.
- **Known unknowns flagged inline, not hidden:** the ATR bytes, `dwFeatures`, and endpoint budget
  are marked "cross-check/verify" — they are the spike's whole point, not placeholders for normal code.
