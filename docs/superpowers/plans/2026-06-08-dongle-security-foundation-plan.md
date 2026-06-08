# Dongle Security Foundation — Implementation Plan (Plan 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the host-testable security foundation for the dongle — a physical-keypress
confirmation gate (`sec_confirm`), the `K_SEC_CONFIRM` keycode + engine hook, the
write-only secret store (`sec_store`), and CDC provisioning commands — WITHOUT the CR
crypto engine or the YubiKey OTP HID transport (those are Plan 2, gated on a protocol spike).

**Architecture:** New `main/security/` module. `sec_confirm` is a pure state machine
(IDLE→PENDING→AUTHORIZED, with timeout) compiled for all roles so `key_processor` can call
it. `sec_store` holds secrets in an NVS blob (pure slot logic + NVS behind `#ifndef
TEST_HOST`, like `trackpad.c`), write-only over CDC. New CDC commands (0xC0–0xC2) provision
slots. `K_SEC_CONFIRM` (0x3E00) on a half routes through the existing
`rf_rx → build_keycode_report → process_advanced_key` path to authorize a pending request.

**Tech Stack:** C, ESP-IDF 5.5, existing host test harness (`test/`, `test_framework.h`),
NVS via `nvs_utils.h`, CDC binary protocol (KS/KR frames). No mbedtls in this plan.

**Spec:** `docs/superpowers/specs/2026-06-08-dongle-security-key-foundation-cr-hmac-design.md`

---

## File structure

- Create `main/security/sec_confirm.h` / `sec_confirm.c` — pure state machine (all roles).
- Create `main/security/sec_store.h` / `sec_store.c` — slot store (pure + NVS, dongle).
- Create `main/comm/cdc/cdc_sec_cmds.c` — provisioning commands (dongle).
- Modify `main/input/key_definitions.h` — add `K_SEC_*` keycodes.
- Modify `main/comm/cdc/cdc_binary_protocol.h` — add `KS_CMD_SEC_*` IDs.
- Modify `main/input/key_processor.c` — hook `K_SEC_CONFIRM` → `sec_confirm_authorize()`.
- Modify `main/CMakeLists.txt` — register new sources (sec_confirm all-roles; sec_store +
  cdc_sec_cmds dongle-gated).
- Wire `cdc_sec_cmds_init()` + `sec_store_init()` at the dongle init site.
- Tests: `test/test_sec_confirm.c`, `test/test_sec_store.c`, `test/test_cdc_sec.c`; additions
  to `test/test_keycodes.c` and `test/test_keycode_report.c`; register in `test/CMakeLists.txt`
  + `test/test_main.c`.

---

## Task 1: `K_SEC_CONFIRM` keycode

**Files:**
- Modify: `main/input/key_definitions.h` (advanced-keycode block, after `K_OVERRIDE`)
- Test: `test/test_keycodes.c`

- [ ] **Step 1: Write the failing test** — append to `test/test_keycodes.c` and register the new function in its suite runner (and `test_main.c` already calls `test_keycodes`).

