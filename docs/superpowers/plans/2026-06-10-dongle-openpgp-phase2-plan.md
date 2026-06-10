# Dongle OpenPGP Card — Phase 2 (complete ECC card: decrypt + SSH auth + on-device keygen) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Phase-1 signing card into a complete ECC OpenPGP card: 3 key slots (SIG/DEC/AUT), `gpg --decrypt` via X25519 ECDH (`PSO:DECIPHER`, UIF D7), SSH login via `INTERNAL AUTHENTICATE` + `gpg-agent --enable-ssh-support` (P-256, UIF D8), and on-device `GENERATE ASYMMETRIC KEY PAIR` so private keys can be born on the dongle and never exist anywhere else.

**Architecture:** The Phase-1 stack (CCID `ccid.c` → applet `openpgp_card.c` over `apdu.c`/`openpgp_do.c`, crypto via injected hooks) grows: (1) the single `s_key` becomes `s_keys[3]` with a versioned NVS blob v2 + transparent v1 migration; (2) the hook vtable becomes algo-aware and gains `ecdh` + `genkey`; (3) X25519 lands in `openpgp_crypto.c` (mbedtls `MBEDTLS_ECP_DP_CURVE25519` — **verified enabled** in `build_kase_dongle/sdkconfig` 2026-06-10, ECDH-only which is exactly what decryption needs); (4) three new INS paths: `PSO:DECIPHER` (0x2A/80/86), `INTERNAL AUTHENTICATE` (0x88), `GENERATE` (0x47/80), plus TERMINATE/ACTIVATE factory reset.

**Tech Stack:** C, ESP-IDF 5.5, mbedtls (ECDSA secp256r1 + X25519 over CURVE25519, HW MPI), host test harness (`test/`, `-DTEST_HOST`), fuzzer `test/fuzz/`. All dongle-gated under `CONFIG_KASE_SEC_OPENPGP`. Spec: `docs/superpowers/specs/2026-06-09-dongle-openpgp-card-design.md` (§8 roadmap, §7b Phase-1 result).

