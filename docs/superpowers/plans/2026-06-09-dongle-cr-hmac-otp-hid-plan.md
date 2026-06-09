# Dongle CR-HMAC + YubiKey OTP HID Transport — Implementation Plan (Plan 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax. **Start in a fresh session** — this plan was written after a large brainstorm/Plan-1 session.

**Goal:** Make the dongle answer YubiKey-compatible HMAC-SHA1 challenge-response over USB, gated by a physical `K_SEC_CONFIRM` keypress, so a (VID-patched) KeePassXC unlocks a database with the dongle.

**Architecture:** Build on Plan 1's foundation (`sec_store`, `sec_confirm`, `K_SEC_CONFIRM`). Add: pure `cr_crc16` (YubiKey CRC-16), `cr_hmac` (HMAC-SHA1 via mbedtls, target), a pure `otp_proto` state machine (frame reassembly + status-byte protocol + response chunking), and a target-only `otp_hid` TinyUSB transport wiring the state machine to SET_REPORT/GET_REPORT on a new HID interface. Host side: a 5-line KeePassXC VID/PID patch (OnlyKey precedent).

**Tech Stack:** C, ESP-IDF 5.5, TinyUSB (feature reports on EP0), mbedtls (HMAC-SHA1), host test harness. All dongle-gated (`CONFIG_KASE_DEVICE_ROLE_DONGLE`).

**Spec:** `docs/superpowers/specs/2026-06-08-dongle-security-key-foundation-cr-hmac-design.md` (§5.3, §5.5).
**Spike findings (authoritative protocol facts, with sources):** see "Protocol reference" at the end of this plan.

---

## Threat-model / compatibility note (decided)

- Host strategy = **Firmware + KeePassXC VID patch**. Dongle keeps VID `0x303a` / PID `0x4001`. Stock KeePassXC/ykpers filter by Yubico VID `0x1050` and will NOT see us; a 5-line patch to `YubiKeyInterfaceUSB.cpp` (Task 7) adds our VID/PID, following the merged OnlyKey precedent (KeePassXC commit `e4326fb`). VID spoofing (advertising 0x1050) was rejected (USB-IF/legal/trust).
- Response is released ONLY after a physical `K_SEC_CONFIRM` — mapped onto the YubiKey "touch-required" semantics: hold `RESP_TIMEOUT_WAIT_FLAG` until the keypress, then `RESP_PENDING_FLAG` + response. KeePassXC blocks up to 256 s with a "Touch" prompt.

## File structure

- Create `main/security/cr_crc16.{c,h}` — YubiKey CRC-16 (pure).
- Create `main/security/cr_hmac.{c,h}` — HMAC-SHA1 (mbedtls; thin).
- Create `main/security/otp_proto.{c,h}` — pure protocol state machine (frame reassembly, status byte, response chunking; calls cr_crc16, and via a function pointer the HMAC + sec_confirm).
- Create `main/security/otp_hid.{c,h}` — target-only TinyUSB transport (descriptor + report callbacks), wires otp_proto ↔ sec_store/sec_confirm/cr_hmac.
- Modify `main/comm/usb/usb_hid.c` + tinyusb descriptors — add the OTP HID interface.
- Modify `main/CMakeLists.txt` — register sources (dongle-gated; cr_crc16/otp_proto host-tested).
- Tests: `test/test_cr_crc16.c`, `test/test_otp_proto.c` (+ harness wiring).
- Host repo (KeePassXC): documented patch in `docs/keepassxc-kase-vid.patch` (Task 7).

---

## Task 1: YubiKey CRC-16 (`cr_crc16`)

**Files:** Create `main/security/cr_crc16.{c,h}`; Test `test/test_cr_crc16.c`.

- [ ] **Step 1: Failing test** — `test/test_cr_crc16.c`. YubiKey CRC-16 is CRC-16/CCITT, init `0xFFFF`, poly `0x8408` (reflected 0x1021), processed LSB-first per ykpers `ykcore/crc16.c`. The validity residual over (data+appended CRC) is `0xF0B8`.