```c
/* Security keycodes (0x3E00 block) */
static void test_keycode_sec_confirm(void)
{
    TEST_ASSERT_EQ(K_SEC_CONFIRM, 0x3E00, "K_SEC_CONFIRM = 0x3E00");
    TEST_ASSERT(K_IS_SEC(K_SEC_CONFIRM), "K_IS_SEC true for K_SEC_CONFIRM");
    TEST_ASSERT(!K_IS_SEC(0x3D00), "K_IS_SEC false for K_OVERRIDE base");
    TEST_ASSERT(!K_IS_SEC(0x4000), "K_IS_SEC false for LT base");
    TEST_ASSERT_EQ(K_SEC_TYPE(K_SEC_CONFIRM), 0x00, "K_SEC_TYPE extracts low byte");
}
```
Add `test_keycode_sec_confirm` to the `TEST_RUN(...)` block inside `test_keycodes()`.
(`test/test_keycodes.c` includes `../main/input/key_definitions.h`.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/mae/Documents/GitHub/KaSe_firmware && ./scripts/check.sh --host-only`
Expected: FAIL — `K_SEC_CONFIRM` / `K_IS_SEC` undeclared.

- [ ] **Step 3: Add the keycodes** — in `main/input/key_definitions.h`, after the
`K_OVERRIDE` block (~0x3D00):

```c
/* Security key actions — 0x3E00 block (free advanced range; rides
 * process_advanced_key dispatch, avoids int16_t sign issues of >=0x8000) */
#define K_SEC_BASE                   0x3E00
#define K_SEC_CONFIRM                0x3E00  /* authorize a pending CR request */
#define K_IS_SEC(kc)                 (((kc) & 0xFF00) == K_SEC_BASE)
#define K_SEC_TYPE(kc)               ((kc) & 0xFF)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./scripts/check.sh --host-only`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add main/input/key_definitions.h test/test_keycodes.c
git commit -m "feat(sec): add K_SEC_CONFIRM keycode (0x3E00)"
```

---

## Task 2: `sec_confirm` state machine (pure)

**Files:**
- Create: `main/security/sec_confirm.h`, `main/security/sec_confirm.c`
- Test: `test/test_sec_confirm.c`

- [ ] **Step 1: Write the header**

```c
/* main/security/sec_confirm.h — physical-keypress confirmation gate.
 * Pure (no NVS/HW); compiled for all roles so key_processor can call it. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SEC_CONFIRM_TIMEOUT_MS 15000u

typedef enum {
    SEC_CONFIRM_IDLE = 0,
    SEC_CONFIRM_PENDING,
    SEC_CONFIRM_AUTHORIZED,
    SEC_CONFIRM_TIMEDOUT,
} sec_confirm_state_t;

void sec_confirm_reset(void);
/* Arm a pending request for `slot`, stamped at now_ms. Overwrites any prior pending. */
void sec_confirm_arm(uint8_t slot, uint32_t now_ms);
/* Physical confirm key pressed: PENDING -> AUTHORIZED; no-op otherwise. */
void sec_confirm_authorize(void);
/* Poll at now_ms. PENDING past timeout -> returns TIMEDOUT once (then IDLE).
 * AUTHORIZED -> writes slot to *out_slot, consumes (-> IDLE), returns AUTHORIZED. */
sec_confirm_state_t sec_confirm_poll(uint32_t now_ms, uint8_t *out_slot);
```

- [ ] **Step 2: Write the failing test** — `test/test_sec_confirm.c`

```c
#include "test_framework.h"
#include "sec_confirm.h"

static void test_arm_authorize_consume(void)
{
    sec_confirm_reset();
    sec_confirm_arm(2, 1000);
    TEST_ASSERT_EQ(sec_confirm_poll(1000, NULL), SEC_CONFIRM_PENDING, "armed -> PENDING");
    sec_confirm_authorize();
    uint8_t slot = 0xFF;
    TEST_ASSERT_EQ(sec_confirm_poll(1100, &slot), SEC_CONFIRM_AUTHORIZED, "authorized");
    TEST_ASSERT_EQ(slot, 2, "slot preserved");
    TEST_ASSERT_EQ(sec_confirm_poll(1200, NULL), SEC_CONFIRM_IDLE, "consumed -> IDLE");
}

static void test_timeout(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0, 1000);
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 14999, NULL), SEC_CONFIRM_PENDING, "before timeout");
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 15000, NULL), SEC_CONFIRM_TIMEDOUT, "at timeout");
    TEST_ASSERT_EQ(sec_confirm_poll(1000 + 16000, NULL), SEC_CONFIRM_IDLE, "after timeout -> IDLE");
}

static void test_authorize_without_arm(void)
{
    sec_confirm_reset();
    sec_confirm_authorize();
    TEST_ASSERT_EQ(sec_confirm_poll(0, NULL), SEC_CONFIRM_IDLE, "authorize w/o arm = no-op");
}

static void test_rearm_overwrites_slot(void)
{
    sec_confirm_reset();
    sec_confirm_arm(1, 1000);
    sec_confirm_arm(3, 1050);
    sec_confirm_authorize();
    uint8_t slot = 0xFF;
    sec_confirm_poll(1060, &slot);
    TEST_ASSERT_EQ(slot, 3, "re-arm overwrites slot");
}

void test_sec_confirm(void)
{
    TEST_SUITE("sec_confirm state machine");
    TEST_RUN(test_arm_authorize_consume);
    TEST_RUN(test_timeout);
    TEST_RUN(test_authorize_without_arm);
    TEST_RUN(test_rearm_overwrites_slot);
}
```

- [ ] **Step 3: Run test to verify it fails** — first register it (Task 6 wires CMake;
for the red run, temporarily add `test_sec_confirm.c` + `../main/security/sec_confirm.c` to
`test/CMakeLists.txt` and declare+call `test_sec_confirm` in `test/test_main.c`, with
`${CMAKE_SOURCE_DIR}/../main/security` on the include path).

Run: `./scripts/check.sh --host-only`
Expected: FAIL — `sec_confirm_*` undefined (no `.c` yet).

- [ ] **Step 4: Write the implementation** — `main/security/sec_confirm.c`

```c
#include "sec_confirm.h"

