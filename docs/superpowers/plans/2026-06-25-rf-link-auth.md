# RF Link Authentication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Authenticate the NRF24 half→dongle link (per-set shared key + truncated HMAC-SHA1 + session-nonce anti-replay) so the dongle accepts input only from genuine paired halves — closing OTA keystroke injection and remote `K_SEC_CONFIRM` forgery.

**Architecture:** A pure, host-tested core `main/security/rf_auth.c` (HMAC injected as a hook, like the openpgp_card hooks) does tag/seal/check. `rf_packet.h` gains the trailer + `PKT_TYPE_AUTH_INIT`. `rf_rx_task` (dongle) verifies; `half_scan_task` (half) seals. The shared key is provisioned over USB CDC (`KS_CMD_RF_KEY_SET`) into the dongle's encrypted NVS and each half's NVS — never over RF.

**Tech Stack:** C11, ESP-IDF 5.5, existing host test harness (`test/`, `TEST_ASSERT`), `cr_hmac_sha1` (mbedtls, target), a vendored host SHA1 for tests. Spec: `docs/superpowers/specs/2026-06-25-rf-link-auth-design.md`.

---

## File structure

- Create `main/security/rf_auth.h` — public API + constants (`RF_AUTH_*`), `rf_auth_rx_t`, the injected `rf_auth_hmac_fn` type.
- Create `main/security/rf_auth.c` — pure logic: `rf_auth_tag`, `rf_auth_seal`, `rf_auth_check`, `rf_auth_init_tag`, `rf_auth_set_hmac`. No ESP-IDF deps.
- Create `test/vendor/sha1.c` + `sha1.h` — public-domain SHA1 + a small `test_hmac_sha1` wrapper, host-only, with one RFC-2202 KAT (proves it matches the target's mbedtls HMAC, which is also correct & deterministic).
- Create `test/test_rf_auth.c` — host tests; register in `test/test_main.c` + `test/CMakeLists.txt`.
- Modify `main/comm/rf/rf_packet.h` — `PKT_TYPE_AUTH_INIT` (0x4), trailer encode/decode helpers, `RF_PKT_MAX`.
- Modify `main/comm/rf/rf_rx_task.c` — dongle: bind `cr_hmac_sha1`, load `rf_link_key`, send `AUTH_INIT` on link-up, verify trailer in `drain_radio` (per-half `rf_auth_rx_t`).
- Modify `main/comm/rf/half_scan_task.c` — half: bind hmac, load key, adopt nonce from `AUTH_INIT`, seal every input packet, manage counter.
- Modify `main/comm/cdc/cdc_binary_protocol.h` — `KS_CMD_RF_KEY_SET = 0xB9`.
- Modify `main/comm/cdc/cdc_dongle_cmds.c` — handler writes `rf_link_key` (32 B) to NVS namespace `storage`.
- Create `main/comm/cdc/cdc_half_provision.c` — half-side `KS_CMD_RF_KEY_SET` (replaces the no-op stub path).
- Create `scripts/kase-rf-provision.sh` — generate key + push to dongle + each half.

NVS: `rf_link_key` (32 B) in namespace `storage` on dongle + each half. No counter/epoch persistence (session nonce makes it unnecessary).

---

### Task 1: Vendored host SHA1 + HMAC for tests (KAT)

**Files:**
- Create: `test/vendor/sha1.h`, `test/vendor/sha1.c`
- Test: covered by Task 2's runner; this task adds a self-check `main`-callable KAT used by `test_rf_auth`.

- [ ] **Step 1: Add a public-domain SHA1** in `test/vendor/sha1.c` exposing
  `void sha1(const uint8_t *msg, size_t len, uint8_t out[20]);` (use the
  well-known Steve Reid public-domain implementation verbatim) and a wrapper:

```c
/* test/vendor/sha1.h */
#pragma once
#include <stdint.h>
#include <stddef.h>
void sha1(const uint8_t *msg, size_t len, uint8_t out[20]);
/* HMAC-SHA1 with the SAME signature as the target cr_hmac_sha1, for injection. */
int test_hmac_sha1(const uint8_t *key, uint16_t key_len,
                   const uint8_t *msg, uint16_t msg_len, uint8_t out20[20]);
```

```c
/* test/vendor/sha1.c — HMAC wrapper (SHA1 body: paste Steve Reid SHA1 above) */
#include "sha1.h"
#include <string.h>
int test_hmac_sha1(const uint8_t *key, uint16_t key_len,
                   const uint8_t *msg, uint16_t msg_len, uint8_t out20[20]) {
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[20];
    if (key_len > 64) { sha1(key, key_len, k); } else { memcpy(k, key, key_len); }
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    uint8_t buf[64 + 4096]; memcpy(buf, ipad, 64); memcpy(buf + 64, msg, msg_len);
    sha1(buf, 64 + msg_len, inner);
    uint8_t buf2[64 + 20]; memcpy(buf2, opad, 64); memcpy(buf2 + 64, inner, 20);
    sha1(buf2, 84, out20);
    return 1; /* mimic cr_hmac_sha1 returning true */
}
```

- [ ] **Step 2: Add a KAT** (used in Task 2 test): RFC 2202 test case 1 —
  key = 20×0x0b, data = "Hi There", expected HMAC-SHA1 =
  `b617318655057264e28bc0b6fb378c8ef146be00`.

- [ ] **Step 3: Commit**

```bash
git add test/vendor/sha1.c test/vendor/sha1.h
git commit -m "test(rf): vendored host SHA1+HMAC for rf_auth tests"
```

---

### Task 2: `rf_auth.h` + `rf_auth_tag` (injected HMAC) — TDD

**Files:**
- Create: `main/security/rf_auth.h`, `main/security/rf_auth.c`
- Test: `test/test_rf_auth.c`; register in `test/test_main.c`, `test/CMakeLists.txt`

- [ ] **Step 1: Define the API** in `rf_auth.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RF_AUTH_KEY_LEN     32
#define RF_AUTH_NONCE_LEN   4
#define RF_AUTH_CTR_LEN     3          /* 24-bit counter on the wire */
#define RF_AUTH_TAG_LEN     8          /* truncated HMAC-SHA1 */
#define RF_AUTH_TRAILER_LEN (RF_AUTH_CTR_LEN + RF_AUTH_TAG_LEN)   /* 11 */

/* Injected HMAC-SHA1 (same signature as cr_hmac_sha1). out20 = full 20-byte tag. */
typedef bool (*rf_auth_hmac_fn)(const uint8_t *key, uint16_t key_len,
                                const uint8_t *msg, uint16_t msg_len, uint8_t out20[20]);
void rf_auth_set_hmac(rf_auth_hmac_fn fn);

/* tag = HMAC(key, nonce ‖ body ‖ counter_le3)[0..7]. body = framed packet bytes
 * (type+flags byte included). */
bool rf_auth_tag(const uint8_t key[RF_AUTH_KEY_LEN], const uint8_t nonce[RF_AUTH_NONCE_LEN],
                 const uint8_t *body, uint16_t body_len, uint32_t counter,
                 uint8_t tag_out[RF_AUTH_TAG_LEN]);
```

- [ ] **Step 2: Write the failing test** in `test/test_rf_auth.c`:

```c
#include "test_framework.h"
#include "../main/security/rf_auth.h"
#include "vendor/sha1.h"
#include <string.h>

static void test_rf_auth_kat_and_tag(void) {
    /* the vendored host HMAC must be a correct HMAC-SHA1 (RFC 2202 #1) */
    uint8_t key20[20]; memset(key20, 0x0b, 20); uint8_t out[20];
    test_hmac_sha1(key20, 20, (const uint8_t*)"Hi There", 8, out);
    static const uint8_t exp[20] = {0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,
                                    0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00};
    TEST_ASSERT(memcmp(out, exp, 20) == 0, "vendored HMAC-SHA1 matches RFC2202 #1");

    rf_auth_set_hmac(test_hmac_sha1);
    uint8_t key[32]; memset(key, 0x11, 32);
    uint8_t nonce[4] = {1,2,3,4};
    uint8_t body[3] = {0x11, 0x23, 0x05};
    uint8_t tag_a[8], tag_b[8];
    TEST_ASSERT(rf_auth_tag(key, nonce, body, 3, 7, tag_a), "tag ok");
    /* deterministic */
    TEST_ASSERT(rf_auth_tag(key, nonce, body, 3, 7, tag_b) && memcmp(tag_a,tag_b,8)==0, "deterministic");
    /* counter change → different tag */
    rf_auth_tag(key, nonce, body, 3, 8, tag_b);
    TEST_ASSERT(memcmp(tag_a, tag_b, 8) != 0, "counter affects tag");
}
void test_rf_auth(void) { TEST_SUITE("rf_auth"); test_rf_auth_kat_and_tag(); }
```

- [ ] **Step 3: Register** — add `extern void test_rf_auth(void);` + `test_rf_auth();` in `test/test_main.c`; add `test_rf_auth.c`, `vendor/sha1.c`, and `../main/security/rf_auth.c` to `test/CMakeLists.txt` `set(SOURCES ...)`.

- [ ] **Step 4: Run, expect FAIL** (link error: `rf_auth_tag` undefined).
  Run: `cd test && rm -rf build && cmake -S . -B build >/dev/null && cmake --build build 2>&1 | tail -3`

- [ ] **Step 5: Implement** `rf_auth.c`:

```c
#include "rf_auth.h"
#include <string.h>
static rf_auth_hmac_fn s_hmac;
void rf_auth_set_hmac(rf_auth_hmac_fn fn) { s_hmac = fn; }

bool rf_auth_tag(const uint8_t key[32], const uint8_t nonce[4],
                 const uint8_t *body, uint16_t body_len, uint32_t counter,
                 uint8_t tag_out[8]) {
    if (!s_hmac || body_len > 64) return false;
    uint8_t msg[4 + 64 + 3]; uint16_t n = 0;
    memcpy(msg + n, nonce, 4); n += 4;
    memcpy(msg + n, body, body_len); n += body_len;
    msg[n++] = (uint8_t)(counter & 0xFF);
    msg[n++] = (uint8_t)((counter >> 8) & 0xFF);
    msg[n++] = (uint8_t)((counter >> 16) & 0xFF);
    uint8_t full[20];
    if (!s_hmac(key, 32, msg, n, full)) return false;
    memcpy(tag_out, full, 8);
    return true;
}
```

- [ ] **Step 6: Run, expect PASS.** Run: `./test/build/test_runner 2>&1 | grep rf_auth`

- [ ] **Step 7: Commit**

```bash
git add main/security/rf_auth.h main/security/rf_auth.c test/test_rf_auth.c test/test_main.c test/CMakeLists.txt
git commit -m "feat(rf): rf_auth_tag — truncated HMAC-SHA1 over nonce+body+counter (TDD)"
```

---

### Task 3: `rf_auth_seal` (TX trailer) — TDD

**Files:** Modify `main/security/rf_auth.{h,c}`, `test/test_rf_auth.c`

- [ ] **Step 1: Declare** in `rf_auth.h`:

```c
/* Append [counter_le3][tag8] after body (buf must hold body_len + RF_AUTH_TRAILER_LEN).
 * Returns new total length, or 0 on error. */
uint16_t rf_auth_seal(const uint8_t key[RF_AUTH_KEY_LEN], const uint8_t nonce[RF_AUTH_NONCE_LEN],
                      uint8_t *buf, uint16_t body_len, uint32_t counter);
```

- [ ] **Step 2: Failing test** (add to `test_rf_auth.c`, call from `test_rf_auth`):

```c
static void test_rf_auth_seal(void) {
    rf_auth_set_hmac(test_hmac_sha1);
    uint8_t key[32]; memset(key, 0x22, 32); uint8_t nonce[4]={9,8,7,6};
    uint8_t buf[32] = {0x11, 0x23, 0x05};
    uint16_t total = rf_auth_seal(key, nonce, buf, 3, 42);
    TEST_ASSERT_EQ(total, 14, "key pkt 3 -> 14 sealed");
    uint8_t ctr[3] = { buf[3], buf[4], buf[5] };
    TEST_ASSERT(ctr[0]==42 && ctr[1]==0 && ctr[2]==0, "counter le3 written");
    uint8_t tag[8]; rf_auth_tag(key, nonce, buf, 3, 42, tag);
    TEST_ASSERT(memcmp(buf+6, tag, 8)==0, "tag appended after counter");
}
```

- [ ] **Step 3: Run, expect FAIL.** `cmake --build test/build 2>&1 | tail -2`

- [ ] **Step 4: Implement** in `rf_auth.c`:

```c
uint16_t rf_auth_seal(const uint8_t key[32], const uint8_t nonce[4],
                      uint8_t *buf, uint16_t body_len, uint32_t counter) {
    buf[body_len + 0] = (uint8_t)(counter & 0xFF);
    buf[body_len + 1] = (uint8_t)((counter >> 8) & 0xFF);
    buf[body_len + 2] = (uint8_t)((counter >> 16) & 0xFF);
    uint8_t tag[8];
    if (!rf_auth_tag(key, nonce, buf, body_len, counter, tag)) return 0;
    memcpy(buf + body_len + 3, tag, 8);
    return body_len + RF_AUTH_TRAILER_LEN;
}
```

- [ ] **Step 5: Run, expect PASS.** `./test/build/test_runner | grep rf_auth`
- [ ] **Step 6: Commit** `git commit -am "feat(rf): rf_auth_seal — append counter+tag trailer (TDD)"`

---

### Task 4: `rf_auth_check` (RX verify + monotonic counter) — TDD

**Files:** Modify `main/security/rf_auth.{h,c}`, `test/test_rf_auth.c`

- [ ] **Step 1: Declare** in `rf_auth.h`:

```c
typedef struct {
    uint8_t  nonce[RF_AUTH_NONCE_LEN];
    uint32_t last_ctr;   /* highest accepted counter this session */
    bool     have_nonce; /* set once AUTH_INIT adopted */
    bool     have_ctr;   /* set after first accepted packet */
} rf_auth_rx_t;

/* Verify a sealed packet (buf[0..total_len), trailer included). body_len =
 * total_len - RF_AUTH_TRAILER_LEN. Accept iff tag valid under st->nonce AND
 * counter strictly greater than st->last_ctr (or first packet). Updates last_ctr. */
bool rf_auth_check(const uint8_t key[RF_AUTH_KEY_LEN], rf_auth_rx_t *st,
                   const uint8_t *buf, uint16_t total_len);
```

- [ ] **Step 2: Failing test:**

```c
static void test_rf_auth_check(void) {
    rf_auth_set_hmac(test_hmac_sha1);
    uint8_t key[32]; memset(key, 0x33, 32);
    rf_auth_rx_t st; memset(&st, 0, sizeof(st));
    memcpy(st.nonce, (uint8_t[]){4,4,4,4}, 4); st.have_nonce = true;

    uint8_t p[32] = {0x11, 0x23, 0x05};
    rf_auth_seal(key, st.nonce, p, 3, 10);
    TEST_ASSERT(rf_auth_check(key, &st, p, 14), "fresh counter 10 accepted");
    TEST_ASSERT(!rf_auth_check(key, &st, p, 14), "replay of counter 10 rejected");

    uint8_t p2[32] = {0x11, 0x23, 0x06}; rf_auth_seal(key, st.nonce, p2, 3, 11);
    TEST_ASSERT(rf_auth_check(key, &st, p2, 14), "counter 11 accepted");

    uint8_t p3[32] = {0x11, 0x23, 0x07}; rf_auth_seal(key, st.nonce, p3, 3, 5);
    TEST_ASSERT(!rf_auth_check(key, &st, p3, 14), "lower counter 5 rejected");

    /* tamper a body byte → tag fails */
    uint8_t p4[32] = {0x11, 0x23, 0x08}; rf_auth_seal(key, st.nonce, p4, 3, 12);
    p4[1] ^= 0x01;
    TEST_ASSERT(!rf_auth_check(key, &st, p4, 14), "tampered body rejected");

    /* wrong nonce (different session) → tag fails */
    rf_auth_rx_t st2; memset(&st2, 0, sizeof(st2));
    memcpy(st2.nonce, (uint8_t[]){9,9,9,9}, 4); st2.have_nonce = true;
    uint8_t p5[32] = {0x11, 0x23, 0x09}; rf_auth_seal(key, (uint8_t[]){4,4,4,4}, p5, 3, 1);
    TEST_ASSERT(!rf_auth_check(key, &st2, p5, 14), "cross-session nonce rejected");
}
```

- [ ] **Step 3: Run, expect FAIL.**
- [ ] **Step 4: Implement** in `rf_auth.c`:

```c
bool rf_auth_check(const uint8_t key[32], rf_auth_rx_t *st,
                   const uint8_t *buf, uint16_t total_len) {
    if (!st->have_nonce || total_len < RF_AUTH_TRAILER_LEN) return false;
    uint16_t body_len = total_len - RF_AUTH_TRAILER_LEN;
    uint32_t ctr = (uint32_t)buf[body_len] |
                   ((uint32_t)buf[body_len+1] << 8) | ((uint32_t)buf[body_len+2] << 16);
    uint8_t want[8];
    if (!rf_auth_tag(key, st->nonce, buf, body_len, ctr, want)) return false;
    /* constant-time compare of the 8-byte tag */
    uint8_t diff = 0; for (int i = 0; i < 8; i++) diff |= buf[body_len+3+i] ^ want[i];
    if (diff != 0) return false;
    if (st->have_ctr && ctr <= st->last_ctr) return false;  /* replay / non-advancing */
    st->last_ctr = ctr; st->have_ctr = true;
    return true;
}
```

- [ ] **Step 5: Run, expect PASS.**
- [ ] **Step 6: Commit** `git commit -am "feat(rf): rf_auth_check — tag verify + monotonic counter gate (TDD)"`

---

### Task 5: `rf_auth_init_tag` (AUTH_INIT handshake tag) — TDD

**Files:** Modify `main/security/rf_auth.{h,c}`, `test/test_rf_auth.c`

- [ ] **Step 1: Declare:**

```c
/* AUTH_INIT tag = HMAC(key, "AINIT" ‖ nonce)[0..7]. */
bool rf_auth_init_tag(const uint8_t key[RF_AUTH_KEY_LEN],
                      const uint8_t nonce[RF_AUTH_NONCE_LEN], uint8_t tag_out[RF_AUTH_TAG_LEN]);
```

- [ ] **Step 2: Failing test:**

```c
static void test_rf_auth_init(void) {
    rf_auth_set_hmac(test_hmac_sha1);
    uint8_t key[32]; memset(key, 0x44, 32); uint8_t nonce[4]={1,1,2,2};
    uint8_t t1[8], t2[8];
    TEST_ASSERT(rf_auth_init_tag(key, nonce, t1), "init tag ok");
    uint8_t nonce2[4]={1,1,2,3};
    rf_auth_init_tag(key, nonce2, t2);
    TEST_ASSERT(memcmp(t1, t2, 8) != 0, "different nonce → different init tag");
}
```

- [ ] **Step 3: Run, expect FAIL.**
- [ ] **Step 4: Implement:**

```c
bool rf_auth_init_tag(const uint8_t key[32], const uint8_t nonce[4], uint8_t tag_out[8]) {
    if (!s_hmac) return false;
    uint8_t msg[5 + 4] = { 'A','I','N','I','T' };
    memcpy(msg + 5, nonce, 4);
    uint8_t full[20];
    if (!s_hmac(key, 32, msg, 9, full)) return false;
    memcpy(tag_out, full, 8);
    return true;
}
```

- [ ] **Step 5: Run, expect PASS.** Then run the FULL suite to confirm no regression:
  `./scripts/check.sh --host-only` → expect "✓ check.sh: tout vert".
- [ ] **Step 6: Commit** `git commit -am "feat(rf): rf_auth_init_tag — AUTH_INIT handshake tag (TDD)"`

---

### Task 6: `rf_packet.h` — AUTH_INIT type + trailer-aware sizes — TDD

**Files:** Modify `main/comm/rf/rf_packet.h`, `test/test_rf_packet.c`

- [ ] **Step 1: Failing test** in `test_rf_packet.c` (mirror existing tests there):

```c
static void test_rf_pkt_auth_init_roundtrip(void) {
    uint8_t nonce[4]={5,6,7,8}, tag[8]; memset(tag, 0xAB, 8);
    uint8_t buf[16];
    uint16_t n = rf_encode_auth_init(buf, nonce, tag);
    TEST_ASSERT_EQ(n, 13, "auth_init = 1+4+8");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_AUTH_INIT, "type 0x4");
    uint8_t no[4], tg[8];
    TEST_ASSERT(rf_decode_auth_init(buf, n, no, tg), "decode ok");
    TEST_ASSERT(memcmp(no,nonce,4)==0 && memcmp(tg,tag,8)==0, "fields roundtrip");
}
```
Add `test_rf_pkt_auth_init_roundtrip();` to the `test_rf_packet` runner.

- [ ] **Step 2: Run, expect FAIL** (undefined `PKT_TYPE_AUTH_INIT`/encoders).
- [ ] **Step 3: Implement** in `rf_packet.h`: add `#define PKT_TYPE_AUTH_INIT 0x4`, `#define RF_PKT_MAX 32`, and:

```c
static inline uint16_t rf_encode_auth_init(uint8_t *buf, const uint8_t nonce[4], const uint8_t tag[8]) {
    buf[0] = (PKT_TYPE_AUTH_INIT << 4);
    memcpy(buf + 1, nonce, 4);
    memcpy(buf + 5, tag, 8);
    return 13;
}
static inline bool rf_decode_auth_init(const uint8_t *buf, uint16_t len, uint8_t nonce_out[4], uint8_t tag_out[8]) {
    if (len < 13 || rf_packet_type(buf, len) != PKT_TYPE_AUTH_INIT) return false;
    memcpy(nonce_out, buf + 1, 4); memcpy(tag_out, buf + 5, 8);
    return true;
}
```

- [ ] **Step 4: Run, expect PASS.** `./scripts/check.sh --host-only`
- [ ] **Step 5: Commit** `git commit -am "feat(rf): PKT_TYPE_AUTH_INIT encode/decode (TDD)"`

---

### Task 7: Target HMAC binding + `rf_link_key` NVS load (dongle + half)

**Files:** Modify `main/comm/rf/rf_rx_task.c` (dongle init), `main/comm/rf/half_scan_task.c` (half init). No host test (firmware NVS/HMAC).

- [ ] **Step 1:** In both files' init, bind the HMAC and load the key once:

```c
#include "rf_auth.h"
#include "cr_hmac.h"
#include "nvs.h"
#include "nvs_utils.h"
#include "keyboard_config.h"   /* STORAGE_NAMESPACE */

static uint8_t s_rf_key[RF_AUTH_KEY_LEN];
static bool    s_rf_key_set;

static void rf_auth_boot_init(void) {
    rf_auth_set_hmac(cr_hmac_sha1);
    size_t len = sizeof(s_rf_key);
    nvs_handle_t h;
    if (nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "rf_link_key", s_rf_key, &len) == ESP_OK && len == RF_AUTH_KEY_LEN)
            s_rf_key_set = true;
        nvs_close(h);
    }
    if (!s_rf_key_set) ESP_LOGW(TAG, "rf_link_key absent — RF auth fails closed until provisioned");
}
```
Call `rf_auth_boot_init()` from the existing task setup (dongle: near `rf_rx_task` start; half: near `half_scan_task` start).

- [ ] **Step 2: Build** dongle + a half: 
  `source ~/esp/esp-idf/export.sh && idf.py -B build_kase_dongle -DBOARD=kase_dongle -DSDKCONFIG=build_kase_dongle/sdkconfig build && idf.py -B build_kase_half_left -DBOARD=kase_half_left -DSDKCONFIG=build_kase_half_left/sdkconfig build`
  Expected: both "Project build complete".
- [ ] **Step 3: Commit** `git commit -am "feat(rf): bind cr_hmac_sha1 + load rf_link_key on boot (dongle+half)"`

---

### Task 8: Dongle RX — AUTH_INIT on link-up + verify trailer

**Files:** Modify `main/comm/rf/rf_rx_task.c` (`drain_radio`, the heartbeat/link-up path). Per-half `rf_auth_rx_t s_auth[2]`.

- [ ] **Step 1:** Add `static rf_auth_rx_t s_auth[2];` (index by `half`). On link-up (when `hb_reconcile` sets `link_up` from false→true, in the `PKT_TYPE_HEARTBEAT` branch), generate + send AUTH_INIT:

```c
uint8_t nonce[4]; esp_fill_random(nonce, 4);
memcpy(s_auth[half].nonce, nonce, 4);
s_auth[half].have_nonce = true; s_auth[half].have_ctr = false;
uint8_t tag[8], pkt[13];
if (s_rf_key_set && rf_auth_init_tag(s_rf_key, nonce, tag)) {
    rf_encode_auth_init(pkt, nonce, tag);
    rf_driver_send(radio, pkt, 13);
}
```

- [ ] **Step 2:** In `drain_radio`, for `PKT_TYPE_KEY` / `PKT_TYPE_HEARTBEAT` / `PKT_TYPE_TRACKPAD`, BEFORE decoding/applying, verify:

```c
if (!s_rf_key_set || !rf_auth_check(s_rf_key, &s_auth[half], buf, n)) {
    radio->pkt_auth_fail++;   /* add this counter to rf_radio_t */
    continue;                  /* drop — never reaches MATRIX_STATE / sec_confirm */
}
n -= RF_AUTH_TRAILER_LEN;      /* hand the decoders the body only */
```
(Keep the existing `hb_apply_key` OOB guard from commit e6dd21f5.)

- [ ] **Step 3: Build** dongle (as Task 7 Step 2). Expected: complete.
- [ ] **Step 4: Commit** `git commit -am "feat(rf): dongle sends AUTH_INIT on link-up + verifies input trailers (fail-closed)"`

---

### Task 9: Half TX — adopt nonce + seal every input packet

**Files:** Modify `main/comm/rf/half_scan_task.c` (key TX ~line 180/340, heartbeat, trackpad), and the RX path that receives AUTH_INIT.

- [ ] **Step 1:** Add `static uint8_t s_nonce[4]; static bool s_have_nonce; static uint32_t s_ctr;`. On receiving `PKT_TYPE_AUTH_INIT` (verify its tag with `rf_auth_init_tag`), adopt:

```c
uint8_t no[4], tg[8], want[8];
if (rf_decode_auth_init(buf, n, no, tg) && s_rf_key_set &&
    rf_auth_init_tag(s_rf_key, no, want) && memcmp(tg, want, 8) == 0) {
    memcpy(s_nonce, no, 4); s_have_nonce = true; s_ctr = 0;
}
```

- [ ] **Step 2:** Replace each `rf_driver_send(&s_radio, buf, N)` for KEY/HEARTBEAT/TRACKPAD with a sealed send:

```c
if (s_rf_key_set && s_have_nonce) {
    uint16_t total = rf_auth_seal(s_rf_key, s_nonce, buf, N, ++s_ctr);
    if (total) rf_driver_send(&s_radio, buf, total);
}   /* else: no nonce yet → drop (fail-closed; AUTH_INIT will arrive on link-up) */
```
Ensure each TX `buf` is sized `>= N + RF_AUTH_TRAILER_LEN` (≥ 20).

- [ ] **Step 3: Build** half_left + half_right. Expected: complete.
- [ ] **Step 4: Commit** `git commit -am "feat(rf): half adopts session nonce + seals KEY/HEARTBEAT/TRACKPAD"`

---

### Task 10: `KS_CMD_RF_KEY_SET` — dongle + half provisioning

**Files:** Modify `main/comm/cdc/cdc_binary_protocol.h`, `main/comm/cdc/cdc_dongle_cmds.c`; Create `main/comm/cdc/cdc_half_provision.c`; Modify `main/CMakeLists.txt` (half src list).

- [ ] **Step 1:** Add to the `KS_CMD_*` enum in `cdc_binary_protocol.h`:
  `KS_CMD_RF_KEY_SET = 0xB9,  /* set 32-byte rf_link_key into NVS (provisioning) */`

- [ ] **Step 2:** Dongle handler in `cdc_dongle_cmds.c` (register in `dongle_cmd_table`):

```c
static void bin_cmd_rf_key_set(uint8_t cmd, const uint8_t *p, uint16_t l) {
    if (l != RF_AUTH_KEY_LEN) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    nvs_handle_t h;
    bool ok = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &h) == ESP_OK
              && nvs_set_blob(h, "rf_link_key", p, RF_AUTH_KEY_LEN) == ESP_OK
              && nvs_commit(h) == ESP_OK;
    if (h) nvs_close(h);
    if (ok) ks_respond(cmd, KS_STATUS_OK, NULL, 0);
    else    ks_respond_err(cmd, KS_STATUS_ERR_NVS);
}
/* add { KS_CMD_RF_KEY_SET, bin_cmd_rf_key_set } to dongle_cmd_table */
```

- [ ] **Step 3:** Half provisioning in new `cdc_half_provision.c` — bring up a minimal CDC that handles ONLY `KS_CMD_RF_KEY_SET` (same NVS write as Step 2). Wire it in place of the no-op `cdc_half_stubs.c` path (or alongside) under `CONFIG_KASE_DEVICE_ROLE_HALF`; add to the half src list in `main/CMakeLists.txt:121`.

- [ ] **Step 4: Build** dongle + half_left. Expected: complete.
- [ ] **Step 5: Commit** `git commit -am "feat(rf): KS_CMD_RF_KEY_SET provisioning (dongle + half CDC)"`

---

### Task 11: `scripts/kase-rf-provision.sh`

**Files:** Create `scripts/kase-rf-provision.sh`

- [ ] **Step 1:** Script that generates one 32-byte key and pushes it to the dongle + each half over CDC (KS frame, CRC-8 — reuse the framing from `kase-pgp-setup.sh`/`docs/CDC_BINARY_PROTOCOL.md`):

```bash
#!/usr/bin/env bash
set -euo pipefail
KEY=$(head -c32 /dev/urandom | xxd -p -c32)   # 64 hex chars
echo "Generated per-set RF key (keep secret). Provisioning all devices..."
for dev in "$@"; do                            # usage: kase-rf-provision.sh /dev/ttyACM0 /dev/ttyACMx ...
  echo "  -> $dev"
  python3 scripts/ks_send.py "$dev" 0xB9 "$KEY"   # ks_send.py: frame KS cmd=0xB9 payload=hex, check KR status
done
echo "Done. Re-flash any device only with the matching firmware (flag day)."
```
Add a tiny `scripts/ks_send.py` helper (KS framing + CRC-8 + read KR status) if one does not already exist; model it on the protocol in `docs/CDC_BINARY_PROTOCOL.md`.

- [ ] **Step 2: Test** (syntax + dry behavior): `bash -n scripts/kase-rf-provision.sh && echo OK`
- [ ] **Step 3: Commit** `git commit -am "feat(rf): kase-rf-provision.sh — generate + push rf_link_key to all devices"`

---

### Task 12: Full regression + HW validation checklist

**Files:** none (validation).

- [ ] **Step 1:** `./scripts/check.sh` → all 6 boards build + host green (note: dongle/half builds need the secure-boot signing key present).
- [ ] **Step 2: HW flag-day** (manual, document results in the spec): flash dongle + both halves with the new firmware; run `scripts/kase-rf-provision.sh` for all three; confirm:
  - normal typing works (sealed packets accepted),
  - an UNPROVISIONED half does not type (fail-closed),
  - a captured frame replayed later is dropped (counter ≤ live; watch `pkt_auth_fail` via `KS_CMD_RF_STATUS`),
  - `K_SEC_CONFIRM` from a forged/unsigned frame does NOT authorize (sec_confirm not armed).
- [ ] **Step 3: Commit** any doc updates `git commit -am "docs(rf): record RF-auth HW flag-day validation"`

---

## Notes for the executor

- **Flag day:** dongle + both halves must run this firmware AND share the same provisioned key. A mixed set fails closed (no nonce/key → dropped). Provision before relying on the keyboard.
- **Fail-closed is intentional:** an un-provisioned device does not type. The provision script runs once at setup.
- Keep the half's new CDC **provisioning-only** — do not expose a general command surface on the half.
- Preserve the `hb_apply_key` OOB guard (commit e6dd21f5) — `rf_auth_check` runs first, but the guard stays as defence in depth.