```c
#include "test_framework.h"
#include "cr_crc16.h"

static void test_crc16_empty(void)
{
    /* CRC-16 of zero bytes = init value 0xFFFF (no bytes processed) */
    TEST_ASSERT_EQ(cr_crc16(NULL, 0), 0xFFFF, "crc16 of empty = 0xFFFF");
}

static void test_crc16_residual(void)
{
    /* A valid YubiKey frame's CRC, when the 2 CRC bytes are themselves
     * included in the computation, yields the constant residual 0xF0B8. */
    uint8_t data[4] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = cr_crc16(data, 4);
    uint8_t framed[6] = {0x01, 0x02, 0x03, 0x04,
                         (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8)};
    TEST_ASSERT_EQ(cr_crc16(framed, 6), 0xF0B8, "crc16 residual over data+crc = 0xF0B8");
}

void test_cr_crc16(void)
{
    TEST_SUITE("cr_crc16 (YubiKey CRC-16)");
    TEST_RUN(test_crc16_empty);
    TEST_RUN(test_crc16_residual);
}
```

- [ ] **Step 2: Register + run red.** Add `test_cr_crc16.c` + `../main/security/cr_crc16.c` to `test/CMakeLists.txt`; declare+call `test_cr_crc16` in `test/test_main.c`. Run `./scripts/check.sh --host-only` → FAIL.

- [ ] **Step 3: Implement** — port ykpers `ykcore/crc16.c` exactly. `main/security/cr_crc16.h`:
```c
#pragma once
#include <stdint.h>
uint16_t cr_crc16(const uint8_t *data, uint16_t len);  /* YubiKey CRC-16, init 0xFFFF */
```
`main/security/cr_crc16.c`:
```c
#include "cr_crc16.h"

uint16_t cr_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            uint16_t lsb = crc & 1;
            crc >>= 1;
            if (lsb) crc ^= 0x8408;
        }
    }
    return crc;
}
```

- [ ] **Step 4: Run** → PASS. If `0xF0B8` residual fails, the init/poll/reflection is wrong — re-check against ykpers `crc16.c` (do NOT guess; the residual test is the oracle).

- [ ] **Step 5: Commit**
```bash
git add main/security/cr_crc16.h main/security/cr_crc16.c test/test_cr_crc16.c test/CMakeLists.txt test/test_main.c
git commit -m "feat(sec): YubiKey CRC-16 (cr_crc16) + host tests"
```

---

## Task 2: HMAC-SHA1 (`cr_hmac`, mbedtls)

**Files:** Create `main/security/cr_hmac.{c,h}`.

> **Host-test note:** the test harness does not link mbedtls, so `cr_hmac` is NOT host-tested here. Its correctness is validated (a) by Task 5's on-target known-answer test against RFC 2202 vectors, and (b) by the Task 7 KeePassXC interop. The host-testable protocol logic (CRC, frame parse, status SM) lives in `otp_proto` (Task 3) and is fully covered. This split keeps the host harness mbedtls-free.

- [ ] **Step 1: Header** — `main/security/cr_hmac.h`:
```c
#pragma once
#include <stdint.h>
/* HMAC-SHA1(key,msg) -> out20 (20 bytes). Returns true on success. */
bool cr_hmac_sha1(const uint8_t *key, uint16_t key_len,
                  const uint8_t *msg, uint16_t msg_len, uint8_t out20[20]);
```

- [ ] **Step 2: Implement** — `main/security/cr_hmac.c` (target build pulls mbedtls transitively; no new dependency per Plan-1 spike):
```c
#include "cr_hmac.h"
#include <stdbool.h>
#include "mbedtls/md.h"

bool cr_hmac_sha1(const uint8_t *key, uint16_t key_len,
                  const uint8_t *msg, uint16_t msg_len, uint8_t out20[20])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!info) return false;
    return mbedtls_md_hmac(info, key, key_len, msg, msg_len, out20) == 0;
}
```

- [ ] **Step 3: Build check** — this file is dongle-only (added to CMake in Task 6). It can't be exercised until then; verify it compiles at Task 6's dongle build. Commit with the header now (no behavior to test on host):
```bash
git add main/security/cr_hmac.h main/security/cr_hmac.c
git commit -m "feat(sec): HMAC-SHA1 wrapper (cr_hmac, mbedtls)"
```

---

## Task 3: OTP protocol state machine (`otp_proto`, pure)

This is the heart of the feature and the main host-tested unit. It is a pure state machine: it is fed inbound feature-report writes (SET_REPORT) and produces the status/response bytes for outbound reads (GET_REPORT). It does NOT call mbedtls or TinyUSB directly — the HMAC computation and the keypress gate are injected so the logic stays host-testable.