static sec_confirm_state_t s_state   = SEC_CONFIRM_IDLE;
static uint8_t             s_slot    = 0;
static uint32_t            s_armed_ms = 0;

void sec_confirm_reset(void)
{
    s_state = SEC_CONFIRM_IDLE;
    s_slot = 0;
    s_armed_ms = 0;
}

void sec_confirm_arm(uint8_t slot, uint32_t now_ms)
{
    s_state = SEC_CONFIRM_PENDING;
    s_slot = slot;
    s_armed_ms = now_ms;
}

void sec_confirm_authorize(void)
{
    if (s_state == SEC_CONFIRM_PENDING)
        s_state = SEC_CONFIRM_AUTHORIZED;
}

sec_confirm_state_t sec_confirm_poll(uint32_t now_ms, uint8_t *out_slot)
{
    if (s_state == SEC_CONFIRM_PENDING &&
        (now_ms - s_armed_ms) >= SEC_CONFIRM_TIMEOUT_MS) {
        s_state = SEC_CONFIRM_IDLE;
        return SEC_CONFIRM_TIMEDOUT;
    }
    if (s_state == SEC_CONFIRM_AUTHORIZED) {
        if (out_slot) *out_slot = s_slot;
        s_state = SEC_CONFIRM_IDLE;
        return SEC_CONFIRM_AUTHORIZED;
    }
    return s_state;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./scripts/check.sh --host-only`
Expected: PASS (4 sec_confirm tests).

- [ ] **Step 6: Commit**

```bash
git add main/security/sec_confirm.h main/security/sec_confirm.c test/test_sec_confirm.c test/CMakeLists.txt test/test_main.c
git commit -m "feat(sec): sec_confirm keypress-gate state machine + host tests"
```

---

## Task 3: `sec_store` secret store (pure slots + NVS)

**Files:**
- Create: `main/security/sec_store.h`, `main/security/sec_store.c`
- Test: `test/test_sec_store.c`

- [ ] **Step 1: Write the header**

```c
/* main/security/sec_store.h — write-only secret slots (dongle). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SEC_N_SLOTS     4
#define SEC_LABEL_LEN   16
#define SEC_SECRET_MAX  64

enum { SEC_SLOT_EMPTY = 0, SEC_SLOT_HMAC_SHA1 = 1 };

typedef struct {
    uint8_t type;
    uint8_t flags;            /* bit0 = require_keypress (always 1 in Phase 1) */
    uint8_t secret_len;
    uint8_t reserved;
    char    label[SEC_LABEL_LEN];
    uint8_t secret[SEC_SECRET_MAX];
} sec_slot_t;

void    sec_store_init(void);     /* load from NVS (target) / zero (host) */
bool    sec_store_set_slot(uint8_t idx, uint8_t type, const char *label,
                           const uint8_t *secret, uint8_t secret_len);
bool    sec_store_clear_slot(uint8_t idx);
uint8_t sec_store_count(void);
uint8_t sec_store_type(uint8_t idx);          /* SEC_SLOT_EMPTY if oob/empty */
const char *sec_store_label(uint8_t idx);     /* NULL if oob/empty */
/* INTERNAL — firmware-only, never exposed over CDC. */
bool    sec_store_get_secret(uint8_t idx, uint8_t *out, uint8_t *out_len);
```

- [ ] **Step 2: Write the failing test** — `test/test_sec_store.c`

```c
#define TEST_HOST 1
#include "test_framework.h"
#include "sec_store.h"