**Decisions ratified (2026-06-10, Mae):**
- **RSA PARKED** (verified: Mae's SSH keys are both Ed25519, GPG keyring empty → fresh ECC keys; no RSA need). Consequence: **no extended-APDU work** — every ECC payload fits the existing short-APDU + `dwMaxCCIDMessageLength=271` path. RSA returns in a future phase only on real need.
- **Decrypt = X25519** (cv25519, gpg's modern default). C2 factory attrs change to the cv25519 OID; the stale Phase-1 P-256-ECDH C2 value is migrated in place (safe: `PSO:DECIPHER` did not exist in Phase 1, no key depends on it).
- **Auth = P-256 ECDSA** (Ed25519 sign absent from the ESP-IDF mbedtls fork — Phase-1 verdict stands). SSH key type: `ecdsa-sha2-nistp256` (accepted by GitLab/GitHub/OpenSSH).
- **Secure boot V2 + flash encryption: NO, documented.** Threat model stays host-malware-only; the dongle stays 100% reprogrammable (eFuses are irreversible). Decision + re-evaluation trigger ("before distributing hardware to third parties") written into the spec in Task 7.
- **Post-quantum: P-256/X25519 kept, posture documented (2026-06-10, Mae).** No production OpenPGP smartcard or GnuPG card format is post-quantum in 2026 (not the YubiKey, Nitrokey, or Gnuk); ML-DSA/ML-KEM are not yet wired into the gpg card path. The card's algorithms are classical by necessity. Mitigation is **trivial rotation**: the day gpg ships PQC card algos, regenerate keys on-device (Task 5) — nothing is fused, the dongle stays reprogrammable. Auth/signature (P-256) need a *live* quantum attacker (does not exist; est. 2035+). The only harvest-now-decrypt-later exposure is the **decrypt slot** (X25519), which is acceptable because the dongle's role is dev identity (git sign + SSH), not long-lived message confidentiality. **The user's server-side "quantum" SSH warnings concern the OpenSSH session KEX — a different layer from the card key — fixed host-side with a PQ KEX (`sntrup761x25519-sha512`), documented in Task 7.**

---

## Prerequisites (host, one-time — same as Phase 1)

- gpg via `nix-shell -p gnupg` (2.4.9). For multi-checkpoint continuity use a **persistent test GNUPGHOME**: `export GNUPGHOME=$HOME/kase-pgp-test` (create once, `chmod 700`). Real `~/.gnupg` only in the final Task 7 end-to-end.
- Flash: `idf.py -B build_kase_dongle -p /dev/ttyUSB0 flash` (CH340; app @ 0x20000, NVS preserved).
- Runtime logs (dongle is CONSOLE_NONE): temporarily add `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` + `CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y` to `sdkconfig.defaults.dongle`, `rm build_kase_dongle/sdkconfig`, rebuild → logs on `/dev/ttyUSB0`. **REVERT before committing.**
- scdaemon debug: `echo -e 'debug-level guru\nlog-file /tmp/scd.log' > $GNUPGHOME/scdaemon.conf`; `gpgconf --kill scdaemon` between attempts.
- PINs: PW1 `123456` (user — sign/decrypt/auth), PW3 `12345678` (admin — import/generate/PUT DATA).

## Phase-0/1 gotchas already learned (do NOT rediscover)

- **ZLP**: bulk-IN response of exactly N×64 B needs a zero-length packet (handled in `ccid_drv_xfer` — don't break it).
- **READ PUBLIC KEY (0x47/81)** required for keytocard; `.pubkey` hook MUST be wired in `s_dongle_hooks` (was forgotten once).
- **Any GET DATA → 6A88 aborts gpg's LEARN** — new DOs must be served (or already covered by `ensure_defaults`).
- **ATR ends `... 90 00 CD`** (TCK mandatory). **`ccid_init()` force-link** from `kase_tinyusb_init()`.
- **Endpoint budget**: 4 IN max — Phase 2 adds **no** endpoint, no descriptor change.
- Use the **USER PIN 123456** to sign/decrypt/auth (PW3 → "Bad PIN"). `keytocard` stub appears only when 0x47 works AND fingerprints match.
- The dongle is the daily-driver keyboard — after every flash, verify typing still works.

## File structure

- Modify `main/security/openpgp_card.h` — hooks v2 (algo-aware `pubkey`, new `ecdh`/`genkey`), slot enum, algo constants moved here.
- Modify `main/security/openpgp_card.c` — `s_keys[3]`, blob v2 + migration, import routing, PSO:DECIPHER, INTERNAL AUTHENTICATE, GENERATE, C1–C3 validation, C4 put, DO 0xDE, TERMINATE/ACTIVATE.
- Modify `main/security/openpgp_crypto.{c,h}` — X25519 ecdh/pubkey, `openpgp_crypto_genkey`, selftest KATs (RFC 7748).
- Modify `main/security/ccid.c` — hooks v2 wiring (`dongle_pubkey` algo-aware, `dongle_ecdh`, `dongle_genkey`).
- Modify `test/test_openpgp_card.c` — fake hooks v2 + new tests (suite already registered).
- Modify `test/fuzz/fuzz_openpgp.c` — hooks v2 + new-INS corpus.
- Modify `docs/superpowers/specs/2026-06-09-dongle-openpgp-card-design.md` — §2b secure-boot decision, §8b Phase-2 result.
- Create `docs/OPENPGP_CARD.md` — user guide (setup, keygen, git signing, SSH).
- No changes: `usb_hid.c` descriptors, partition table, Kconfig personalities.

---

### Task 1: Multi-slot key store + algo-aware hooks (TDD)

The foundation everything else builds on. `s_key` (1 slot) → `s_keys[3]` (SIG=0, DEC=1, AUT=2), NVS blob v2 under a **new key `"pgp_keys"`** with transparent migration from the Phase-1 `"pgp_key"` blob, import routing by CRT tag, READ PUBLIC KEY per slot.

**Files:**
- Modify: `main/security/openpgp_card.h`
- Modify: `main/security/openpgp_card.c`
- Test: `test/test_openpgp_card.c`

- [ ] **Step 1: New public types in `openpgp_card.h`.** Replace the hooks struct and add slot/algo constants (move `PGP_ALGO_*` out of the `.c`):

```c
/* Key slots (OpenPGP: Signature / Decryption / Authentication) */
enum {
    OPENPGP_SLOT_SIG = 0,
    OPENPGP_SLOT_DEC = 1,
    OPENPGP_SLOT_AUT = 2,
    OPENPGP_SLOT_COUNT = 3,
};

/* Slot algorithm ids — match the leading byte of algo attrs C1/C2/C3.
 * 0x12 (ECDH) always means X25519 in this implementation: C2 only accepts
 * the cv25519 OID (Task 2), ECDH-P256 is not supported. */
#define PGP_ALGO_ECDSA_P256  0x13u
#define PGP_ALGO_ECDH        0x12u

typedef struct {
    /* ECDSA P-256 over `hash` with scalar d (BE). Used by SIG and AUT slots. */
    bool (*sign)(const uint8_t d[32],
                 const uint8_t *hash, uint16_t n,
                 uint8_t *out, uint16_t *out_n);
    /* UIF gate: 0 = not yet, 1 = authorized, 2 = denied/timeout. */
    int  (*confirm)(void);
    /* Derive the public key for `algo`.
     * PGP_ALGO_ECDSA_P256: writes 65 B (0x04||X||Y), *out_n = 65.
     * PGP_ALGO_ECDH (X25519): writes 32 B (RFC 7748 LE u), *out_n = 32.
     * NULL-tolerant: if NULL, READ PUBLIC KEY / GENERATE return 6A88. */
    bool (*pubkey)(uint8_t algo, const uint8_t d[32],
                   uint8_t *out, uint16_t *out_n);
    /* X25519 ECDH: shared secret = d · peer (peer = 32 B LE u-coordinate).
     * Writes 32 B, *out_n = 32. NULL-tolerant (PSO:DECIPHER → 6985). */
    bool (*ecdh)(const uint8_t d[32],
                 const uint8_t *peer, uint16_t peer_n,
                 uint8_t *out, uint16_t *out_n);
    /* Generate a private scalar for `algo` (Task 5). BE output.
     * NULL-tolerant (GENERATE 0x47/80 → 6985). */
    bool (*genkey)(uint8_t algo, uint8_t d_out[32]);
} openpgp_card_hooks_t;
```

Update the `openpgp_card_key_is_set()` doc comment: "true if a key is present in the **Signature** slot" (behavior unchanged — it keeps reporting SIG, its only current caller semantics).

- [ ] **Step 2: Failing tests.** In `test/test_openpgp_card.c`:

Update the fakes to the v2 signatures (this breaks compilation of old fakes — that IS the red step; fix signatures, keep behaviors):

```c
static bool fake_pubkey(uint8_t algo, const uint8_t d[32],
                        uint8_t *out, uint16_t *out_n)
{
    (void)d;
    if (algo == PGP_ALGO_ECDH) { memset(out, 0x66, 32); *out_n = 32; }
    else { out[0] = 0x04; memset(out + 1, 0x77, 64); *out_n = 65; }
    return true;
}

static uint8_t g_fake_ecdh_last_d[32];
static uint8_t g_fake_ecdh_last_peer[32];
static bool fake_ecdh(const uint8_t d[32], const uint8_t *peer, uint16_t peer_n,
                      uint8_t *out, uint16_t *out_n)
{
    if (peer_n != 32) return false;
    memcpy(g_fake_ecdh_last_d, d, 32);
    memcpy(g_fake_ecdh_last_peer, peer, 32);
    memset(out, 0x55, 32); *out_n = 32;   /* canned shared secret */
    return true;
}

static uint8_t g_fake_genkey_last_algo;
static bool fake_genkey(uint8_t algo, uint8_t d_out[32])
{
    g_fake_genkey_last_algo = algo;
    memset(d_out, 0xA0 + algo, 32);       /* distinguishable per algo */
    return true;
}
```

Wire `ecdh`/`genkey` into the hooks struct built in `setup_card()`. Generalize the import builder with a CRT parameter (keep the old name as a thin wrapper so existing tests stay untouched):

```c
/* Extended-header-list import for any slot CRT (0xB6 / 0xB8 / 0xA4). */
static uint16_t build_key_import_crt(uint8_t crt, const uint8_t d[32], uint8_t *buf)
{
    uint16_t i = 0;
    buf[i++] = 0x00; buf[i++] = 0xDB; buf[i++] = 0x3F; buf[i++] = 0xFF;
    buf[i++] = 0x00;                 /* Lc placeholder */
    uint16_t lc_at = 4, body = i;
    buf[i++] = 0x4D; buf[i++] = 0x00; uint16_t l4d = i - 1;
    buf[i++] = crt;  buf[i++] = 0x00;             /* B6/B8/A4, empty */
    buf[i++] = 0x7F; buf[i++] = 0x48; buf[i++] = 0x02;
    buf[i++] = 0x92; buf[i++] = 0x20;             /* template: d is 32 B */
    buf[i++] = 0x5F; buf[i++] = 0x48; buf[i++] = 0x20;
    memcpy(buf + i, d, 32); i += 32;
    buf[l4d]   = (uint8_t)(i - l4d - 1);
    buf[lc_at] = (uint8_t)(i - body);
    return i;
}
static uint16_t build_key_import(const uint8_t d[32], uint8_t *buf)
{ return build_key_import_crt(0xB6, d, buf); }   /* replaces the old body */
```

New tests:

```c
/* Import routes B6→SIG, B8→DEC, A4→AUT; each slot independent. */
static void test_import_routes_to_slots(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp); do_verify_pw3(cmd, rsp);

    uint8_t d_sig[32], d_dec[32], d_aut[32];
    memset(d_sig, 0x11, 32); memset(d_dec, 0x22, 32); memset(d_aut, 0x33, 32);

    n = build_key_import_crt(0xB6, d_sig, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import B6 ok");
    n = build_key_import_crt(0xB8, d_dec, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import B8 ok");
    n = build_key_import_crt(0xA4, d_aut, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import A4 ok");

    /* PSO:CDS must use the SIG scalar (0x11...), not DEC/AUT. */
    do_verify_pw1_sign(cmd, rsp); do_disable_uif(cmd, rsp);
    uint8_t hash[32]; memset(hash, 0x5A, 32);
    n = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "CDS ok");
    TEST_ASSERT(g_fake_sign_last_d[0] == 0x11, "CDS signed with SIG slot key");
}

/* READ PUBLIC KEY per CRT: B8 (X25519 slot) returns 7F49{86: 0x40||pub32}. */
static void test_read_pubkey_per_slot(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp); do_verify_pw3(cmd, rsp);

    /* No DEC key yet → 6A88 */
    n = build_read_pubkey(0x81, 0xB8, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A88, "no DEC key yet");

    uint8_t d_dec[32]; memset(d_dec, 0x22, 32);
    n = build_key_import_crt(0xB8, d_dec, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import B8 ok");

    n = build_read_pubkey(0x81, 0xB8, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "read DEC pubkey ok");
    /* 7F49 L { 86 0x21 0x40 <32×0x66> } — X25519 native format is 0x40-prefixed */
    TEST_ASSERT(rsp[0] == 0x7F && rsp[1] == 0x49, "outer 7F49");
    int i86 = find_index(rsp, rlen, (const uint8_t[]){0x86, 0x21, 0x40, 0x66}, 4);
    TEST_ASSERT(i86 >= 0, "86 holds 0x40-prefixed 33-byte point");
}

/* SIG-slot ops must not see DEC/AUT keys: import only B8 → CDS 6A88. */
static void test_sign_needs_sig_slot(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp); do_verify_pw3(cmd, rsp);
    uint8_t d_dec[32]; memset(d_dec, 0x22, 32);
    n = build_key_import_crt(0xB8, d_dec, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import B8 ok");
    do_verify_pw1_sign(cmd, rsp); do_disable_uif(cmd, rsp);
    uint8_t hash[32]; memset(hash, 0x5A, 32);
    n = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A88, "CDS without SIG key → 6A88");
}

/* Import with no recognizable CRT (e.g. only a bogus 0xB7) → 6A80. */
static void test_import_unknown_crt_rejected(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp); do_verify_pw3(cmd, rsp);
    uint8_t d[32]; memset(d, 0x44, 32);
    n = build_key_import_crt(0xB7, d, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80, "unknown CRT rejected");
}
```

Register the four tests with `TEST_RUN(...)` in `test_openpgp_card()`. Also update `test_import_rejects_zero_scalar` if it referenced `s_key` semantics (it goes through the APDU API — should be untouched).

- [ ] **Step 3: Run red.** `cd test && cmake -B build && cmake --build build && ./build/test_runner` — compilation fails on the hook signatures, then the new tests fail.

- [ ] **Step 4: Implement in `openpgp_card.c`.**

Key state:

```c
/* Private keys, one per OpenPGP slot.  d is big-endian as received from gpg.
 * origin per DO 0xDE encoding: 1 = generated on-device, 2 = imported. */
typedef struct {
    uint8_t set;
    uint8_t algo;     /* PGP_ALGO_ECDSA_P256 / PGP_ALGO_ECDH */
    uint8_t origin;
    uint8_t d[32];
} pgp_key_t;
static pgp_key_t s_keys[OPENPGP_SLOT_COUNT];
```

Helpers:

```c
/* CRT tag → slot index, or -1. */
static int slot_from_crt(uint8_t crt)
{
    switch (crt) {
    case 0xB6: return OPENPGP_SLOT_SIG;
    case 0xB8: return OPENPGP_SLOT_DEC;
    case 0xA4: return OPENPGP_SLOT_AUT;
    default:   return -1;
    }
}

/* Slot's algorithm from the live algo-attrs DO (C1/C2/C3 leading byte). */
static uint8_t slot_algo(int slot)
{
    static const uint16_t attr_tag[OPENPGP_SLOT_COUNT] = {0x00C1u, 0x00C2u, 0x00C3u};
    const uint8_t *v; uint16_t n;
    if (openpgp_do_get(attr_tag[slot], &v, &n) && n >= 1) return v[0];
    return PGP_ALGO_ECDSA_P256;
}
```

Import (0xDB) changes: find the CRT among B6/B8/A4 via `tlv_find` on `inner` (in that order; first match wins — gpg sends exactly one), no CRT → 6A80. The 7F48 template walk and 5F48 extraction stay; scalar rules:
- slot algo `PGP_ALGO_ECDSA_P256`: `elen` must be exactly 32 (unchanged Phase-1 rule);
- slot algo `PGP_ALGO_ECDH` (X25519): accept `1 <= elen <= 32`, **left-pad** with zeros into the 32-byte big-endian buffer (gpg's MPI encoding may strip leading zero bytes — clamped cv25519 scalars have MSB 0x40–0x7F so it shouldn't happen, but Gnuk pads defensively and so do we).
- zero-scalar reject stays, applied to the padded 32 bytes.
- store: `s_keys[slot] = {.set=1, .algo=slot_algo(slot), .origin=2, .d=...}`; persist; on persist failure scrub that slot and return 6F00.

READ PUBLIC KEY (0x47/81): `slot_from_crt(a.data[0])` instead of the B6-only check; key lookup in `s_keys[slot]`; response built by a shared builder (GENERATE reuses it in Task 5):

```c
/* Build the 7F49 public-key response for a populated slot.
 * P-256: 86 holds 65 B (04||X||Y).  X25519: 86 holds 0x40 || pub(32) —
 * the OpenPGP "native curve" point format gpg expects for cv25519. */
static uint16_t build_pubkey_response(int slot, uint8_t *out, uint16_t out_max)
{
    const pgp_key_t *k = &s_keys[slot];
    if (!s_hooks->pubkey) return sw_only(out, out_max, SW_REF_NOT_FOUND);

    uint8_t raw[65]; uint16_t raw_n = 0;
    if (!s_hooks->pubkey(k->algo, k->d, raw, &raw_n))
        return sw_only(out, out_max, SW_REF_NOT_FOUND);

    uint8_t point[66]; uint16_t point_n;
    if (k->algo == PGP_ALGO_ECDH) {            /* X25519 → 0x40-prefixed */
        if (raw_n != 32) return sw_only(out, out_max, 0x6F00u);
        point[0] = 0x40; memcpy(point + 1, raw, 32); point_n = 33;
    } else {
        if (raw_n != 65) return sw_only(out, out_max, 0x6F00u);
        memcpy(point, raw, 65); point_n = 65;
    }

    uint8_t inner[70];
    uint16_t ioff = tlv_append(inner, sizeof(inner), 0, 0x86u, point, point_n);
    if (ioff == 0) return sw_only(out, out_max, 0x6F00u);
    uint8_t body[80];
    uint16_t off = tlv_append(body, sizeof(body), 0, 0x7F49u, inner, ioff);
    if (off == 0) return sw_only(out, out_max, 0x6F00u);
    return respond(out, out_max, body, off, SW_OK);
}
```

PSO:CDS: `s_key` → `s_keys[OPENPGP_SLOT_SIG]` (and `openpgp_card_key_is_set()`, `factory_reset` clear all 3 slots).

NVS v2 (target section):

```c
/* NVS blob v2 — new key "pgp_keys" (the v1 "pgp_key" single-slot blob is
 * migrated on first load and left in place, harmless). */
static bool key_persist(void)
{
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "pgp_keys",
                                             s_keys, sizeof(s_keys),
                                             "pgp_keys_ver", 2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "key persist failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
```

`openpgp_card_load()` key section becomes:

```c
    /* v2 blob first; on a fresh-from-Phase-1 device fall back to the legacy
     * v1 single-slot blob, migrate it into the SIG slot, persist v2. */
    pgp_key_t loaded[OPENPGP_SLOT_COUNT];
    memset(loaded, 0, sizeof(loaded));
    ver = 0;
    if (nvs_load_blob_with_total(STORAGE_NAMESPACE, "pgp_keys",
                                 loaded, sizeof(loaded),
                                 "pgp_keys_ver", &ver) == ESP_OK) {
        memcpy(s_keys, loaded, sizeof(s_keys));
    } else {
        typedef struct { uint8_t set; uint8_t algo; uint8_t d[32]; } pgp_key_v1_t;
        pgp_key_v1_t v1; memset(&v1, 0, sizeof(v1));
        ver = 0;
        if (nvs_load_blob_with_total(STORAGE_NAMESPACE, "pgp_key",
                                     &v1, sizeof(v1), "pgp_key_ver", &ver) == ESP_OK
            && v1.set) {
            s_keys[OPENPGP_SLOT_SIG].set    = 1;
            s_keys[OPENPGP_SLOT_SIG].algo   = v1.algo;
            s_keys[OPENPGP_SLOT_SIG].origin = 2;      /* Phase 1 = import only */
            memcpy(s_keys[OPENPGP_SLOT_SIG].d, v1.d, 32);
            key_persist();                             /* write v2 */
            ESP_LOGI(TAG, "migrated Phase-1 SIG key to multi-slot blob");
        }
    }
```

- [ ] **Step 5: Run green.** All host tests pass (existing suite adapted + 4 new).

- [ ] **Step 6: Build check + commit.**

```bash
./scripts/check.sh --board kase_dongle
git add main/security/openpgp_card.c main/security/openpgp_card.h test/test_openpgp_card.c
git commit -m "feat(pgp): multi-slot key store (SIG/DEC/AUT) + algo-aware hooks, NVS v2 with v1 migration (Phase 2 Task 1)"
```

---

### Task 2: PSO:DECIPHER + UIF D7 + algo-attrs validation (TDD)

The decrypt path, applet-side, against the fake `ecdh` hook. Also: C2 factory value becomes cv25519, PUT DATA C1/C2/C3 only accept supported algorithms.

**Files:**
- Modify: `main/security/openpgp_card.c`
- Test: `test/test_openpgp_card.c`

- [ ] **Step 1: Failing tests.**

```c
/* Build PSO:DECIPHER 00 2A 80 86 Lc [ A6{ 7F49{ 86 <peer> } } ] */
static uint16_t build_pso_decipher(const uint8_t *peer, uint8_t peer_n, uint8_t *buf)
{
    uint16_t i = 0;
    buf[i++] = 0x00; buf[i++] = 0x2A; buf[i++] = 0x80; buf[i++] = 0x86;
    buf[i++] = (uint8_t)(peer_n + 7);                  /* Lc */
    buf[i++] = 0xA6; buf[i++] = (uint8_t)(peer_n + 5);
    buf[i++] = 0x7F; buf[i++] = 0x49; buf[i++] = (uint8_t)(peer_n + 2);
    buf[i++] = 0x86; buf[i++] = peer_n;
    memcpy(buf + i, peer, peer_n); i += peer_n;
    return i;
}

static void do_verify_pw1_user(uint8_t *cmd, uint8_t *rsp)   /* mode 0x82 */
{
    static const uint8_t pw1[] = {'1','2','3','4','5','6'};
    uint16_t clen = build_verify(0x82, pw1, sizeof(pw1), cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, 256);
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY PW1 (82) returns 9000");
}

/* Full happy path + gate checks for PSO:DECIPHER. */
static void test_decipher_gates_and_result(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp);

    uint8_t peer[32]; memset(peer, 0x99, 32);

    /* no key → 6A88 */
    n = build_pso_decipher(peer, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A88, "DECIPHER no key");

    do_verify_pw3(cmd, rsp);
    uint8_t d_dec[32]; memset(d_dec, 0x22, 32);
    n = build_key_import_crt(0xB8, d_dec, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import DEC key");

    /* PW1 mode 81 alone is NOT the decipher gate */
    do_verify_pw1_sign(cmd, rsp);
    n = build_pso_decipher(peer, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982, "DECIPHER needs mode 82");

    /* mode 82 + factory UIF D7 = off → succeeds, returns the canned shared */
    do_verify_pw1_user(cmd, rsp);
    n = build_pso_decipher(peer, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "DECIPHER ok");
    TEST_ASSERT_EQ(rlen, 32 + 2, "32-byte shared secret");
    TEST_ASSERT(rsp[0] == 0x55 && rsp[31] == 0x55, "canned ecdh output");
    TEST_ASSERT(g_fake_ecdh_last_d[0] == 0x22, "used the DEC slot scalar");
    TEST_ASSERT(g_fake_ecdh_last_peer[0] == 0x99, "peer point forwarded");

    /* mode 82 is NOT consumed: second DECIPHER also works */
    n = build_pso_decipher(peer, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "82 not consumed");
}

/* UIF D7 on → confirm() is consulted; denial → 6985, grant → 9000. */
static void test_decipher_uif_gate(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp); do_verify_pw3(cmd, rsp);
    uint8_t d_dec[32]; memset(d_dec, 0x22, 32);
    n = build_key_import_crt(0xB8, d_dec, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import DEC key");

    static const uint8_t uif_on[] = {0x01, 0x20};
    n = build_put_data(0x00, 0xD7, uif_on, 2, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "enable UIF D7");

    do_verify_pw1_user(cmd, rsp);
    uint8_t peer[32]; memset(peer, 0x99, 32);

    g_confirm_retval = 2;                      /* denied */
    n = build_pso_decipher(peer, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6985, "UIF denial → 6985");

    g_confirm_retval = 1;                      /* touch */
    n = build_pso_decipher(peer, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "UIF grant → ok");
}

/* 0x40-prefixed 33-byte peer accepted; malformed frames rejected. */
static void test_decipher_input_formats(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp); do_verify_pw3(cmd, rsp);
    uint8_t d_dec[32]; memset(d_dec, 0x22, 32);
    n = build_key_import_crt(0xB8, d_dec, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import DEC key");
    do_verify_pw1_user(cmd, rsp);

    uint8_t peer40[33]; peer40[0] = 0x40; memset(peer40 + 1, 0x99, 32);
    n = build_pso_decipher(peer40, 33, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "0x40-prefixed peer accepted");
    TEST_ASSERT(g_fake_ecdh_last_peer[0] == 0x99, "prefix stripped");

    uint8_t bad[16]; memset(bad, 0x99, 16);    /* wrong point length */
    n = build_pso_decipher(bad, 16, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80, "bad point length → 6A80");

    /* garbage instead of A6 */
    uint8_t raw[40]; memset(raw, 0xEE, sizeof(raw));
    uint16_t i = 0;
    cmd[i++] = 0x00; cmd[i++] = 0x2A; cmd[i++] = 0x80; cmd[i++] = 0x86;
    cmd[i++] = 40; memcpy(cmd + i, raw, 40); i += 40;
    rlen = openpgp_card_apdu(cmd, i, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80, "no A6 container → 6A80");
}

/* C1/C3 only accept ECDSA-P256; C2 only accepts ECDH-cv25519. */
static void test_algo_attrs_validation(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp); do_verify_pw3(cmd, rsp);

    static const uint8_t p256_ecdsa[9]  = {0x13,0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
    static const uint8_t cv25519[11]    = {0x12,0x2B,0x06,0x01,0x04,0x01,0x97,0x55,0x01,0x05,0x01};
    static const uint8_t ed25519_bad[10]= {0x16,0x2B,0x06,0x01,0x04,0x01,0xDA,0x47,0x0F,0x01};

    n = build_put_data(0x00, 0xC1, p256_ecdsa, 9, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "C1 = ECDSA P-256 accepted");

    n = build_put_data(0x00, 0xC2, cv25519, 11, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "C2 = cv25519 accepted");

    n = build_put_data(0x00, 0xC1, ed25519_bad, 10, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80, "C1 = EdDSA rejected");

    n = build_put_data(0x00, 0xC2, p256_ecdsa, 9, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80, "C2 = ECDH P-256 rejected (X25519 only)");
}

/* Factory C2 is cv25519 and a stale Phase-1 P-256-ECDH C2 gets migrated. */
static void test_factory_c2_is_cv25519(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[64], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp);
    n = build_get_data(0x00, 0xC2, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET C2 ok");
    TEST_ASSERT_EQ(rlen, 11 + 2, "C2 is 11 bytes");
    TEST_ASSERT(rsp[0] == 0x12 && rsp[1] == 0x2B && rsp[10] == 0x01, "cv25519 OID");

    /* simulate a Phase-1 card: plant the legacy value, re-run defaults */
    static const uint8_t legacy[9] = {0x12,0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
    openpgp_do_put(0x00C2u, legacy, 9);
    openpgp_card_ensure_defaults();
    n = build_get_data(0x00, 0xC2, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(rlen, 11 + 2, "legacy C2 migrated to cv25519");
}
```

Register all five with `TEST_RUN(...)`.

- [ ] **Step 2: Run red.**

- [ ] **Step 3: Implement.**

Factory C2 (replaces the 9-byte P-256-ECDH value):

```c
/* Algorithm attributes 0xC2 — ECDH with cv25519 (OID 1.3.6.1.4.1.3029.1.5.1),
 * gpg's default ENC subkey algorithm since 2.3. */
static const uint8_t FACTORY_C2[11] = {
    0x12, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x97, 0x55, 0x01, 0x05, 0x01
};
```

In `openpgp_card_ensure_defaults()` change the C2 line to `ENSURE(0x00C2, FACTORY_C2, 11);` and append after the `#undef ENSURE`:

```c
    /* Phase-1 cards shipped C2 = ECDH P-256; Phase 2 decrypts with X25519
     * only.  Upgrade the stale factory value in place — safe: PSO:DECIPHER
     * did not exist in Phase 1, so no key material depends on the old attrs. */
    static const uint8_t LEGACY_C2_P256[9] = {0x12,0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
    if (openpgp_do_get(0x00C2u, &v, &n) && n == 9 && memcmp(v, LEGACY_C2_P256, 9) == 0)
        if (!openpgp_do_put(0x00C2u, FACTORY_C2, sizeof(FACTORY_C2))) failures++;
```

Algo-attrs validation, called from the generic PUT DATA path before `openpgp_do_put`:

```c
/* Only the algorithms this card implements may be written to C1/C2/C3 —
 * accepting anything else would let gpg build keys the card cannot use. */
static bool algo_attrs_acceptable(uint16_t tag, const uint8_t *v, uint16_t n)
{
    static const uint8_t P256_ECDSA[9] = {0x13,0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
    if (tag == 0x00C1u || tag == 0x00C3u)
        return n == sizeof(P256_ECDSA) && memcmp(v, P256_ECDSA, n) == 0;
    if (tag == 0x00C2u)
        return n == sizeof(FACTORY_C2) && memcmp(v, FACTORY_C2, n) == 0;
    return true;
}
```

In the INS 0xDA handler, before the generic `openpgp_do_put`:

```c
        if ((tag == 0x00C1u || tag == 0x00C2u || tag == 0x00C3u) &&
            !algo_attrs_acceptable(tag, a.data, a.lc))
            return sw_only(out, out_max, SW_WRONG_DATA);
```

Generalize the UIF check (replaces `uif_signing_required`):

```c
/* UIF DO for a slot (0x00D6 sig / 0x00D7 dec / 0x00D8 aut): byte0 != 0 → touch. */
static bool uif_required(uint16_t uif_tag)
{
    const uint8_t *v; uint16_t n;
    return openpgp_do_get(uif_tag, &v, &n) && n >= 1 && v[0] != 0x00;
}
```

PSO:DECIPHER — inside the existing `a.ins == 0x2A` block, before the CDS P1P2 check:

```c
        /* ---- PSO: DECIPHER (P1=0x80, P2=0x86) — X25519 ECDH ---- */
        if (a.p1 == 0x80 && a.p2 == 0x86) {
            if (!s_crypto_ok) return sw_only(out, out_max, 0x6581u);

            const pgp_key_t *k = &s_keys[OPENPGP_SLOT_DEC];
            if (!k->set) return sw_only(out, out_max, SW_REF_NOT_FOUND);
            if (k->algo != PGP_ALGO_ECDH)
                return sw_only(out, out_max, SW_COND_NOT_SAT);
            if (!s_pw1_user_verified)
                return sw_only(out, out_max, SW_SEC_NOT_SAT);

            /* data = A6{ 7F49{ 86 <ephemeral public point> } } */
            uint16_t a6_n; const uint8_t *a6 = tlv_find(a.data, a.lc, 0x00A6u, &a6_n);
            if (!a6) return sw_only(out, out_max, SW_WRONG_DATA);
            uint16_t t_n; const uint8_t *t = tlv_find(a6, a6_n, 0x7F49u, &t_n);
            if (!t) return sw_only(out, out_max, SW_WRONG_DATA);
            uint16_t p_n; const uint8_t *p = tlv_find(t, t_n, 0x0086u, &p_n);
            if (!p) return sw_only(out, out_max, SW_WRONG_DATA);
            /* X25519 u-coordinate: 32 B; tolerate the 0x40 native-format prefix. */
            if (p_n == 33 && p[0] == 0x40) { p++; p_n = 32; }
            if (p_n != 32) return sw_only(out, out_max, SW_WRONG_DATA);

            /* UIF D7 gate (after input validation: don't burn a touch on garbage). */
            if (uif_required(0x00D7u)) {
                int cs = s_hooks->confirm();
                if (cs != 1) return sw_only(out, out_max, SW_COND_NOT_SAT);
            }
            /* NOTE: mode-82 verification is NOT consumed — the C4 validity
             * byte applies to PSO:CDS only (OpenPGP 3.4 §7.2.2). */

            uint8_t shared[64]; uint16_t shared_n = 0;
            if (!s_hooks->ecdh ||
                !s_hooks->ecdh(k->d, p, p_n, shared, &shared_n))
                return sw_only(out, out_max, SW_COND_NOT_SAT);
            return respond(out, out_max, shared, shared_n, SW_OK);
        }
```

PSO:CDS keeps its `uif_required(0x00D6u)` call (renamed helper).

- [ ] **Step 4: Run green.**

- [ ] **Step 5: Commit.**

```bash
git add main/security/openpgp_card.c test/test_openpgp_card.c
git commit -m "feat(pgp): PSO:DECIPHER (X25519, UIF D7, PW1-82 gate) + C1-C3 algo-attrs validation, factory C2 = cv25519"
```

---

### Task 3: X25519 target crypto + hooks wiring + HW checkpoint `gpg --decrypt`

**Files:**
- Modify: `main/security/openpgp_crypto.{c,h}`
- Modify: `main/security/ccid.c`

- [ ] **Step 1: Crypto API.** Append to `openpgp_crypto.h`:

```c
/* X25519 (RFC 7748).  Scalar convention: big-endian, as received from gpg's
 * MPI encoding (the implementation reverses + clamps internally). */

/* Public key: out_le = X25519(d, basepoint 9), 32 B little-endian u. */
bool openpgp_crypto_x25519_pubkey(const uint8_t d_be[32], uint8_t out_le[32]);

/* Shared secret: out_le = X25519(d, peer), peer = 32 B LE u-coordinate. */
bool openpgp_crypto_x25519_ecdh(const uint8_t d_be[32],
                                const uint8_t peer_le[32],
                                uint8_t out_le[32]);

/* Generate a private scalar for `algo` (PGP_ALGO_ECDSA_P256 / PGP_ALGO_ECDH).
 * Output big-endian (matches the import/store convention). */
bool openpgp_crypto_genkey(uint8_t algo, uint8_t d_out[32]);
```

- [ ] **Step 2: Implementation in `openpgp_crypto.c`.** Add `#include "mbedtls/ecdh.h"` and `#include "openpgp_card.h"` (for the `PGP_ALGO_*` ids):

```c
static void be32_to_le(const uint8_t in[32], uint8_t out[32])
{
    for (int i = 0; i < 32; i++) out[i] = in[31 - i];
}

/* Load the X25519 scalar: BE→LE, RFC 7748 clamp (gpg sends pre-clamped
 * scalars; re-clamping is idempotent and protects against bad imports),
 * then read little-endian into the MPI. Scrubs the stack copy. */
static int x25519_load_scalar(mbedtls_mpi *dd, const uint8_t d_be[32])
{
    uint8_t d_le[32];
    be32_to_le(d_be, d_le);
    d_le[0]  &= 0xF8;
    d_le[31] = (uint8_t)((d_le[31] & 0x7F) | 0x40);
    int rc = mbedtls_mpi_read_binary_le(dd, d_le, 32);
    memset(d_le, 0, sizeof(d_le));
    return rc;
}

bool openpgp_crypto_x25519_ecdh(const uint8_t d_be[32],
                                const uint8_t peer_le[32],
                                uint8_t out_le[32])
{
    if (!d_be || !peer_le || !out_le) return false;

    mbedtls_ecp_group grp; mbedtls_ecp_point Qp; mbedtls_mpi dd, z;
    mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Qp);
    mbedtls_mpi_init(&dd); mbedtls_mpi_init(&z);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519)) break;
        if (x25519_load_scalar(&dd, d_be)) break;
        /* Montgomery point read: 32-byte little-endian u-coordinate. */
        if (mbedtls_ecp_point_read_binary(&grp, &Qp, peer_le, 32)) break;
        if (mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &dd, rng_cb, NULL)) break;
        if (mbedtls_mpi_write_binary_le(&z, out_le, 32)) break;
        ok = true;
    } while (0);

    mbedtls_mpi_lset(&dd, 0);  /* scrub the scalar copy before free */
    mbedtls_mpi_lset(&z, 0);   /* scrub the shared secret copy */
    mbedtls_mpi_free(&dd); mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&Qp); mbedtls_ecp_group_free(&grp);
    return ok;
}

bool openpgp_crypto_x25519_pubkey(const uint8_t d_be[32], uint8_t out_le[32])
{
    if (!d_be || !out_le) return false;

    mbedtls_ecp_group grp; mbedtls_ecp_point Q; mbedtls_mpi dd;
    mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&dd);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519)) break;
        if (x25519_load_scalar(&dd, d_be)) break;
        if (mbedtls_ecp_mul(&grp, &Q, &dd, &grp.G, rng_cb, NULL)) break;
        size_t olen = 0;
        if (mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                           &olen, out_le, 32)) break;
        if (olen != 32) break;
        ok = true;
    } while (0);

    mbedtls_mpi_lset(&dd, 0);
    mbedtls_mpi_free(&dd);
    mbedtls_ecp_point_free(&Q); mbedtls_ecp_group_free(&grp);
    return ok;
}

bool openpgp_crypto_genkey(uint8_t algo, uint8_t d_out[32])
{
    if (!d_out) return false;
    mbedtls_ecp_group_id gid;
    if      (algo == PGP_ALGO_ECDSA_P256) gid = MBEDTLS_ECP_DP_SECP256R1;
    else if (algo == PGP_ALGO_ECDH)       gid = MBEDTLS_ECP_DP_CURVE25519;
    else return false;

    mbedtls_ecp_group grp; mbedtls_mpi dd;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&dd);
    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, gid)) break;
        if (mbedtls_ecp_gen_privkey(&grp, &dd, rng_cb, NULL)) break;
        if (mbedtls_mpi_write_binary(&dd, d_out, 32)) break;  /* BE store */
        ok = true;
    } while (0);
    mbedtls_mpi_lset(&dd, 0);
    mbedtls_mpi_free(&dd); mbedtls_ecp_group_free(&grp);
    return ok;
}
```

- [ ] **Step 3: Extend the boot selftest** (same function, after the pubkey KAT) with the RFC 7748 §6.1 Diffie-Hellman vectors. Inputs reversed to BE because our API takes gpg's byte order:

```c
    /* KAT 3: X25519 — RFC 7748 §6.1.  Alice priv (REVERSED to BE for our
     * API), Bob pub (LE wire format), expected shared K (LE). */
    static const uint8_t alice_d_be[32] = {   /* reverse of 77076d0a...2c2a */
        0x2A,0x2C,0xB9,0x1D,0xA5,0xFB,0x77,0xB1,0x2A,0x99,0xC0,0xEB,0x87,0x2F,0x4C,0xDF,
        0x45,0x66,0xB2,0x51,0x72,0xC1,0x16,0x3C,0x7D,0xA5,0x18,0x73,0x0A,0x6D,0x07,0x77,
    };
    static const uint8_t alice_pub_le[32] = {
        0x85,0x20,0xF0,0x09,0x89,0x30,0xA7,0x54,0x74,0x8B,0x7D,0xDC,0xB4,0x3E,0xF7,0x5A,
        0x0D,0xBF,0x3A,0x0D,0x26,0x38,0x1A,0xF4,0xEB,0xA4,0xA9,0x8E,0xAA,0x9B,0x4E,0x6A,
    };
    static const uint8_t bob_pub_le[32] = {
        0xDE,0x9E,0xDB,0x7D,0x7B,0x7D,0xC1,0xB4,0xD3,0x5B,0x61,0xC2,0xEC,0xE4,0x35,0x37,
        0x3F,0x83,0x43,0xC8,0x5B,0x78,0x67,0x4D,0xAD,0xFC,0x7E,0x14,0x6F,0x88,0x2B,0x4F,
    };
    static const uint8_t shared_k_le[32] = {
        0x4A,0x5D,0x9D,0x5B,0xA4,0xCE,0x2D,0xE1,0x72,0x8E,0x3B,0xF4,0x80,0x35,0x0F,0x25,
        0xE0,0x7E,0x21,0xC9,0x47,0xD1,0x9E,0x33,0x76,0xF0,0x9B,0x3C,0x1E,0x16,0x17,0x42,
    };
    uint8_t xpub[32], xshared[32];
    if (!openpgp_crypto_x25519_pubkey(alice_d_be, xpub) ||
        memcmp(xpub, alice_pub_le, 32) != 0) {
        ESP_LOGE(TAG, "selftest: X25519 pubkey KAT failed");
        return false;
    }
    if (!openpgp_crypto_x25519_ecdh(alice_d_be, bob_pub_le, xshared) ||
        memcmp(xshared, shared_k_le, 32) != 0) {
        ESP_LOGE(TAG, "selftest: X25519 ECDH KAT failed");
        return false;
    }
```

(The single `s_crypto_ok` flag now covers all algorithms — a failed X25519 KAT blocks sign too, deliberately: a broken crypto stack is a broken card.)

- [ ] **Step 4: Wire hooks v2 in `ccid.c`.** Replace `dongle_pubkey` and add the two new hooks:

```c
static bool dongle_pubkey(uint8_t algo, const uint8_t d[32],
                          uint8_t *out, uint16_t *out_n)
{
    if (algo == PGP_ALGO_ECDH) {
        if (!openpgp_crypto_x25519_pubkey(d, out)) return false;
        *out_n = 32;
        return true;
    }
    if (!openpgp_crypto_p256_pubkey(d, out)) return false;
    *out_n = 65;
    return true;
}

static bool dongle_ecdh(const uint8_t d[32], const uint8_t *peer, uint16_t peer_n,
                        uint8_t *out, uint16_t *out_n)
{
    if (peer_n != 32) return false;
    if (!openpgp_crypto_x25519_ecdh(d, peer, out)) return false;
    *out_n = 32;
    return true;
}

static bool dongle_genkey(uint8_t algo, uint8_t d_out[32])
{
    return openpgp_crypto_genkey(algo, d_out);
}

static const openpgp_card_hooks_t s_dongle_hooks = {
    .sign    = dongle_sign,
    .confirm = dongle_confirm,
    .pubkey  = dongle_pubkey,
    .ecdh    = dongle_ecdh,
    .genkey  = dongle_genkey,   /* used from Task 5; harmless before */
};
```

- [ ] **Step 5: Build + flash + migration check.**

```bash
./scripts/check.sh --board kase_dongle
idf.py -B build_kase_dongle -p /dev/ttyUSB0 flash
```

With temp console enabled, boot log must show `selftest: PASS` (all 3 KATs) and `migrated Phase-1 SIG key to multi-slot blob` (once). Then:

```bash
export GNUPGHOME=$HOME/kase-pgp-test
nix-shell -p gnupg --run 'gpgconf --kill scdaemon; gpg --card-status'
```

Expected: card shows as before, **Signature key fingerprint intact** (migration worked), Key attributes now `nistp256 cv25519 nistp256`, and `gpg --clearsign` still works with touch (Phase-1 regression check).

- [ ] **Step 6: HW checkpoint — decrypt end-to-end.**

```bash
nix-shell -p gnupg --run '
  export GNUPGHOME=$HOME/kase-pgp-test
  FPR=$(gpg --list-secret-keys --with-colons | awk -F: "/^fpr/{print \$10; exit}")
  gpg --quick-add-key $FPR cv25519 encr        # cv25519 ENC subkey on host
  gpg --edit-key $FPR                          # → key 1 → keytocard → (2) Encryption → PW3
  echo "secret payload" | gpg -r kase-test -e > /tmp/kase.gpg
  gpgconf --kill gpg-agent
  gpg -d /tmp/kase.gpg                         # PW1 = 123456
'
```

Expected: `gpg -d` prints `secret payload`; scdaemon log shows `PSO:DECIPHER` → `9000`. Then enable UIF for decryption (`gpg --card-edit` → `admin` → `uif 2 on`) and decrypt again: gpg shows the touch prompt, no touch → fails after 15 s (6985), with `K_SEC_CONFIRM` → decrypts.
Debug path if the shared secret is wrong (decrypt error, not 6985): endianness of the scalar — dump `scd.log` APDUs, compare the card's 7F49/86 against `gpg --list-keys --with-colons` cv25519 public; the BE/LE reversal in `x25519_load_scalar` is the prime suspect. Fix + add the failing case to the KAT.

- [ ] **Step 7: Commit.**

```bash
git add main/security/openpgp_crypto.c main/security/openpgp_crypto.h main/security/ccid.c
git commit -m "feat(pgp): X25519 ECDH + genkey (mbedtls CURVE25519, RFC7748 KATs in boot selftest), hooks v2 wired — gpg --decrypt validated on HW"
```

---

### Task 4: INTERNAL AUTHENTICATE (TDD) + HW checkpoint SSH

**Files:**
- Modify: `main/security/openpgp_card.c`
- Test: `test/test_openpgp_card.c`

- [ ] **Step 1: Failing tests.**

```c
static uint16_t build_internal_auth(const uint8_t *data, uint8_t n, uint8_t *buf)
{
    uint16_t i = 0;
    buf[i++] = 0x00; buf[i++] = 0x88; buf[i++] = 0x00; buf[i++] = 0x00;
    buf[i++] = n; memcpy(buf + i, data, n); i += n;
    return i;
}

/* Gates + happy path: AUT slot key, PW1 mode 82, UIF D8. */
static void test_internal_auth(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp);
    uint8_t hash[32]; memset(hash, 0x5A, 32);

    /* no key → 6A88 */
    n = build_internal_auth(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A88, "no AUT key");

    do_verify_pw3(cmd, rsp);
    uint8_t d_aut[32]; memset(d_aut, 0x33, 32);
    n = build_key_import_crt(0xA4, d_aut, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import AUT key");

    /* unverified PW1-82 → 6982 */
    n = build_internal_auth(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982, "AUTH needs mode 82");

    /* mode 82 + factory UIF D8 = off → signs with the AUT scalar */
    do_verify_pw1_user(cmd, rsp);
    n = build_internal_auth(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "AUTH ok");
    TEST_ASSERT_EQ(rlen, 64 + 2, "r||s signature");
    TEST_ASSERT(g_fake_sign_last_d[0] == 0x33, "signed with AUT slot key");

    /* NOT consumed (unlike CDS): a second AUTH still works */
    n = build_internal_auth(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "82 not consumed by AUTH");

    /* wrong P1P2 / oversized data */
    cmd[2] = 0x01;
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A86, "P1 must be 00");
}

/* UIF D8 on → confirm consulted. */
static void test_internal_auth_uif(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp); do_verify_pw3(cmd, rsp);
    uint8_t d_aut[32]; memset(d_aut, 0x33, 32);
    n = build_key_import_crt(0xA4, d_aut, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "import AUT key");
    static const uint8_t uif_on[] = {0x01, 0x20};
    n = build_put_data(0x00, 0xD8, uif_on, 2, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "enable UIF D8");
    do_verify_pw1_user(cmd, rsp);
    uint8_t hash[32]; memset(hash, 0x5A, 32);

    g_confirm_retval = 2;
    n = build_internal_auth(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6985, "UIF denial → 6985");

    g_confirm_retval = 1;
    n = build_internal_auth(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "UIF grant → ok");
}
```

Register both.

- [ ] **Step 2: Run red.**

- [ ] **Step 3: Implement.** New dispatch block after PSO (INS 0x2A):

```c
    /* ---- INTERNAL AUTHENTICATE (INS=0x88) — SSH via gpg-agent ---- */
    if (a.ins == 0x88) {
        if (a.p1 != 0x00 || a.p2 != 0x00)
            return sw_only(out, out_max, SW_WRONG_P1P2);
        if (!s_crypto_ok) return sw_only(out, out_max, 0x6581u);

        const pgp_key_t *k = &s_keys[OPENPGP_SLOT_AUT];
        if (!k->set) return sw_only(out, out_max, SW_REF_NOT_FOUND);
        if (k->algo != PGP_ALGO_ECDSA_P256)
            return sw_only(out, out_max, SW_COND_NOT_SAT);
        if (!s_pw1_user_verified)
            return sw_only(out, out_max, SW_SEC_NOT_SAT);
        if (a.lc == 0 || a.lc > 64 || !a.data)
            return sw_only(out, out_max, SW_WRONG_DATA);

        if (uif_required(0x00D8u)) {
            int cs = s_hooks->confirm();
            if (cs != 1) return sw_only(out, out_max, SW_COND_NOT_SAT);
        }
        /* mode-82 is NOT consumed and the DS counter does NOT move —
         * both are PSO:CDS-only semantics (OpenPGP 3.4 §7.2.10/§7.2.13). */

        uint8_t sig[256]; uint16_t sig_len = 0;
        if (!s_hooks->sign(k->d, a.data, a.lc, sig, &sig_len))
            return sw_only(out, out_max, SW_COND_NOT_SAT);
        return respond(out, out_max, sig, sig_len, SW_OK);
    }
```

- [ ] **Step 4: Run green.**

- [ ] **Step 5: HW checkpoint — SSH end-to-end.**

```bash
./scripts/check.sh --board kase_dongle && idf.py -B build_kase_dongle -p /dev/ttyUSB0 flash
nix-shell -p gnupg --run '
  export GNUPGHOME=$HOME/kase-pgp-test
  FPR=$(gpg --list-secret-keys --with-colons | awk -F: "/^fpr/{print \$10; exit}")
  gpg --quick-add-key $FPR nistp256 auth       # P-256 AUTH subkey on host
  gpg --edit-key $FPR                          # → key N → keytocard → (3) Authentication → PW3
  echo enable-ssh-support >> $GNUPGHOME/gpg-agent.conf
  gpgconf --kill gpg-agent
  export SSH_AUTH_SOCK=$(gpgconf --list-dirs agent-ssh-socket)
  ssh-add -L                                   # must print ecdsa-sha2-nistp256 ... cardno:FF00...
'
```

Then a real login: add the printed `ecdsa-sha2-nistp256` line to GitLab (test SSH key) and `ssh -T git@gitlab.com` → greeting, PW1 prompt on first use. Enable `uif 3 on` via `gpg --card-edit` admin and re-`ssh` → touch required.
Known trap: gpg-agent caches the PW1; `gpgconf --kill gpg-agent` to re-test gates. If `ssh-add -L` is empty: card must be "active" — run `gpg --card-status` once in the same session, and check `scd.log` for the 0x88 exchange.

- [ ] **Step 6: Commit.**

```bash
git add main/security/openpgp_card.c test/test_openpgp_card.c
git commit -m "feat(pgp): INTERNAL AUTHENTICATE (P-256, PW1-82, UIF D8) — SSH via gpg-agent validated on HW"
```

---

### Task 5: GENERATE ASYMMETRIC KEY PAIR — on-device keygen (TDD + HW)

`0x47 P1=0x80` (today → 6D00). Keys born on the dongle, never on the host. gpg sets timestamps/fingerprints afterward via the existing PUT DATA paths.

**Files:**
- Modify: `main/security/openpgp_card.c`
- Test: `test/test_openpgp_card.c`

- [ ] **Step 1: Failing tests.**

```c
/* 0x47 P1=0x80: PW3-gated, routes CRT→slot, uses the slot's algo attrs,
 * persists, returns the 7F49 pubkey template. */
static void test_generate_keypair(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[64], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp);

    /* PW3 gate */
    n = build_read_pubkey(0x80, 0xB6, cmd);   /* same APDU shape, P1=0x80 */
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982, "GENERATE needs PW3");

    do_verify_pw3(cmd, rsp);

    /* SIG slot: C1 = ECDSA P-256 → genkey(0x13), response holds 65-B point */
    n = build_read_pubkey(0x80, 0xB6, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GENERATE B6 ok");
    TEST_ASSERT_EQ(g_fake_genkey_last_algo, PGP_ALGO_ECDSA_P256, "SIG algo from C1");
    TEST_ASSERT(rsp[0] == 0x7F && rsp[1] == 0x49, "7F49 response");
    int i86 = find_index(rsp, rlen, (const uint8_t[]){0x86, 0x41, 0x04}, 3);
    TEST_ASSERT(i86 >= 0, "uncompressed P-256 point");

    /* DEC slot: C2 = cv25519 → genkey(0x12), 0x40-prefixed 33-B point */
    n = build_read_pubkey(0x80, 0xB8, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GENERATE B8 ok");
    TEST_ASSERT_EQ(g_fake_genkey_last_algo, PGP_ALGO_ECDH, "DEC algo from C2");
    i86 = find_index(rsp, rlen, (const uint8_t[]){0x86, 0x21, 0x40}, 3);
    TEST_ASSERT(i86 >= 0, "0x40-prefixed X25519 point");

    /* generated SIG key signs (genkey fake fills d with 0xA0+0x13 = 0xB3) */
    do_verify_pw1_sign(cmd, rsp); do_disable_uif(cmd, rsp);
    uint8_t hash[32]; memset(hash, 0x5A, 32);
    n = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "sign with generated key");
    TEST_ASSERT(g_fake_sign_last_d[0] == (uint8_t)(0xA0 + PGP_ALGO_ECDSA_P256),
                "generated scalar in use");
}

/* DS counter resets when a new SIG key is generated or imported. */
static void test_ds_counter_resets_on_new_sig_key(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp); do_verify_pw3(cmd, rsp);
    uint8_t d[32]; memset(d, 0x11, 32);
    do_import_key(d, cmd, rsp);
    do_verify_pw1_sign(cmd, rsp); do_disable_uif(cmd, rsp);
    uint8_t hash[32]; memset(hash, 0x5A, 32);
    n = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "one signature made");

    /* counter is now 1; generating a fresh SIG key must zero it */
    n = build_read_pubkey(0x80, 0xB6, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GENERATE B6 ok");
    n = build_get_data(0x00, 0x93, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET 93 ok");
    TEST_ASSERT(rsp[0] == 0 && rsp[1] == 0 && rsp[2] == 0, "DS counter reset");
}
```

Register both.

- [ ] **Step 2: Run red.**

- [ ] **Step 3: Implement.** In the INS 0x47 block, replace the P1=0x80 fall-through. Factor a DS-counter reset helper (also called from the 0xDB import path when the target slot is SIG — that is part of this step):

```c
/* Reset the signature counter (DO 0x93) — required when a new SIG key is
 * generated or imported (OpenPGP 3.4 §7.2.14). */
static void ds_counter_reset(void)
{
    static const uint8_t zero[3] = {0, 0, 0};
    openpgp_do_put(0x0093u, zero, 3);
}
```

```c
        if (a.p1 == 0x80) {   /* GENERATE on-device */
            if (a.lc == 0 || !a.data)
                return sw_only(out, out_max, SW_WRONG_DATA);
            int slot = slot_from_crt(a.data[0]);
            if (slot < 0) return sw_only(out, out_max, SW_REF_NOT_FOUND);
            if (!s_crypto_ok) return sw_only(out, out_max, 0x6581u);
            if (!s_pw3_verified)
                return sw_only(out, out_max, SW_SEC_NOT_SAT);
            if (!s_hooks->genkey)
                return sw_only(out, out_max, SW_COND_NOT_SAT);

            uint8_t algo = slot_algo(slot);
            uint8_t d[32];
            if (!s_hooks->genkey(algo, d))
                return sw_only(out, out_max, SW_COND_NOT_SAT);

            s_keys[slot].set    = 1;
            s_keys[slot].algo   = algo;
            s_keys[slot].origin = 1;            /* generated on-device */
            memcpy(s_keys[slot].d, d, 32);
            memset(d, 0, sizeof(d));
            if (!key_persist()) {
                memset(&s_keys[slot], 0, sizeof(s_keys[slot]));
                return sw_only(out, out_max, 0x6F00u);
            }
            if (slot == OPENPGP_SLOT_SIG) ds_counter_reset();
            return build_pubkey_response(slot, out, out_max);
        }
        if (a.p1 == 0x81) {   /* READ existing public key (Task 1 version) */
            ...
        }
```

In the 0xDB import path, after a successful persist: `if (slot == OPENPGP_SLOT_SIG) ds_counter_reset();`.

- [ ] **Step 4: Run green.**

- [ ] **Step 5: HW checkpoint — full on-card generate.** Keygen timing note: P-256 `gen_privkey` + the pubkey derivation run well under scdaemon's BWT budget — no WTX needed (the WTX machinery only arms for UIF waits). If the worker stack high-watermark log shows < 1 KB headroom after keygen, bump the task stack in `ccid_init` from 6144 to 8192.

```bash
./scripts/check.sh --board kase_dongle && idf.py -B build_kase_dongle -p /dev/ttyUSB0 flash
nix-shell -p gnupg --run '
  export GNUPGHOME=$HOME/kase-pgp-test
  gpgconf --kill scdaemon
  gpg --card-edit     # → admin → generate
'
```

In the generate dialog: no off-card backup, validity 0, name/email test values, PW3 + PW1 when prompted. Expected: gpg drives GENERATE on B6/B8/A4 (watch scd.log: three 0x47/80 → 9000 + 7F49), then PUTs fingerprints/timestamps, and a complete new key appears in `gpg -K` with `sec>`/`ssb>` (all card-backed). Then smoke all three ops: `gpg --clearsign` (touch), `gpg -e`/`-d` roundtrip, `ssh-add -L`.

- [ ] **Step 6: Commit.**

```bash
git add main/security/openpgp_card.c test/test_openpgp_card.c
git commit -m "feat(pgp): on-device GENERATE ASYMMETRIC KEY PAIR (0x47/80) per slot + DS counter reset — keys born on the dongle"
```

---

### Task 6: DO polish — PW1 validity (C4 put), key-info DO 0xDE, TERMINATE/ACTIVATE (TDD)

**Files:**
- Modify: `main/security/openpgp_card.c`
- Test: `test/test_openpgp_card.c`

- [ ] **Step 1: Failing tests.**

```c
/* PUT DATA C4: byte0=1 → PW1-81 valid for several signatures (forcesig off). */
static void test_pw1_validity_put(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp); do_verify_pw3(cmd, rsp);
    uint8_t d[32]; memset(d, 0x11, 32);
    do_import_key(d, cmd, rsp);
    do_disable_uif(cmd, rsp);
    uint8_t hash[32]; memset(hash, 0x5A, 32);

    /* default (0): second CDS without re-VERIFY fails */
    do_verify_pw1_sign(cmd, rsp);
    n = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "first CDS ok");
    n = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982, "second CDS consumed");

    /* PUT C4 = 01 → not consumed anymore */
    static const uint8_t multi[1] = {0x01};
    n = build_put_data(0x00, 0xC4, multi, 1, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PUT C4 ok");
    do_verify_pw1_sign(cmd, rsp);
    n = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "CDS 1");
    n = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "CDS 2 — validity=1 keeps the token");

    /* GET C4 reflects it (synthesized byte0) */
    n = build_get_data(0x00, 0xC4, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET C4 ok");
    TEST_ASSERT_EQ(rsp[0], 0x01, "validity byte served");

    /* invalid value rejected */
    static const uint8_t bad[1] = {0x07};
    n = build_put_data(0x00, 0xC4, bad, 1, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80, "C4 byte must be 0/1");
}

/* DO 0xDE: per-slot presence/origin — 01 xx 02 xx 03 xx. */
static void test_key_information_do(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp);
    n = build_get_data(0x00, 0xDE, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DE ok");
    TEST_ASSERT_EQ(rlen, 6 + 2, "3 pairs");
    static const uint8_t empty[6] = {0x01,0x00, 0x02,0x00, 0x03,0x00};
    TEST_ASSERT(memcmp(rsp, empty, 6) == 0, "all slots empty");

    do_verify_pw3(cmd, rsp);
    uint8_t d[32]; memset(d, 0x11, 32);
    do_import_key(d, cmd, rsp);                       /* SIG ← imported (2) */
    n = build_read_pubkey(0x80, 0xB8, cmd);           /* DEC ← generated (1) */
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GENERATE B8 ok");

    n = build_get_data(0x00, 0xDE, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    static const uint8_t mixed[6] = {0x01,0x02, 0x02,0x01, 0x03,0x00};
    TEST_ASSERT(memcmp(rsp, mixed, 6) == 0, "imported=2 / generated=1 / empty=0");
}

/* TERMINATE DF (0xE6) + ACTIVATE FILE (0x44): gpg factory-reset sequence. */
static void test_terminate_activate(void)
{
    openpgp_card_hooks_t h; setup_card(&h);
    uint8_t cmd[128], rsp[300]; uint16_t n, rlen;
    do_select(cmd, rsp);

    /* without PW3 and with live retry counters → 6982 */
    cmd[0]=0x00; cmd[1]=0xE6; cmd[2]=0x00; cmd[3]=0x00;
    rlen = openpgp_card_apdu(cmd, 4, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982, "TERMINATE needs PW3 (or blocked PINs)");

    do_verify_pw3(cmd, rsp);
    uint8_t d[32]; memset(d, 0x11, 32);
    do_import_key(d, cmd, rsp);

    cmd[0]=0x00; cmd[1]=0xE6; cmd[2]=0x00; cmd[3]=0x00;
    rlen = openpgp_card_apdu(cmd, 4, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "TERMINATE ok");

    /* terminated: everything except ACTIVATE answers 6285 */
    n = build_get_data(0x00, 0x4F, cmd);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6285, "terminated state");
    n = build_select(cmd, NULL, 0);
    rlen = openpgp_card_apdu(cmd, n, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6285, "SELECT also 6285 while terminated");

    /* ACTIVATE re-initializes: fresh card, key wiped, factory PINs */
    cmd[0]=0x00; cmd[1]=0x44; cmd[2]=0x00; cmd[3]=0x00;
    rlen = openpgp_card_apdu(cmd, 4, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "ACTIVATE ok");
    do_select(cmd, rsp);
    TEST_ASSERT(!openpgp_card_key_is_set(), "key wiped by factory reset");
    do_verify_pw3(cmd, rsp);   /* factory PW3 works again */
}
```

(`build_select(cmd, NULL, 0)` — adapt to the existing helper's actual signature.) Register all three.

- [ ] **Step 2: Run red.**

- [ ] **Step 3: Implement.**
  - **C4 PUT**: in INS 0xDA, intercept `tag == 0x00C4` before the generic path: accept `lc == 1` (or `lc == 4` from older gpg — use byte0) with byte0 ∈ {0,1}, store as a 1-byte DO under `0x00C4` (the DO store persists it via `pgp_dos`); else 6A80. `fill_pw_status()` byte0 = stored value (default 0). The PSO:CDS consume line becomes:
    ```c
        /* Consume the sign token only in "valid for one signature" mode. */
        const uint8_t *c4v; uint16_t c4n;
        bool multi = openpgp_do_get(0x00C4u, &c4v, &c4n) && c4n >= 1 && c4v[0] == 0x01;
        if (!multi) s_pw1_sign_verified = false;
    ```
    GET DATA `0x00C4` keeps synthesizing the 7-byte status (byte0 now live).
  - **DO 0xDE**: synthesized in GET DATA (like C4) AND appended to the `73` template in the `6E` builder:
    ```c
    /* Key Information DO 0xDE (OpenPGP 3.4 §4.4.3.9): key-ref, status pairs.
     * Status: 0 = not present, 1 = generated on card, 2 = imported. */
    static uint16_t fill_key_info(uint8_t de[6])
    {
        for (int i = 0; i < OPENPGP_SLOT_COUNT; i++) {
            de[2*i]     = (uint8_t)(i + 1);
            de[2*i + 1] = s_keys[i].set ? s_keys[i].origin : 0x00;
        }
        return 6;
    }
    ```
  - **TERMINATE/ACTIVATE**: a `static bool s_terminated;` (RAM-only — gpg sends both in one session; a power-cycle mid-sequence just leaves a factory-reset card, which is fine). At the very top of `openpgp_card_apdu` after parse:
    ```c
        /* Terminated DF (after INS 0xE6): only ACTIVATE FILE revives the card. */
        if (s_terminated) {
            if (a.ins == 0x44) {
                s_terminated = false;
                openpgp_card_factory_reset();
                return sw_only(out, out_max, SW_OK);
            }
            return sw_only(out, out_max, 0x6285u);
        }
    ```
    Dispatch blocks (after CHANGE REFERENCE DATA):
    ```c
        /* ---- TERMINATE DF (INS=0xE6) — gpg factory-reset, step 1 ---- */
        if (a.ins == 0xE6) {
            if (!(s_pw3_verified || (s_pw1_retry == 0 && s_pw3_retry == 0)))
                return sw_only(out, out_max, SW_SEC_NOT_SAT);
            openpgp_card_factory_reset();
            s_terminated = true;
            return sw_only(out, out_max, SW_OK);
        }
        /* ---- ACTIVATE FILE (INS=0x44) — no-op when not terminated ---- */
        if (a.ins == 0x44)
            return sw_only(out, out_max, SW_OK);
    ```
    `s_terminated = false;` added to `openpgp_card_init()`.

- [ ] **Step 4: Run green.**

- [ ] **Step 5: HW spot-check + commit.** Flash, then `gpg --card-edit` → `admin` → `forcesig` (toggles C4 — check `Signature PIN ....: not forced` flips) and `factory-reset` (full wipe → card comes back factory-fresh, `gpg --card-status` shows empty keys, PW1/PW3 back to defaults).

```bash
git add main/security/openpgp_card.c test/test_openpgp_card.c
git commit -m "feat(pgp): PW1 validity (forcesig), key-info DO 0xDE, TERMINATE/ACTIVATE factory reset"
```

---

### Task 7: Fuzzer extension, security audit, docs, final end-to-end with real keys

**Files:**
- Modify: `test/fuzz/fuzz_openpgp.c`
- Modify: `docs/superpowers/specs/2026-06-09-dongle-openpgp-card-design.md`
- Create: `docs/OPENPGP_CARD.md`
- Memory: update `project_dongle_openpgp`

- [ ] **Step 1: Fuzzer hooks v2 + corpus.** Update the fake hooks in `fuzz_openpgp.c` to the v2 signatures (mirror the test-file fakes from Task 1, including an `ecdh` that requires `peer_n == 32` and a `genkey` that fills a pattern). Extend the structured-APDU generator so the random INS pool includes the new surface — `0x2A` with P1P2 ∈ {9E9A, 8086, random}, `0x88`, `0x47` with P1 ∈ {0x80, 0x81} and CRT bytes {B6, B8, A4, random}, `0xE6`, `0x44`, and PUT DATA against C1/C2/C3/C4 with random bodies. Keep the invariant checks: no crash (ASan/UBSan), the canned sign/ecdh output never contains the private scalar bytes, and a response is always ≥ 2 bytes.

- [ ] **Step 2: Run the fuzzer.**

```bash
cd test/fuzz
gcc -std=c11 -g -O1 -fsanitize=address,undefined -DTEST_HOST \
    -I../.. -I../../main/security -I../../main/comm/cdc \
    -I../../main/input -I../../boards/kase_v2_debug \
    fuzz_openpgp.c ../../main/security/openpgp_card.c \
    ../../main/security/openpgp_do.c ../../main/security/apdu.c \
    ../../main/security/sec_confirm.c -o fuzz_openpgp
./fuzz_openpgp
```

Expected: `Fuzz complete: N iterations, 0 crashes` (N ≥ 150000). Any finding: fix + host test + re-run.

- [ ] **Step 3: Security audit.** Invoke the `kase-security-auditor` agent, scope: (a) the three new INS handlers' input validation (DECIPHER TLV parse, INT-AUTH bounds, GENERATE persistence failure paths); (b) the **multi-UIF question carried from the Phase-1 pentest**: three UIF-gated ops now share the single `sec_confirm` slot `0xF0` — verify the CCID serialization argument still holds (one APDU in flight: `bMaxCCIDBusySlots=1`, single worker, `s_busy` guard) and document the invariant in a comment near `CCID_CONFIRM_SLOT`; (c) key material hygiene (scrub-on-failure paths, no scalar in any response); (d) PW1-82 non-consumption semantics (deliberate, spec-conform — confirm no privilege creep into PW3 paths). Apply fixes with tests; commit per fix.

- [ ] **Step 4: Spec + decision docs.** In `2026-06-09-dongle-openpgp-card-design.md`:
  - Add **§2b — Release-eng decision (2026-06-10): secure boot V2 + flash encryption NOT enabled.** Body: threat model is host-malware-only (§2); the dongle is a personal, non-distributed, deliberately reprogrammable device; both features burn irreversible eFuses and would end free reflashing. **Re-evaluation trigger:** before distributing flashed hardware to any third party. Pointer: ESP-IDF Security Guide (secure boot V2 + flash-encryption release mode), and note the lighter future option already in §2 (eFuse-HMAC KEK "Couche 1" for NVS-at-rest encryption without losing reprogrammability).
  - Add **§2c — Quantum posture (2026-06-10): classical algos by necessity, trivial rotation.** Body: no gpg OpenPGP card format supports PQC in 2026; P-256/X25519 are the only viable choice and match every commercial token. Exposure: auth/sign need a *live* quantum attacker (est. 2035+), only the X25519 decrypt slot is harvest-now-decrypt-later (accepted — dev-identity role, not long-lived confidentiality). Mitigation = on-device regeneration (Task 5) when gpg ships PQC card algos; nothing fused (ties to §2b). Note the SSH session-KEX warnings are a separate transport layer, fixed host-side.
  - Add **§8b — Phase 2 RESULT** mirroring §7b's structure: what passed on hardware (decrypt, SSH, on-card generate, factory-reset), gotchas discovered during Phase-2 bring-up (fill during execution — e.g. any X25519 endianness or 0x40-prefix surprise), and what remains parked (RSA + extended APDU, with the "Mae has zero RSA keys" rationale).

- [ ] **Step 5: User guide `docs/OPENPGP_CARD.md`.** Sections: what the card is (3 slots, algos, touch policy); host setup (udev already in 99-local.rules, scdaemon internal driver, no pcscd); PINs (defaults, change via `gpg --card-edit` → `passwd`, retry behavior); **key generation on-device** (`gpg --card-edit` → `admin` → `generate`, no off-card backup = the point); git signing setup (`git config --global user.signingkey <keyid>`, `commit.gpgsign true`); SSH setup (`enable-ssh-support`, `SSH_AUTH_SOCK`, NixOS note for making it persistent); UIF touch control (`uif 1|2|3 on`); factory reset; troubleshooting (kill scdaemon, scd.log guru, console-UART debug procedure with the REVERT warning).
  Add a **"Quantum posture"** section: (1) the card keys are classical (P-256/X25519) because no gpg card format supports PQC in 2026 — same as every commercial token; (2) exposure table (auth/sign need a live quantum attacker, only the X25519 decrypt slot is harvest-now-decrypt-later, accepted for a dev-identity key); (3) **rotation plan** — regenerate on-device when gpg ships PQC card algos, nothing is fused; (4) **the SSH "quantum" server warnings are the session KEX, not this card** — fix host-side with `KexAlgorithms sntrup761x25519-sha512@openssh.com,curve25519-sha256` in `~/.ssh/config` (include the snippet). Cross-link the spec §2c.

- [ ] **Step 6: Final end-to-end — real keys, real `~/.gnupg`.** The Phase-2 deliverable:

```bash
# 1. wipe the test identity from the card
nix-shell -p gnupg --run 'GNUPGHOME=$HOME/kase-pgp-test gpg --card-edit'  # factory-reset
# 2. real keyring (default ~/.gnupg), real PINs first:
nix-shell -p gnupg --run 'gpg --card-edit'   # admin → passwd → set real PW1/PW3
# 3. generate Mae's identity ON the card (admin → generate, no backup)
# 4. git signing:
git config --global user.signingkey <new keyid>
git config --global commit.gpgsign true
# 5. SSH: enable-ssh-support in ~/.gnupg/gpg-agent.conf, SSH_AUTH_SOCK export
#    in the NixOS home config, ssh-add -L pubkey added to GitLab.
```

Acceptance: one real signed commit on a scratch branch (touch required, `git log --show-signature` = Good signature), one `ssh -T git@gitlab.com` via the card (touch if uif 3 on), one encrypt/decrypt roundtrip. Run `docs/HARDWARE_SMOKE_TEST.md` (keyboard still the daily driver). `./scripts/check.sh` fully green (6 boards + host).

- [ ] **Step 7: Memory + final commit.** Update the `project_dongle_openpgp` memory: Phase 2 done (what was validated), RSA parked + why, secure-boot decision, any new HW gotchas. Commit docs + fuzzer:

```bash
git add test/fuzz/fuzz_openpgp.c docs/OPENPGP_CARD.md docs/superpowers/specs/2026-06-09-dongle-openpgp-card-design.md
git commit -m "docs(pgp): Phase 2 result + OpenPGP card user guide; fuzz: cover decrypt/auth/keygen/terminate surface"
```

---

## Risks / open points carried into execution

1. **X25519 byte-order vs live gpg** — the BE-scalar convention (reverse + clamp) matches Gnuk's handling of gpg's MPI encoding, and the 0x40 point prefix matches SmartPGP; both are KAT-covered, but the Task 3 HW checkpoint is the real oracle. Symptom of a mismatch: decrypt produces garbage (host-side KDF fails) with SW 9000. The debug path is written into Task 3 Step 6.
2. **`mbedtls_ecp_point_read_binary` on CURVE25519** assumes mbedtls 3.x Montgomery support (32-byte LE u). If the ESP-IDF fork rejects it, fall back to building the point manually (`mbedtls_mpi_read_binary_le` into `Qp.X`, `lset(Qp.Z, 1)`).
3. **gpg's generate flow exact APDU order** (PUT C1–C3 before/after GENERATE, which CRTs) — covered by serving validation-on-write and reading `slot_algo()` live at generate time, but watch scd.log at the Task 5 checkpoint.
4. **Worker stack** with keygen on top of ECDSA — HWM log + bump to 8192 if tight (noted in Task 5).
5. **sec_confirm single slot 0xF0 with 3 UIF ops** — believed safe by CCID serialization; Task 7 Step 3 makes the auditor prove or fix it.

## Self-review notes (done while writing)

- Spec §8 coverage: decrypt slot → T2/T3, auth slot → T4, on-device keygen → T5, "full DO set" → T1 (DE origin field) + T6, RSA → explicitly parked (ratified decision, §8b documents it). Phase-1 pentest carry-overs: multi-UIF re-audit → T7 Step 3, secure-boot decision → T7 Step 4.
- Type consistency pass: hooks v2 signatures identical in T1 (header), T1 tests, T3 ccid.c, T7 fuzzer; `slot_from_crt`/`slot_algo`/`build_pubkey_response`/`uif_required`/`ds_counter_reset` defined once (T1/T2/T5) and reused by name afterward.
- `build_read_pubkey(p1, crt, buf)` exists from Phase 1 tests (signature `(uint8_t p1, uint8_t crt_tag, uint8_t *buf)`) — reused for GENERATE with P1=0x80.
- All responses stay < 271 B (largest: 6E ≈ 246 B unchanged; 7F49 P-256 = 72 B) — no CCID descriptor/buffer change anywhere.
- `find_index` helper exists in the test file (line ~149); `g_fake_sign_last_d` exists (Phase 1).