**Files:** Create `main/security/otp_proto.{c,h}`; Test `test/test_otp_proto.c`.

- [ ] **Step 1: Header** — `main/security/otp_proto.h`:
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Status byte flags (YubiKey OTP HID protocol) */
#define OTP_SLOT_WRITE_FLAG        0x80
#define OTP_RESP_PENDING_FLAG      0x40
#define OTP_RESP_TIMEOUT_WAIT_FLAG 0x20

#define OTP_FEATURE_RPT_SIZE 8     /* 7 data + 1 seq/flag */
#define OTP_FRAME_SIZE       70    /* 64 payload + slot + crc16 + 3 filler */
#define OTP_SLOT_CHAL_HMAC1  0x30
#define OTP_SLOT_CHAL_HMAC2  0x38
#define OTP_RESP_LEN         20    /* HMAC-SHA1 digest */

/* Injected dependencies (keeps otp_proto pure + host-testable):
 *  - compute_hmac(slot, challenge[64], out20) -> true if a configured slot
 *    answered (false = no such slot). On target: looks up sec_store +
 *    cr_hmac_sha1. In tests: a fake.
 *  - confirm_state() -> 0 = no confirm yet (keep waiting), 1 = authorized,
 *    2 = denied/timeout. On target: drives sec_confirm. In tests: a fake. */
typedef struct {
    bool (*compute_hmac)(uint8_t slot, const uint8_t challenge[64], uint8_t out20[20]);
    int  (*confirm_state)(void);
    void (*confirm_arm)(uint8_t slot);   /* called when a valid challenge arrives */
} otp_proto_hooks_t;

void otp_proto_init(const otp_proto_hooks_t *hooks);
void otp_proto_reset(void);
/* Host wrote one 8-byte feature report (SET_REPORT). */
void otp_proto_on_write(const uint8_t report[OTP_FEATURE_RPT_SIZE]);
/* Host reads one 8-byte feature report (GET_REPORT): fill it, advancing state. */
void otp_proto_on_read(uint8_t report[OTP_FEATURE_RPT_SIZE]);
```

- [ ] **Step 2: Failing test** — `test/test_otp_proto.c`. Drive a full HMAC1 challenge-response with a fake HMAC + fake confirm, asserting: (a) write-flag clears between chunks, (b) while confirm pending, reads return RESP_TIMEOUT_WAIT, (c) after confirm authorizes, reads return RESP_PENDING + the 20-byte response across 3 reports, (d) bad CRC frame is rejected. (Full test body: build a 70-byte frame with `cr_crc16`, slot=0x30, a known 64-byte challenge; split into 10 reports; feed; poll reads.) Use `cr_crc16` (link it into this test too). Assert the response bytes equal the fake HMAC output.

```c
#include "test_framework.h"
#include "otp_proto.h"
#include "cr_crc16.h"
#include <string.h>

static uint8_t  g_fake_resp[20];
static int      g_confirm = 0;     /* 0 pending, 1 authorized */
static uint8_t  g_armed_slot = 0xFF;

static bool fake_hmac(uint8_t slot, const uint8_t ch[64], uint8_t out[20])
{ (void)slot; (void)ch; memcpy(out, g_fake_resp, 20); return true; }
static int  fake_confirm(void) { return g_confirm; }
static void fake_arm(uint8_t slot) { g_armed_slot = slot; }

static void feed_frame(uint8_t slot, const uint8_t challenge[64])
{
    uint8_t frame[OTP_FRAME_SIZE];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, challenge, 64);
    frame[64] = slot;
    uint16_t crc = cr_crc16(frame, 65);          /* CRC over payload+slot */
    frame[65] = (uint8_t)(crc & 0xFF);
    frame[66] = (uint8_t)(crc >> 8);
    /* 70 bytes -> 10 reports of 7 data bytes + seq/flag */
    for (uint8_t seq = 0; seq < 10; seq++) {
        uint8_t r[OTP_FEATURE_RPT_SIZE] = {0};
        memcpy(r, &frame[seq * 7], (seq < 9) ? 7 : (70 - 9 * 7));
        r[7] = (seq & 0x1F) | OTP_SLOT_WRITE_FLAG;
        otp_proto_on_write(r);
    }
}