static void test_set_get(void)
{
    sec_store_init();
    uint8_t key[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
    TEST_ASSERT(sec_store_set_slot(1, SEC_SLOT_HMAC_SHA1, "github", key, 20), "set ok");
    TEST_ASSERT_EQ(sec_store_type(1), SEC_SLOT_HMAC_SHA1, "type stored");
    TEST_ASSERT(strcmp(sec_store_label(1), "github") == 0, "label stored");
    uint8_t out[64]; uint8_t olen = 0;
    TEST_ASSERT(sec_store_get_secret(1, out, &olen), "get secret ok");
    TEST_ASSERT_EQ(olen, 20, "secret_len");
    TEST_ASSERT(memcmp(out, key, 20) == 0, "secret bytes match");
    TEST_ASSERT_EQ(sec_store_count(), 1, "count = 1");
}

static void test_clear(void)
{
    sec_store_init();
    uint8_t key[4] = {9,9,9,9};
    sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "x", key, 4);
    TEST_ASSERT(sec_store_clear_slot(0), "clear ok");
    TEST_ASSERT_EQ(sec_store_type(0), SEC_SLOT_EMPTY, "slot empty after clear");
    TEST_ASSERT(sec_store_label(0) == NULL, "label NULL after clear");
    TEST_ASSERT_EQ(sec_store_count(), 0, "count = 0");
}

static void test_bounds(void)
{
    sec_store_init();
    uint8_t key[4] = {1,2,3,4};
    TEST_ASSERT(!sec_store_set_slot(SEC_N_SLOTS, SEC_SLOT_HMAC_SHA1, "x", key, 4), "idx oob rejected");
    uint8_t big[65] = {0};
    TEST_ASSERT(!sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "x", big, 65), "secret_len > max rejected");
    TEST_ASSERT(sec_store_get_secret(2, key, NULL) == false, "get empty slot fails");
}

void test_sec_store(void)
{
    TEST_SUITE("sec_store");
    TEST_RUN(test_set_get);
    TEST_RUN(test_clear);
    TEST_RUN(test_bounds);
}
```

- [ ] **Step 3: Run test to verify it fails** — register `test_sec_store.c` +
`../main/security/sec_store.c` in `test/CMakeLists.txt` and `test_main.c` (as in Task 2).

Run: `./scripts/check.sh --host-only`
Expected: FAIL — `sec_store_*` undefined.

- [ ] **Step 4: Write the implementation** — `main/security/sec_store.c`

```c
#include "sec_store.h"
#include <string.h>

static void sec_store_persist(void);   /* defined per-target below */

static sec_slot_t s_slots[SEC_N_SLOTS];

bool sec_store_set_slot(uint8_t idx, uint8_t type, const char *label,
                        const uint8_t *secret, uint8_t secret_len)
{
    if (idx >= SEC_N_SLOTS) return false;
    if (secret_len > SEC_SECRET_MAX) return false;
    sec_slot_t *s = &s_slots[idx];
    memset(s, 0, sizeof(*s));
    s->type = type;
    s->flags = 0x01;                 /* require_keypress forced on */
    s->secret_len = secret_len;
    if (label) {
        strncpy(s->label, label, SEC_LABEL_LEN - 1);
        s->label[SEC_LABEL_LEN - 1] = '\0';
    }
    if (secret && secret_len) memcpy(s->secret, secret, secret_len);
    sec_store_persist();
    return true;
}

bool sec_store_clear_slot(uint8_t idx)
{
    if (idx >= SEC_N_SLOTS) return false;
    memset(&s_slots[idx], 0, sizeof(s_slots[idx]));
    sec_store_persist();
    return true;
}

uint8_t sec_store_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < SEC_N_SLOTS; i++)
        if (s_slots[i].type != SEC_SLOT_EMPTY) n++;
    return n;
}

uint8_t sec_store_type(uint8_t idx)
{
    return (idx < SEC_N_SLOTS) ? s_slots[idx].type : SEC_SLOT_EMPTY;
}

const char *sec_store_label(uint8_t idx)
{
    if (idx >= SEC_N_SLOTS || s_slots[idx].type == SEC_SLOT_EMPTY) return NULL;
    return s_slots[idx].label;
}

bool sec_store_get_secret(uint8_t idx, uint8_t *out, uint8_t *out_len)
{
    if (idx >= SEC_N_SLOTS || s_slots[idx].type == SEC_SLOT_EMPTY) return false;
    if (out)     memcpy(out, s_slots[idx].secret, s_slots[idx].secret_len);
    if (out_len) *out_len = s_slots[idx].secret_len;
    return true;
}

#ifndef TEST_HOST
#include "nvs_utils.h"
#include "keyboard_config.h"   /* STORAGE_NAMESPACE */
static void sec_store_persist(void)
{
    nvs_save_blob_with_total(STORAGE_NAMESPACE, "sec_slots", s_slots,
                             sizeof(s_slots), "sec_slots_ver", 1);
}
void sec_store_init(void)
{
    uint32_t ver = 0;
    nvs_load_blob_with_total(STORAGE_NAMESPACE, "sec_slots", s_slots,
                             sizeof(s_slots), "sec_slots_ver", &ver);
}
#else
static void sec_store_persist(void) { /* host: no NVS */ }
void sec_store_init(void) { memset(s_slots, 0, sizeof(s_slots)); }
#endif
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./scripts/check.sh --host-only`
Expected: PASS (3 sec_store tests).

- [ ] **Step 6: Commit**

```bash
git add main/security/sec_store.h main/security/sec_store.c test/test_sec_store.c test/CMakeLists.txt test/test_main.c
git commit -m "feat(sec): sec_store write-only secret slots + host tests"
```

---

## Task 4: CDC provisioning commands (write-only)

**Files:**
- Modify: `main/comm/cdc/cdc_binary_protocol.h` (add IDs)
- Create: `main/comm/cdc/cdc_sec_cmds.c`
- Test: `test/test_cdc_sec.c` (tests the pure parse/build helpers)

- [ ] **Step 1: Add command IDs** — in `main/comm/cdc/cdc_binary_protocol.h`, in the free
0xC0 range (do this first; it's a non-testable constant add):

```c
/* Security key provisioning (dongle) — write-only, no secret read-back */
#define KS_CMD_SEC_SET_SLOT    0xC0
#define KS_CMD_SEC_CLEAR_SLOT  0xC1
#define KS_CMD_SEC_LIST        0xC2
```

- [ ] **Step 2: Write the failing test** — `test/test_cdc_sec.c` (tests the pure helpers
that the handlers will call):

```c
#define TEST_HOST 1
#include "test_framework.h"
#include "sec_store.h"
#include "cdc_sec_cmds.h"

/* SET_SLOT payload: [idx:u8][type:u8][label:16][secret_len:u8][secret...] */
static void test_parse_set_slot_valid(void)
{
    uint8_t p[3 + SEC_LABEL_LEN + 20];
    memset(p, 0, sizeof(p));
    p[0] = 2;                       /* idx */
    p[1] = SEC_SLOT_HMAC_SHA1;      /* type */
    memcpy(&p[2], "github", 6);     /* label */
    p[2 + SEC_LABEL_LEN] = 20;      /* secret_len */
    for (int i = 0; i < 20; i++) p[3 + SEC_LABEL_LEN + i] = (uint8_t)i;

    uint8_t idx, type, slen; char label[SEC_LABEL_LEN]; uint8_t secret[SEC_SECRET_MAX];
    bool ok = sec_cmd_parse_set_slot(p, sizeof(p), &idx, &type, label, secret, &slen);
    TEST_ASSERT(ok, "valid SET_SLOT parses");
    TEST_ASSERT_EQ(idx, 2, "idx");
    TEST_ASSERT_EQ(slen, 20, "secret_len");
    TEST_ASSERT(strcmp(label, "github") == 0, "label");
    TEST_ASSERT_EQ(secret[19], 19, "secret bytes");
}

static void test_parse_set_slot_rejects(void)
{
    uint8_t p[3 + SEC_LABEL_LEN + 64];
    memset(p, 0, sizeof(p));
    uint8_t idx, type, slen; char label[SEC_LABEL_LEN]; uint8_t secret[SEC_SECRET_MAX];

    p[0] = SEC_N_SLOTS;             /* idx oob */
    p[2 + SEC_LABEL_LEN] = 4;
    TEST_ASSERT(!sec_cmd_parse_set_slot(p, 3 + SEC_LABEL_LEN + 4, &idx, &type, label, secret, &slen),
                "idx oob rejected");

    p[0] = 0;
    p[2 + SEC_LABEL_LEN] = 65;      /* secret_len > max */
    TEST_ASSERT(!sec_cmd_parse_set_slot(p, sizeof(p), &idx, &type, label, secret, &slen),
                "secret_len > max rejected");

    TEST_ASSERT(!sec_cmd_parse_set_slot(p, 3, &idx, &type, label, secret, &slen),
                "truncated payload rejected");
}

/* LIST payload must contain labels but NEVER secret bytes (invariant I1) */
static void test_list_has_no_secret(void)
{
    sec_store_init();
    uint8_t key[20]; memset(key, 0xAB, 20);
    sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "acct", key, 20);

    uint8_t buf[128]; uint16_t len = 0;
    sec_cmd_build_list(buf, &len);
    /* label present */
    TEST_ASSERT(memmem(buf, len, "acct", 4) != NULL, "label present in LIST");
    /* secret byte 0xAB run absent */
    uint8_t needle[4] = {0xAB,0xAB,0xAB,0xAB};
    TEST_ASSERT(memmem(buf, len, needle, 4) == NULL, "no secret bytes in LIST (I1)");
}