static void test_full_cr_with_confirm(void)
{
    otp_proto_hooks_t h = { fake_hmac, fake_confirm, fake_arm };
    otp_proto_init(&h);
    otp_proto_reset();
    for (int i = 0; i < 20; i++) g_fake_resp[i] = (uint8_t)(0xA0 + i);
    g_confirm = 0; g_armed_slot = 0xFF;

    uint8_t challenge[64];
    for (int i = 0; i < 64; i++) challenge[i] = (uint8_t)i;
    feed_frame(OTP_SLOT_CHAL_HMAC1, challenge);
    TEST_ASSERT_EQ(g_armed_slot, OTP_SLOT_CHAL_HMAC1, "valid frame armed confirm with slot");

    uint8_t r[OTP_FEATURE_RPT_SIZE];
    otp_proto_on_read(r);
    TEST_ASSERT(r[7] & OTP_RESP_TIMEOUT_WAIT_FLAG, "pending confirm -> RESP_TIMEOUT_WAIT");

    g_confirm = 1;  /* user pressed K_SEC_CONFIRM */
    /* read 3 response reports */
    uint8_t got[21]; int n = 0;
    for (int k = 0; k < 3; k++) {
        otp_proto_on_read(r);
        TEST_ASSERT(r[7] & OTP_RESP_PENDING_FLAG, "authorized -> RESP_PENDING");
        memcpy(&got[n], r, 7); n += 7;
    }
    TEST_ASSERT(memcmp(got, g_fake_resp, 20) == 0, "response bytes match HMAC output");
}

static void test_bad_crc_rejected(void)
{
    otp_proto_hooks_t h = { fake_hmac, fake_confirm, fake_arm };
    otp_proto_init(&h);
    otp_proto_reset();
    g_armed_slot = 0xFF;
    uint8_t challenge[64] = {0};
    /* Feed a frame then corrupt: feed_frame computes valid CRC; instead feed manually with bad CRC */
    uint8_t frame[OTP_FRAME_SIZE]; memset(frame, 0, sizeof(frame));
    memcpy(frame, challenge, 64); frame[64] = OTP_SLOT_CHAL_HMAC1;
    frame[65] = 0xDE; frame[66] = 0xAD;           /* wrong CRC */
    for (uint8_t seq = 0; seq < 10; seq++) {
        uint8_t r[OTP_FEATURE_RPT_SIZE] = {0};
        memcpy(r, &frame[seq * 7], (seq < 9) ? 7 : 7);
        r[7] = (seq & 0x1F) | OTP_SLOT_WRITE_FLAG;
        otp_proto_on_write(r);
    }
    TEST_ASSERT_EQ(g_armed_slot, 0xFF, "bad-CRC frame did NOT arm confirm");
}

void test_otp_proto(void)
{
    TEST_SUITE("otp_proto state machine");
    TEST_RUN(test_full_cr_with_confirm);
    TEST_RUN(test_bad_crc_rejected);
}
```

- [ ] **Step 3: Register + run red** (add sources/include like Task 1). FAIL (otp_proto_* undefined).

- [ ] **Step 4: Implement `otp_proto.c`.** State machine:
  - `RX`: accumulate 10 written reports into the 70-byte frame (use seq low-5-bits ordering; ignore reports that are all-zero data with no flag per the "skip zero segment" optimization — but accept simple sequential for v1). When 70 bytes received: validate `cr_crc16(frame, 65)` against `frame[65..66]` (LSB-first). If valid and `frame[64]` is `OTP_SLOT_CHAL_HMAC1/2`: copy the 64-byte challenge, call `hooks->confirm_arm(slot)`, go to `WAIT`. If invalid: `reset()` (drop).
  - `WAIT`: `on_read` returns status byte = `OTP_RESP_TIMEOUT_WAIT_FLAG`. On each read, check `hooks->confirm_state()`: 0 → stay WAIT; 2 → reset (deny); 1 → call `hooks->compute_hmac(slot, challenge, resp)`, go to `RESP`.
  - `RESP`: `on_read` emits the 20-byte response over 3 reports (7+7+6), status byte = `OTP_RESP_PENDING_FLAG | (seq counter)`; last report resets to IDLE.
  - Implement `otp_proto_on_write` to clear `OTP_SLOT_WRITE_FLAG` semantics (the device-side status during RX reads returns write-flag-busy=0 so host proceeds). Keep buffers static; no malloc.

  (Write the full ~150-line state machine here following the spec above. Keep it pure — only standard C + cr_crc16 + the injected hooks.)

- [ ] **Step 5: Run** → PASS (both tests). **Step 6: Commit**
```bash
git add main/security/otp_proto.h main/security/otp_proto.c test/test_otp_proto.c test/CMakeLists.txt test/test_main.c
git commit -m "feat(sec): OTP HID protocol state machine (otp_proto) + host tests"
```

---

## Task 4: OTP HID transport (`otp_hid`, target-only)

**Files:** Create `main/security/otp_hid.{c,h}`; modify `main/comm/usb/usb_hid.c` + tinyusb descriptors.

This wires `otp_proto` to USB and to the real hooks. Target-only (no host test — validated by Task 5 KAT + Task 7 interop).

- [ ] **Step 1:** Add a second HID interface to the composite descriptor in `main/comm/usb/usb_hid.c` (and tinyusb_config / descriptor arrays). It is a HID keyboard-usage interface (usage page 1, usage 6) with an 8-byte FEATURE report. Bump `CFG_TUD_HID` to 2. Route `tud_hid_set_report_cb`/`tud_hid_get_report_cb` by interface index: existing kbd/mouse on instance 0, OTP on instance 1. (Feature reports come over EP0 — no extra interrupt endpoint strictly required; verify enumeration.)

- [ ] **Step 2:** Implement the real hooks in `otp_hid.c`:
  - `compute_hmac(slot, challenge, out20)`: map slot 0x30→sec_store slot 0, 0x38→slot 1 (decision: fixed mapping; document). `sec_store_get_secret()` → `cr_hmac_sha1(secret, secret_len, challenge, 64, out20)`. Return false if slot empty.
  - `confirm_arm(slot)`: `sec_confirm_arm(slotIndex, now_ms())`.
  - `confirm_state()`: call `sec_confirm_poll(now_ms(), &s)` → map AUTHORIZED→1, TIMEDOUT→2, else 0.
  - On `tud_hid_set_report_cb` (FEATURE, instance 1): `otp_proto_on_write(buf)`.
  - On `tud_hid_get_report_cb` (FEATURE, instance 1): `otp_proto_on_read(buf)`; return 8.
  - `otp_hid_init()` calls `otp_proto_init(&real_hooks)`.

- [ ] **Step 3:** Wire `otp_hid_init()` at the dongle init site (next to `cdc_sec_cmds_init()` in `main.c`). Build dongle: `./scripts/check.sh --board kase_dongle` green. Commit.

---

## Task 5: On-target HMAC-SHA1 known-answer test

- [ ] Add a one-shot KAT (behind a debug CDC command or run-once at boot under a build flag) that computes `cr_hmac_sha1` over RFC 2202 test case 1 (key=0x0b×20, data="Hi There") and checks the result equals `b617318655057264e28bc0b6fb378c8ef146be00`. Log PASS/FAIL. Remove or gate it after validating on hardware. (This is the only on-target validation of the mbedtls HMAC path.) Commit.

---

## Task 6: Build wiring (dongle gating)

- [ ] In `main/CMakeLists.txt`: add `cr_crc16.c` + `otp_proto.c` (host-tested, but compile for dongle) and `cr_hmac.c` + `otp_hid.c` inside the `CONFIG_KASE_DEVICE_ROLE_DONGLE` block. (cr_crc16/otp_proto are pure — fine to compile for dongle only since only the dongle uses OTP.) Build `kase_dongle` + `kase_v2` green. Commit.

---

## Task 7: KeePassXC VID/PID patch + host provisioning tool (Rust)

- [ ] Produce a patch against KeePassXC `src/keys/drivers/YubiKeyInterfaceUSB.cpp` adding the KaSe dongle VID `0x303a` and PID `0x4001` to the `vids[]`/`pids[]` arrays, mirroring commit `e4326fb` (OnlyKey). Save it as `docs/keepassxc-kase-vid.patch` in this repo with build instructions, and (optionally) open an upstream PR.
- [ ] **Host provisioning tool — write it in RUST** (decided 2026-06-09). A small CLI (`kase-sec`) that speaks the CDC binary protocol (KS/KR frames, CRC-8) to the dongle to provision/clear/list slots: e.g. `kase-sec provision --slot 0 --secret <base32|hex>` → `KS_CMD_SEC_SET_SLOT`. Rationale: this is the natural place to use Rust on this project — host-side, greenfield, isolated, zero firmware FFI pain, and a great ecosystem fit (`clap` for CLI, `serialport`/`hidapi`, `hmac`/`sha1` for cross-checking). It scratches the original "learn Rust + fun" goal that started this whole effort, on terrain where Rust wins (the firmware stays C). Do NOT extend the C#/WPF controller for this — new Rust crate instead. Suggested location: a `tools/kase-sec/` Rust crate in this repo, or its own repo.
- [ ] Document in a new `docs/SECURITY_KEY.md` how to provision a slot (via `kase-sec`) and configure KeePassXC to use the dongle as a challenge-response key. Commit.

---

## Hardware validation (after the plan)

Run `docs/HARDWARE_SMOKE_TEST.md` plus: provision a 20-byte secret into slot 0 via CDC; in patched KeePassXC, set up a CR-protected database; on unlock, confirm the "Touch" prompt appears and pressing `K_SEC_CONFIRM` on a half unlocks the DB. Cross-check the same secret with `ykchalresp -2 -x <hex>` against a software HMAC-SHA1 of the challenge.

---

## Self-review notes (done while writing)

- **Spec coverage:** §5.5 cr_hmac → Task 2; §5.3 OTP HID (descriptor + frame + status SM) → Tasks 3,4; the CRC the frame needs → Task 1; require-keypress integration (sec_confirm) → Task 3 hooks + Task 4 real hooks; build gating → Task 6; the host-side compat decision → Task 7.
- **Crypto host-test gap is explicit and mitigated** (Task 2 note + Task 5 KAT). The complex/bug-prone logic (CRC, frame parse, status SM, response chunking) IS host-tested in Tasks 1+3.
- **Known detail to verify in impl (not a placeholder):** exact CRC coverage range (payload+slot = 65 bytes assumed; confirm against ykpers) — the `0xF0B8` residual test (Task 1) and the otp_proto round-trip (Task 3) are the oracles. The TinyUSB 2-HID-interface descriptor specifics (Task 4 Step 1) must be confirmed against the existing usb_hid.c at implementation time.

## Protocol reference (from the 2026-06-09 spike, sourced)

- OTP interface = keyboard HID (usage page 1, usage 6), **8-byte FEATURE reports over EP0** (SET/GET_REPORT). (yubikey-manager `USAGE_OTP=(1,6)`.)
- Frame = 70 bytes: 64 payload + 1 slot + 2 CRC-16 + 3 filler. Written as 10×8-byte reports: bytes[0..6]=data, byte[7]=`(seq&0x1F)|0x80`. (ykpers `ykcore.c`, `ykdef.h`.)
- Slot cmds: `SLOT_CHAL_HMAC1=0x30`, `SLOT_CHAL_HMAC2=0x38`. Challenge ≤64 (KeePassXC pads PKCS7 to 64). Response = 20 bytes.
- Status byte (GET_REPORT byte[7]): `SLOT_WRITE_FLAG=0x80` (busy), `RESP_PENDING_FLAG=0x40` (response ready), `RESP_TIMEOUT_WAIT_FLAG=0x20` (waiting for touch). Read response over 3 reports; seq resets to 0 on last.
- Touch: hold `RESP_TIMEOUT_WAIT_FLAG` until press → then `RESP_PENDING_FLAG`. KeePassXC `mayBlock=true` waits up to 256 s, shows "Touch" prompt.
- CRC = YubiKey CRC-16 (init 0xFFFF, reflected poly 0x8408, residual 0xF0B8). (ykpers `crc16.c`.)
- **Host compat: KeePassXC/ykpers filter by Yubico VID 0x1050; our 0x303a needs the Task-7 patch (OnlyKey precedent, KeePassXC commit e4326fb).**
- Sources: Yubico `yubikey-personalization` (ykdef.h, ykcore.c, crc16.c, ykchalresp.c); KeePassXC `YubiKeyInterfaceUSB.cpp` + commit e4326fb; yubikey-manager hid/base.py; TinyUSB hid_device.h.