void test_cdc_sec(void)
{
    TEST_SUITE("cdc_sec provisioning");
    TEST_RUN(test_parse_set_slot_valid);
    TEST_RUN(test_parse_set_slot_rejects);
    TEST_RUN(test_list_has_no_secret);
}
```
(`memmem` is a GNU extension available in the host glibc build; the harness compiles with
`-D_GNU_SOURCE` already via `string.h` usage — if not, add `#define _GNU_SOURCE` before the
includes in this test file.)

- [ ] **Step 3: Write the failing test run** — register `test_cdc_sec.c` +
`../main/comm/cdc/cdc_sec_cmds.c` + `../main/security/sec_store.c` in `test/CMakeLists.txt`
and `test_main.c`; add `main/comm/cdc` to the test include path.

Run: `./scripts/check.sh --host-only`
Expected: FAIL — `sec_cmd_parse_set_slot` / `sec_cmd_build_list` undefined.

- [ ] **Step 4: Write the header + implementation** — `main/comm/cdc/cdc_sec_cmds.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sec_store.h"

/* Pure helpers (host-tested) */
bool sec_cmd_parse_set_slot(const uint8_t *p, uint16_t len,
                            uint8_t *idx, uint8_t *type,
                            char label[SEC_LABEL_LEN],
                            uint8_t secret[SEC_SECRET_MAX], uint8_t *secret_len);
void sec_cmd_build_list(uint8_t *out, uint16_t *out_len);

/* Registers the SEC command table (dongle init). */
void cdc_sec_cmds_init(void);
```

`main/comm/cdc/cdc_sec_cmds.c`:

```c
#include "cdc_sec_cmds.h"
#include <string.h>

bool sec_cmd_parse_set_slot(const uint8_t *p, uint16_t len,
                            uint8_t *idx, uint8_t *type,
                            char label[SEC_LABEL_LEN],
                            uint8_t secret[SEC_SECRET_MAX], uint8_t *secret_len)
{
    if (len < (uint16_t)(3 + SEC_LABEL_LEN)) return false;
    uint8_t i = p[0], t = p[1];
    uint8_t slen = p[2 + SEC_LABEL_LEN];
    if (i >= SEC_N_SLOTS) return false;
    if (slen > SEC_SECRET_MAX) return false;
    if (len < (uint16_t)(3 + SEC_LABEL_LEN + slen)) return false;
    *idx = i; *type = t; *secret_len = slen;
    memcpy(label, &p[2], SEC_LABEL_LEN);
    label[SEC_LABEL_LEN - 1] = '\0';
    if (slen) memcpy(secret, &p[3 + SEC_LABEL_LEN], slen);
    return true;
}

/* LIST response: [count:u8] then per non-empty slot [idx:u8][type:u8][label:16].
 * Deliberately no secret bytes (invariant I1). */
void sec_cmd_build_list(uint8_t *out, uint16_t *out_len)
{
    uint16_t n = 0;
    uint8_t count = 0;
    n = 1;                                   /* reserve count byte */
    for (uint8_t i = 0; i < SEC_N_SLOTS; i++) {
        if (sec_store_type(i) == SEC_SLOT_EMPTY) continue;
        out[n++] = i;
        out[n++] = sec_store_type(i);
        const char *lbl = sec_store_label(i);
        memset(&out[n], 0, SEC_LABEL_LEN);
        if (lbl) strncpy((char *)&out[n], lbl, SEC_LABEL_LEN - 1);
        n += SEC_LABEL_LEN;
        count++;
    }
    out[0] = count;
    *out_len = n;
}

#ifndef TEST_HOST
#include "cdc_binary_protocol.h"

static void bin_cmd_sec_set_slot(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    uint8_t idx, type, slen; char label[SEC_LABEL_LEN]; uint8_t secret[SEC_SECRET_MAX];
    if (!sec_cmd_parse_set_slot(payload, len, &idx, &type, label, secret, &slen)) {
        ks_respond_err(cmd_id, KS_STATUS_ERR_PARAM);
        return;
    }
    sec_store_set_slot(idx, type, label, secret, slen);
    ks_respond_ok(cmd_id);
}

static void bin_cmd_sec_clear_slot(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    if (len < 1 || !sec_store_clear_slot(payload[0])) {
        ks_respond_err(cmd_id, KS_STATUS_ERR_PARAM);
        return;
    }
    ks_respond_ok(cmd_id);
}

static void bin_cmd_sec_list(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    (void)payload; (void)len;
    uint8_t buf[1 + SEC_N_SLOTS * (2 + SEC_LABEL_LEN)];
    uint16_t out_len = 0;
    sec_cmd_build_list(buf, &out_len);
    ks_respond(cmd_id, KS_STATUS_OK, buf, out_len);
}

static const ks_bin_cmd_entry_t sec_cmd_table[] = {
    { KS_CMD_SEC_SET_SLOT,   bin_cmd_sec_set_slot   },
    { KS_CMD_SEC_CLEAR_SLOT, bin_cmd_sec_clear_slot },
    { KS_CMD_SEC_LIST,       bin_cmd_sec_list       },
};

void cdc_sec_cmds_init(void)
{
    ks_register_binary_commands(sec_cmd_table, sizeof(sec_cmd_table) / sizeof(sec_cmd_table[0]));
}
#else
void cdc_sec_cmds_init(void) { /* host: not registered */ }
#endif
```
(If `KS_STATUS_ERR_PARAM` does not exist in `cdc_binary_protocol.h`, use the existing
generic error status defined there — grep `KS_STATUS_ERR_` and pick the param/validation
one; do not invent a new status in this task.)

- [ ] **Step 5: Run test to verify it passes**

Run: `./scripts/check.sh --host-only`
Expected: PASS (3 cdc_sec tests).

- [ ] **Step 6: Commit**

```bash
git add main/comm/cdc/cdc_binary_protocol.h main/comm/cdc/cdc_sec_cmds.c main/comm/cdc/cdc_sec_cmds.h test/test_cdc_sec.c test/CMakeLists.txt test/test_main.c
git commit -m "feat(sec): CDC SEC provisioning commands (write-only, 0xC0-0xC2)"
```

---

## Task 5: Engine hook — `K_SEC_CONFIRM` authorizes

**Files:**
- Modify: `main/input/key_processor.c` (`process_advanced_key`)
- Test: `test/test_keycode_report.c`

- [ ] **Step 1: Write the failing test** — append to `test/test_keycode_report.c`. Add at
top: `#include "sec_confirm.h"` and `#define T_K_SEC_CONFIRM 0x3E00u`. Then:

```c
static void test_kp_sec_confirm_authorizes(void)
{
    reset_kp_state();
    sec_confirm_reset();
    sec_confirm_arm(2, 0);                 /* pending request for slot 2 */
    keymaps[0][0][0] = T_K_SEC_CONFIRM;
    press_key(0, 0, 0);
    build_keycode_report();

    uint8_t slot = 0xFF;
    sec_confirm_state_t st = sec_confirm_poll(1, &slot);
    TEST_ASSERT_EQ(st, SEC_CONFIRM_AUTHORIZED, "K_SEC_CONFIRM press authorizes pending");
    TEST_ASSERT_EQ(slot, 2, "authorized slot = 2");
    TEST_ASSERT_EQ(keycodes[0], 0, "K_SEC_CONFIRM absorbed (not in HID report)");
}
```
Register `TEST_RUN(test_kp_sec_confirm_authorizes)` in `test_keycode_report()`. Add
`../main/security/sec_confirm.c` + the `main/security` include dir to the
`test_keycode_report` build in `test/CMakeLists.txt` (sec_confirm.c is already added by
Task 2; just ensure the include path is present).

- [ ] **Step 2: Run test to verify it fails**

Run: `./scripts/check.sh --host-only`
Expected: FAIL — `K_SEC_CONFIRM` not handled (`keycodes[0]` becomes `extra_keycodes`/0 but
`sec_confirm` stays PENDING, so state != AUTHORIZED).

- [ ] **Step 3: Add the hook** — in `main/input/key_processor.c`, add the include near the
top (with the other feature includes):

```c
#include "security/sec_confirm.h"
```
Then in `process_advanced_key()`, add this branch (alongside the other `if (kc == ...)`
single-keycode features, e.g. near `K_CAPS_WORD`):

```c
    if (kc == K_SEC_CONFIRM) { sec_confirm_authorize(); return 0; }
```
(`return 0` makes the key absorbed — never emitted as a keystroke. `K_SEC_CONFIRM` =
0x3E00 is already routed to `process_advanced_key` by `is_advanced_keycode`, since it is
>0xFF and outside the MO/TO/MACRO/BT ranges.)

- [ ] **Step 4: Run test to verify it passes**

Run: `./scripts/check.sh --host-only`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add main/input/key_processor.c test/test_keycode_report.c test/CMakeLists.txt
git commit -m "feat(sec): K_SEC_CONFIRM keypress authorizes pending CR request"
```

---

## Task 6: Build wiring (dongle gating + init) + dongle build

**Files:**
- Modify: `main/CMakeLists.txt`
- Modify: dongle init site (where `cdc_dongle_cmds_init()` is called — grep it)

- [ ] **Step 1: Register sources in `main/CMakeLists.txt`**

`sec_confirm.c` is needed by `key_processor.c` for ALL roles → add it unconditionally to
`srcs` (next to the other `input/` or core sources):

```cmake
    "security/sec_confirm.c"
```
`sec_store.c` + `cdc_sec_cmds.c` are dongle-only → add inside the existing
`if(CONFIG_KASE_DEVICE_ROLE_DONGLE)` block (next to `cdc_dongle_cmds.c`):

```cmake
        "security/sec_store.c"
        "comm/cdc/cdc_sec_cmds.c"
```
Ensure `main/security` is on the component include dirs (add to the `INCLUDE_DIRS`/
`target_include_directories` for `main` if components reference it; `key_processor.c` uses
the relative `"security/sec_confirm.h"`, which resolves from `main/`, so no change may be
needed — verify the build).

- [ ] **Step 2: Wire init at the dongle startup site**

Grep the call site: `grep -rn "cdc_dongle_cmds_init" main/`. In the same function (dongle
init), add:

```c
    sec_store_init();
    cdc_sec_cmds_init();
```
with the includes `#include "security/sec_store.h"` and `#include "comm/cdc/cdc_sec_cmds.h"`
(adjust the relative path to match that file's location).

- [ ] **Step 3: Build the dongle to verify it compiles**

Run:
```bash
source ~/esp/esp-idf/export.sh
./scripts/check.sh --board kase_dongle
```
Expected: `✓ Build kase_dongle OK` and host tests green.

- [ ] **Step 4: Build a keyboard board to verify sec_confirm-everywhere didn't break it**

Run: `./scripts/check.sh --board kase_v2`
Expected: `✓ Build kase_v2 OK` (sec_confirm.c compiles for non-dongle; `K_SEC_CONFIRM`
authorizes a never-armed gate = harmless no-op).

- [ ] **Step 5: Commit**

```bash
git add main/CMakeLists.txt main/main.c
git commit -m "build(sec): gate sec_store/cdc_sec to dongle, wire init, compile sec_confirm for all roles"
```
(Adjust the `git add` path in step 5 to whatever file holds the dongle init site.)

---

## Self-review notes (done while writing)

- **Spec coverage:** §5.1 sec_store → Task 3; §5.2 CDC provisioning → Task 4; §5.4 require-keypress
  (`K_SEC_CONFIRM` + state machine) → Tasks 1, 2, 5; §7 I1 (no secret read-back) → Task 4
  `test_list_has_no_secret`; §12 module layout + gating → Task 6. **Out of scope here (Plan 2):**
  §5.3 OTP HID, §5.5 cr_hmac, the actual CR response path — these need the protocol spike and are
  intentionally deferred (no placeholder code written for them).
- **Type consistency:** `sec_slot_t`, `SEC_N_SLOTS`, `SEC_LABEL_LEN`, `SEC_SECRET_MAX`,
  `sec_confirm_state_t`, `sec_confirm_arm/authorize/poll`, `sec_cmd_parse_set_slot/build_list`,
  `KS_CMD_SEC_*`, `K_SEC_CONFIRM` are used identically across tasks.
- **Open verification points flagged inline:** `KS_STATUS_ERR_PARAM` name (Task 4 Step 4),
  `memmem`/`_GNU_SOURCE` (Task 4 Step 2), `main/security` include path (Task 6 Step 1), dongle
  init site (Task 6 Step 2). Each says exactly how to resolve without guessing.

## Next: Plan 2 (separate)

`cr_hmac` (HMAC-SHA1) + YubiKey OTP HID transport, after a research spike pins the YubiKey
OTP HID challenge-response frame + "touch required" handshake against the
`yubikey-personalization` / KeePassXC sources. Plan 2 wires `otp_hid` → `sec_confirm_arm` →
(physical confirm via this plan's gate) → `cr_hmac` → response.
```
