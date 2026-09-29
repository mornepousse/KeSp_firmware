# Chest Link v2 (S3 master) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the left half's chest master from protocol v1 to v2: confirmation bound to the armed instance, USB mode chosen from the keyboard (`K_CHEST_NEXT`) with the active mode read back and shown, and `K_SEC_CONFIRM` counted from the left half only.

**Architecture:** Same three layers as v1. `chest_proto` (pure) gets the v2 map, the instance and active mode, the mode cycle / self-heal / arrival tracking, and the instance-aware confirm rule — pinned to the contract's vectors V1–V14. `chest_gate` (pure) carries a (op, instance) tag instead of an op, plus a lock-free "next mode" request. `chest_link` (firmware) writes `{0x5A, instance}` at `0x10` and the wanted mode at `0x12` (rewritten whenever the read-back differs). The memory-LCD shows the active mode (upper), a pending one (lower) or `ERR`.

**Tech Stack:** ESP-IDF 5.5 (spi_master half-duplex), FreeRTOS, LVGL 8, host CMake tests.

**Spec:** `docs/superpowers/specs/2026-09-29-chest-link-v2-design.md`
**Contract (authoritative for the wire):** Niphar_chest `docs/LINK_CONTRACT.md` at commit `14f9352` (§1 map, §5 confirmation, §6 mode selection, §11 vectors). The chest is flashed at `14f9352`.

## Global Constraints

- Protocol version **2**; register map: `0x00-0x0B` unchanged (magic `NIPH`, version, state, pending op LE, counter LE), `0x0C` instance, `0x0D` ACTIVE mode, `0x0E-0x0F` CRC-16/MCRF4XX (`cr_crc16`, check `0x6F91`) over `0x00..0x0D` (14 bytes, LE); master word `0x10` confirm `0x5A`, `0x11` instance echo, `0x12` requested mode, `0x13` reserved.
- Mode wire values: `0x00` none, `0x01` storage, `0x02` pgp, `0x03` otp, `0x04` fido, `0x05` oath; active `0xFF` = switch in flight (not an error, up to ~15 s).
- A block with `USB_MOUNTED` (state bit 1) set and an active mode that is not in `0x01..0x05` is CORRUPT.
- Confirmation write: ONE WRBUF of 2 bytes at `0x10` = `{0x5A, instance}`; mode write: ONE WRBUF of 1 byte at `0x12`; never both in one write.
- `K_SEC_CONFIRM` counts only from the left half's columns (`col < MATRIX_COLS` when `KEYMAP_COLS > MATRIX_COLS`); a right-half press reaches neither the chest nor the local `sec_confirm` gate.
- `K_CHEST_NEXT` = `0x3E01`; must appear in `docs/KEYCODE_MAP.md` (the `keycode-map` tripwire brick enforces it).
- Security invariant unchanged: `chest_gate_press()` called only from `main/input/key_processor.c` (tripwire brick `chest-confirm.sh`). `chest_gate_mode_next()` is not a security gesture and is not restricted.
- One chip one owner: every chest transaction under `rf_bus_lock`; GPIO3 driven only while present; nothing new runs on battery.
- Comments/commits in English; TDD (test first, red, green); `./scripts/check.sh --fast` green per task; full six-board check where firmware sources change; one commit per task; no push.

## Review Focus

- V14 (switch in flight, active `0xFF`) must render as pending, never as a fault — pinned in Task 1 (`chest_mode_track`) and Task 3.
- A press for op A instance 3 must not confirm op A instance 4 armed within the next 250 ms — pinned in Task 1 (`chest_press_matches` on the tag) and Task 2.
- A right-half `K_SEC_CONFIRM` must not authorize the LOCAL gate either — pinned in Task 2.
- After a chest reboot (`0x12` reads back `0x00`) the wanted mode is rewritten — pinned in Task 1 (`chest_mode_needs_write`) and by reading in Task 4.
- A v1 chest (version 1) is refused as BAD_VERSION (`P4?`), not half-parsed — pinned in Task 1 (V5-style: version ≠ 2).

---

### Task 1: `chest_proto` v2 — map, vectors, mode logic, instance-aware confirm

**Files:**
- Modify: `main/comm/chest/chest_proto.h`, `main/comm/chest/chest_proto.c`
- Modify: `test/test_chest_proto.c` (vectors replaced, tests updated/added)
- Modify: `test/test_keycode_report.c` ONLY if it breaks on the `chest_press_matches` signature (it should not: it uses the gate, Task 2)

**Interfaces — Produces (`chest_proto.h`, replace the v1 definitions):**

```c
#define CHEST_PROTO_VERSION     2
#define CHEST_REG_SIZE          0x14
#define CHEST_REG_INSTANCE      0x0C
#define CHEST_REG_MODE_ACTIVE   0x0D
#define CHEST_REG_CRC           0x0E
#define CHEST_REG_CRC_SPAN      0x0E   /* CRC over 0x00..0x0D */
#define CHEST_REG_USER_CONFIRM  0x10
#define CHEST_REG_ECHO          0x11
#define CHEST_REG_MODE_REQ      0x12
#define CHEST_CONFIRM_MAGIC     0x5A
#define CHEST_STATE_SD          0x01
#define CHEST_STATE_USB         0x02
#define CHEST_STATE_READY       0x04
#define CHEST_CMD_WRBUF         0x01
#define CHEST_CMD_RDBUF         0x02
#define CHEST_CONFIRM_RETRY_MS  200u
#define CHEST_LABEL_BUF         8
/* USB mode wire values (contract §6.1) */
#define CHEST_MODE_NONE      0x00
#define CHEST_MODE_STORAGE   0x01
#define CHEST_MODE_PGP       0x02
#define CHEST_MODE_OTP       0x03
#define CHEST_MODE_FIDO      0x04
#define CHEST_MODE_OATH      0x05
#define CHEST_MODE_COUNT     6
#define CHEST_MODE_IN_FLIGHT 0xFF
#define CHEST_MODE_FAULT_READS 8     /* 2 s at the 250 ms read cadence */
#define CHEST_MODE_LABEL_BUF 5       /* 4 UNSCII characters + NUL */

/* A press as seen by the owner: the op on screen AND the arming it belongs to. */
#define CHEST_TAG(op, inst)  ((uint32_t)(op) | ((uint32_t)(inst) << 16))
#define CHEST_TAG_OP(t)      ((uint16_t)((t) & 0xFFFFu))
#define CHEST_TAG_INST(t)    ((uint8_t)(((t) >> 16) & 0xFFu))

typedef struct {
    uint8_t  version, state;
    uint16_t pending_op;
    uint32_t confirm_count;
    uint8_t  instance;       /* 0x0C */
    uint8_t  active_mode;    /* 0x0D: CHEST_MODE_* or CHEST_MODE_IN_FLIGHT */
} chest_status_t;

typedef enum { CHEST_BLOCK_OK = 0, CHEST_BLOCK_ABSENT, CHEST_BLOCK_BAD_VERSION, CHEST_BLOCK_CORRUPT } chest_block_t;

bool          chest_proto_is_absent(const uint8_t *regs, size_t len);
/* Contract order: short, absent, magic, version, CRC; then the mounted
 * invariant (USB_MOUNTED set => active mode in 0x01..0x05, else CORRUPT).
 * `out` written only on OK. */
chest_block_t chest_proto_parse(const uint8_t *regs, size_t len, chest_status_t *out);
/* The press confirms THIS round only if: OK block, READY, tag != 0, and both
 * the op AND the instance of the tag equal the block's pending op and instance. */
bool          chest_press_matches(uint32_t pressed_tag, chest_block_t block, const chest_status_t *st);
void          chest_op_label(uint16_t op, char out[CHEST_LABEL_BUF]);   /* unchanged */

/* Confirm delivery: one write of {0x5A, instance}, one retry after 200 ms if the
 * counter has not moved and the same op AND instance are still pending, never a third. */
typedef struct { bool armed; uint8_t writes; uint16_t op; uint8_t instance; uint32_t count0, t_ms; } chest_confirm_t;
bool chest_confirm_request(chest_confirm_t *c, const chest_status_t *st, uint32_t now_ms);
bool chest_confirm_step(chest_confirm_t *c, const chest_status_t *st, uint32_t now_ms);

/* Mode selection (contract §6). */
uint8_t chest_mode_next(uint8_t mode);                                  /* 0->1->..->5->0; unknown -> 0 */
bool    chest_mode_needs_write(const uint8_t *regs, uint8_t wanted);    /* regs[0x12] != wanted */
typedef enum { CHEST_MODE_ARRIVED = 0, CHEST_MODE_PENDING, CHEST_MODE_FAULT } chest_mode_state_t;
typedef struct { uint8_t differ_reads; } chest_mode_track_t;
/* One OK read: ARRIVED if active == wanted; PENDING if active == IN_FLIGHT
 * (counter reset) or two known values differ for < FAULT_READS reads;
 * FAULT once they have differed for FAULT_READS consecutive reads. */
chest_mode_state_t chest_mode_track(chest_mode_track_t *t, uint8_t active, uint8_t wanted);
/* 4-character line: ARRIVED -> upper name of `active` ("" for none);
 * PENDING -> lower name of `wanted` ("" for none); FAULT -> "ERR".
 * Names: none "", storage MSC, pgp PGP, otp OTP, fido FIDO, oath OATH. */
void    chest_mode_label(chest_mode_state_t s, uint8_t active, uint8_t wanted, char out[CHEST_MODE_LABEL_BUF]);
```

- [ ] **Step 1: Replace the vectors and write the failing tests** in `test/test_chest_proto.c`. Remove the v1 V1..V9 arrays and their assertions; copy the v2 bytes VERBATIM from Niphar_chest `docs/LINK_CONTRACT.md` §11 (commit `14f9352`):

```c
/* Niphar_chest docs/LINK_CONTRACT.md §11, v2, commit 14f9352 — copied verbatim, never edited. */
static const uint8_t V1[20]  = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V2[20]  = { 0 };
static const uint8_t V3[20]  = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const uint8_t V4[20]  = { 0x4E,0x49,0x50,0x58,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V5[20]  = { 0x4E,0x49,0x50,0x48,0x03,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xCC,0x07,0x00,0x00,0x00,0x00 };
static const uint8_t V6[20]  = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2B,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V6b[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEA,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V6c[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2A,0x00,0x00,0x00,0x00 };
static const uint8_t V6d[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x02,0x01,0xEB,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V6e[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x05,0xEB,0x2B,0x00,0x00,0x00,0x00 };
static const uint8_t V8[20]  = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x5A,0x03,0x00,0x00 };
static const uint8_t V9[20]  = { 0x4E,0x49,0x50,0x48,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x39,0xD4,0x00,0x00,0x00,0x00 };
static const uint8_t V10[20] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };
static const uint8_t V11[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x5A,0x02,0x00,0x00 };
static const uint8_t V12[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x00,0x00,0x09,0x00 };
static const uint8_t V13[20] = { 0x4E,0x49,0x50,0x48,0x02,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0x03,0x01,0xEB,0x2B,0x00,0x00,0x02,0x00 };
static const uint8_t V14[20] = { 0x4E,0x49,0x50,0x48,0x02,0x05,0x00,0x00,0x2A,0x00,0x00,0x00,0x03,0xFF,0x5F,0x2F,0x00,0x00,0x00,0x00 };
```

Tests to write (replace `test_chest_contract_vectors`, keep `test_chest_crc_check_value`, `test_chest_op_labels`, noise and absence tests adapted to v2 blocks):

```c
static void test_chest_v2_vectors(void)
{
    chest_status_t s;
    TEST_ASSERT_EQ(chest_proto_parse(V1, 20, &s), CHEST_BLOCK_OK, "V1 nominal");
    TEST_ASSERT_EQ(s.version, 2, "V1 version 2");
    TEST_ASSERT_EQ(s.state, 0x07, "V1 state");
    TEST_ASSERT_EQ(s.pending_op, 1, "V1 PSO:CDS");
    TEST_ASSERT_EQ(s.confirm_count, 42, "V1 count 42");
    TEST_ASSERT_EQ(s.instance, 3, "V1 instance 3");
    TEST_ASSERT_EQ(s.active_mode, CHEST_MODE_STORAGE, "V1 active storage");
    TEST_ASSERT_EQ(chest_proto_parse(V2, 20, &s), CHEST_BLOCK_ABSENT, "V2 absent 0x00");
    TEST_ASSERT_EQ(chest_proto_parse(V3, 20, &s), CHEST_BLOCK_ABSENT, "V3 absent 0xFF");
    TEST_ASSERT_EQ(chest_proto_parse(V4, 20, &s), CHEST_BLOCK_CORRUPT, "V4 bad magic");
    TEST_ASSERT_EQ(chest_proto_parse(V5, 20, &s), CHEST_BLOCK_BAD_VERSION, "V5 version 3 refused");
    TEST_ASSERT_EQ(chest_proto_parse(V6, 20, &s), CHEST_BLOCK_CORRUPT, "V6 payload bit");
    TEST_ASSERT_EQ(chest_proto_parse(V6b, 20, &s), CHEST_BLOCK_CORRUPT, "V6b CRC low byte");
    TEST_ASSERT_EQ(chest_proto_parse(V6c, 20, &s), CHEST_BLOCK_CORRUPT, "V6c CRC high byte");
    TEST_ASSERT_EQ(chest_proto_parse(V6d, 20, &s), CHEST_BLOCK_CORRUPT, "V6d instance covered by the CRC");
    TEST_ASSERT_EQ(chest_proto_parse(V6e, 20, &s), CHEST_BLOCK_CORRUPT, "V6e active mode covered by the CRC");
    TEST_ASSERT_EQ(chest_proto_parse(V1, 19, &s), CHEST_BLOCK_CORRUPT, "V7 truncated");
    TEST_ASSERT_EQ(chest_proto_parse(V8, 20, &s), CHEST_BLOCK_OK, "V8 master word outside the CRC");
    TEST_ASSERT_EQ(chest_proto_parse(V9, 20, &s), CHEST_BLOCK_OK, "V9 present, booting");
    TEST_ASSERT_EQ(s.state, 0, "V9 not ready");
    TEST_ASSERT_EQ(s.instance, 0, "V9 nothing ever armed");
    TEST_ASSERT_EQ(s.active_mode, CHEST_MODE_NONE, "V9 no active mode");
    TEST_ASSERT(!chest_proto_is_absent(V10, 20), "V10 uniform but the last byte: present");
    TEST_ASSERT_EQ(chest_proto_parse(V10, 20, &s), CHEST_BLOCK_CORRUPT, "V10 not a chest");
    TEST_ASSERT_EQ(chest_proto_parse(V11, 20, &s), CHEST_BLOCK_OK, "V11 block valid");
    TEST_ASSERT_EQ(chest_proto_parse(V12, 20, &s), CHEST_BLOCK_OK, "V12 block valid");
    TEST_ASSERT_EQ(chest_proto_parse(V13, 20, &s), CHEST_BLOCK_OK, "V13 block valid");
    TEST_ASSERT_EQ(chest_proto_parse(V14, 20, &s), CHEST_BLOCK_OK, "V14 switch in flight is valid");
    TEST_ASSERT_EQ(s.active_mode, CHEST_MODE_IN_FLIGHT, "V14 active indeterminate");
    TEST_ASSERT_EQ(s.state & CHEST_STATE_USB, 0, "V14 USB_MOUNTED cleared");
}

/* A v1 chest (the 20-byte v1 map, version 1) is refused, not half-parsed. */
static void test_chest_v1_chest_refused(void)
{
    static const uint8_t v1_nominal[20] = { 0x4E,0x49,0x50,0x48,0x01,0x07,0x01,0x00,0x2A,0x00,0x00,0x00,0xAF,0xEA,0x00,0x00,0x00,0x00,0x00,0x00 };
    chest_status_t s;
    TEST_ASSERT_EQ(chest_proto_parse(v1_nominal, 20, &s), CHEST_BLOCK_BAD_VERSION, "a v1 chest is BAD_VERSION");
}

/* USB_MOUNTED set with no known mode is self-contradictory: corrupt (contract §1). */
static void test_chest_mounted_without_mode_is_corrupt(void)
{
    uint8_t b[20]; chest_status_t s;
    memcpy(b, V1, 20);
    b[CHEST_REG_MODE_ACTIVE] = CHEST_MODE_NONE;
    uint16_t crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF); b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "mounted + active none");
    b[CHEST_REG_MODE_ACTIVE] = CHEST_MODE_IN_FLIGHT;
    crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF); b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "mounted + active in flight");
    b[CHEST_REG_MODE_ACTIVE] = CHEST_MODE_OATH;
    crc = cr_crc16(b, CHEST_REG_CRC_SPAN);
    b[CHEST_REG_CRC] = (uint8_t)(crc & 0xFF); b[CHEST_REG_CRC + 1] = (uint8_t)(crc >> 8);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_OK, "mounted + a known mode");
}

static void test_chest_press_matches_tag(void)
{
    chest_status_t s;
    chest_proto_parse(V1, 20, &s);                                   /* op 1, instance 3, READY */
    TEST_ASSERT(chest_press_matches(CHEST_TAG(1, 3), CHEST_BLOCK_OK, &s), "same op, same instance");
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(1, 2), CHEST_BLOCK_OK, &s), "same op, older instance");
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(2, 3), CHEST_BLOCK_OK, &s), "other op");
    TEST_ASSERT(!chest_press_matches(0, CHEST_BLOCK_OK, &s), "no press");
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(1, 3), CHEST_BLOCK_CORRUPT, &s), "non-OK block");
    chest_proto_parse(V9, 20, &s);
    TEST_ASSERT(!chest_press_matches(CHEST_TAG(0, 0), CHEST_BLOCK_OK, &s), "not READY, nothing pending");
}

static void test_chest_confirm_rule_v2(void)
{
    chest_status_t a, b;
    chest_proto_parse(V1, 20, &a);                                   /* op 1, inst 3, count 42 */
    chest_confirm_t c = {0};
    TEST_ASSERT(chest_confirm_request(&c, &a, 1000), "pending: accepted");
    TEST_ASSERT_EQ(c.instance, 3, "instance recorded");
    TEST_ASSERT(chest_confirm_step(&c, &a, 1000), "first write");
    TEST_ASSERT(!chest_confirm_step(&c, &a, 1199), "no retry at 199 ms");
    TEST_ASSERT(chest_confirm_step(&c, &a, 1200), "one retry at 200 ms");
    TEST_ASSERT(!chest_confirm_step(&c, &a, 1400), "never a third");
    /* Instance changed before the retry: same op code, another arming — no retry. */
    chest_confirm_request(&c, &a, 2000);
    TEST_ASSERT(chest_confirm_step(&c, &a, 2000), "write");
    b = a; b.instance = 4;
    TEST_ASSERT(!chest_confirm_step(&c, &b, 2250), "new instance: disarmed, no retry");
    TEST_ASSERT(!c.armed, "disarmed");
    /* Counter moved: delivered. */
    chest_confirm_request(&c, &a, 3000);
    chest_confirm_step(&c, &a, 3000);
    b = a; b.confirm_count = 43;
    TEST_ASSERT(!chest_confirm_step(&c, &b, 3050), "delivered");
    b = a; b.pending_op = 0;
    TEST_ASSERT(!chest_confirm_request(&c, &b, 4000), "nothing pending: dropped");
}

static void test_chest_mode_logic(void)
{
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_NONE), CHEST_MODE_STORAGE, "none -> storage");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_STORAGE), CHEST_MODE_PGP, "storage -> pgp");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_PGP), CHEST_MODE_OTP, "pgp -> otp");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_OTP), CHEST_MODE_FIDO, "otp -> fido");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_FIDO), CHEST_MODE_OATH, "fido -> oath");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_OATH), CHEST_MODE_NONE, "oath -> none");
    TEST_ASSERT_EQ(chest_mode_next(0x09), CHEST_MODE_NONE, "unknown -> none");
    TEST_ASSERT_EQ(chest_mode_next(CHEST_MODE_IN_FLIGHT), CHEST_MODE_NONE, "0xFF -> none");
    TEST_ASSERT(!chest_mode_needs_write(V13, CHEST_MODE_PGP), "V13 already asks pgp");
    TEST_ASSERT(chest_mode_needs_write(V1, CHEST_MODE_PGP), "V1 asks none: rewrite pgp (chest reboot self-heal)");
    TEST_ASSERT(!chest_mode_needs_write(V1, CHEST_MODE_NONE), "none wanted, none written");
    TEST_ASSERT(chest_mode_needs_write(V12, CHEST_MODE_NONE), "V12 carries 0x09: rewrite");
}

static void test_chest_mode_track(void)
{
    chest_mode_track_t t = {0};
    TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_PGP, CHEST_MODE_PGP), CHEST_MODE_ARRIVED, "arrived");
    /* V14: in flight for as long as the chest says so — never a fault. */
    for (int i = 0; i < 100; i++)
        TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_IN_FLIGHT, CHEST_MODE_OATH), CHEST_MODE_PENDING, "0xFF is pending");
    /* Two known values differing: pending for 7 reads, fault at the 8th. */
    for (int i = 0; i < CHEST_MODE_FAULT_READS - 1; i++)
        TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_STORAGE, CHEST_MODE_OATH), CHEST_MODE_PENDING, "differs, not yet a fault");
    TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_STORAGE, CHEST_MODE_OATH), CHEST_MODE_FAULT, "persisting: fault");
    TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_IN_FLIGHT, CHEST_MODE_OATH), CHEST_MODE_PENDING, "in flight resets the count");
    TEST_ASSERT_EQ(chest_mode_track(&t, CHEST_MODE_OATH, CHEST_MODE_OATH), CHEST_MODE_ARRIVED, "caught up");
}

static void test_chest_mode_label(void)
{
    char l[CHEST_MODE_LABEL_BUF];
    chest_mode_label(CHEST_MODE_ARRIVED, CHEST_MODE_FIDO, CHEST_MODE_FIDO, l);
    TEST_ASSERT(strcmp(l, "FIDO") == 0, "arrived: upper");
    chest_mode_label(CHEST_MODE_ARRIVED, CHEST_MODE_STORAGE, CHEST_MODE_STORAGE, l);
    TEST_ASSERT(strcmp(l, "MSC") == 0, "storage is MSC");
    chest_mode_label(CHEST_MODE_ARRIVED, CHEST_MODE_NONE, CHEST_MODE_NONE, l);
    TEST_ASSERT(l[0] == '\0', "none: nothing");
    chest_mode_label(CHEST_MODE_PENDING, CHEST_MODE_IN_FLIGHT, CHEST_MODE_OATH, l);
    TEST_ASSERT(strcmp(l, "oath") == 0, "pending: wanted, lower");
    chest_mode_label(CHEST_MODE_FAULT, CHEST_MODE_STORAGE, CHEST_MODE_PGP, l);
    TEST_ASSERT(strcmp(l, "ERR") == 0, "fault");
    for (uint8_t m = 0; m < CHEST_MODE_COUNT; m++) {
        chest_mode_label(CHEST_MODE_ARRIVED, m, m, l);
        TEST_ASSERT(strlen(l) <= 4, "4 characters max");
    }
}
```

Register them in `test_chest_proto()`; keep `test_chest_crc_check_value`, `test_chest_op_labels`, `test_chest_noise_is_not_a_chest` (adapt its V5 use: a version-3 block is BAD_VERSION) and `test_chest_absence_full_scan`; delete the v1 `test_chest_contract_vectors`, `test_chest_confirm_rule` and `test_chest_press_matches` (replaced by the v2 ones above). Keep `#include "../main/security/cr_crc16.h"`.

- [ ] **Step 2: Run to verify it fails** — `./scripts/check.sh --fast` → RED (new symbols undefined).

- [ ] **Step 3: Implement** in `chest_proto.c`:

```c
chest_block_t chest_proto_parse(const uint8_t *regs, size_t len, chest_status_t *out)
{
    if (!regs || len < CHEST_REG_SIZE) return CHEST_BLOCK_CORRUPT;
    if (chest_proto_is_absent(regs, CHEST_REG_SIZE)) return CHEST_BLOCK_ABSENT;
    if (memcmp(regs, k_magic, 4) != 0) return CHEST_BLOCK_CORRUPT;
    if (regs[0x04] != CHEST_PROTO_VERSION) return CHEST_BLOCK_BAD_VERSION;
    if (get_u16(&regs[CHEST_REG_CRC]) != cr_crc16(regs, CHEST_REG_CRC_SPAN)) return CHEST_BLOCK_CORRUPT;
    uint8_t active = regs[CHEST_REG_MODE_ACTIVE];
    if ((regs[0x05] & CHEST_STATE_USB) && !(active >= CHEST_MODE_STORAGE && active <= CHEST_MODE_OATH))
        return CHEST_BLOCK_CORRUPT;                    /* mounted without a mode: contract §1 */
    if (out) {
        out->version = regs[0x04];
        out->state = regs[0x05];
        out->pending_op = get_u16(&regs[0x06]);
        out->confirm_count = get_u32(&regs[0x08]);
        out->instance = regs[CHEST_REG_INSTANCE];
        out->active_mode = active;
    }
    return CHEST_BLOCK_OK;
}

bool chest_press_matches(uint32_t pressed_tag, chest_block_t block, const chest_status_t *st)
{
    if (block != CHEST_BLOCK_OK || !st || pressed_tag == 0) return false;
    if (!(st->state & CHEST_STATE_READY)) return false;
    return CHEST_TAG_OP(pressed_tag) == st->pending_op && st->pending_op != 0
        && CHEST_TAG_INST(pressed_tag) == st->instance;
}

bool chest_confirm_request(chest_confirm_t *c, const chest_status_t *st, uint32_t now_ms)
{
    if (!st || st->pending_op == 0) return false;
    c->armed = true; c->writes = 0; c->op = st->pending_op; c->instance = st->instance;
    c->count0 = st->confirm_count; c->t_ms = now_ms;
    return true;
}

bool chest_confirm_step(chest_confirm_t *c, const chest_status_t *st, uint32_t now_ms)
{
    if (!c->armed || !st) return false;
    if (st->confirm_count != c->count0 || st->pending_op != c->op || st->instance != c->instance) {
        c->armed = false; return false;
    }
    if (c->writes == 0) { c->writes = 1; c->t_ms = now_ms; return true; }
    if ((uint32_t)(now_ms - c->t_ms) < CHEST_CONFIRM_RETRY_MS) return false;
    if (c->writes == 1) { c->writes = 2; c->t_ms = now_ms; return true; }
    c->armed = false;
    return false;
}

uint8_t chest_mode_next(uint8_t mode)
{
    return (mode < CHEST_MODE_OATH) ? (uint8_t)(mode + 1) : CHEST_MODE_NONE;
}

bool chest_mode_needs_write(const uint8_t *regs, uint8_t wanted)
{
    return regs && regs[CHEST_REG_MODE_REQ] != wanted;
}

chest_mode_state_t chest_mode_track(chest_mode_track_t *t, uint8_t active, uint8_t wanted)
{
    if (active == wanted) { t->differ_reads = 0; return CHEST_MODE_ARRIVED; }
    if (active == CHEST_MODE_IN_FLIGHT) { t->differ_reads = 0; return CHEST_MODE_PENDING; }
    if (t->differ_reads < CHEST_MODE_FAULT_READS) t->differ_reads++;
    return (t->differ_reads >= CHEST_MODE_FAULT_READS) ? CHEST_MODE_FAULT : CHEST_MODE_PENDING;
}

void chest_mode_label(chest_mode_state_t s, uint8_t active, uint8_t wanted, char out[CHEST_MODE_LABEL_BUF])
{
    static const char *const up[CHEST_MODE_COUNT] = { "", "MSC", "PGP", "OTP", "FIDO", "OATH" };
    static const char *const lo[CHEST_MODE_COUNT] = { "", "msc", "pgp", "otp", "fido", "oath" };
    const char *txt = "";
    if (s == CHEST_MODE_FAULT) txt = "ERR";
    else if (s == CHEST_MODE_ARRIVED) txt = (active < CHEST_MODE_COUNT) ? up[active] : "";
    else txt = (wanted < CHEST_MODE_COUNT) ? lo[wanted] : "";
    snprintf(out, CHEST_MODE_LABEL_BUF, "%s", txt);
}
```

(Note on `chest_mode_next`: `mode < CHEST_MODE_OATH` covers 0..4; 5, 0xFF and any unknown value go to none.)

Update the file header comments of `chest_proto.h/.c` to say "protocol version 2 — Niphar_chest LINK_CONTRACT.md at 14f9352, vectors V1–V14".

- [ ] **Step 4: Run** — `./scripts/check.sh --fast --force` → GREEN. Prove bites (build OK before each): CRC span 0x0D instead of 0x0E → V6e passes wrongly → RED; drop the mounted invariant → RED; `chest_press_matches` ignoring the instance → RED; `chest_mode_track` counting 0xFF as a difference → RED; revert each.

- [ ] **Step 5: Commit** — `feat(chest): protocol v2 pure logic — instance, active mode, mode cycle/self-heal/arrival, instance-bound confirm, pinned to LINK_CONTRACT v2 vectors V1-V14`.

---

### Task 2: gate tag, `K_CHEST_NEXT`, left-only `K_SEC_CONFIRM`

**Files:**
- Modify: `main/comm/chest/chest_gate.h`, `main/comm/chest/chest_gate.c`
- Modify: `main/input/key_definitions.h` (add `K_CHEST_NEXT`), `docs/KEYCODE_MAP.md` (row)
- Modify: `main/input/key_processor.c` (K_SEC_CONFIRM branch + K_CHEST_NEXT branch)
- Modify: `test/test_keycode_report.c`, `test/CMakeLists.txt` (compile definition)
- Modify: `COMPORTEMENTS.md`

**Interfaces:**
- Consumes: `CHEST_TAG/CHEST_TAG_OP/CHEST_TAG_INST` (Task 1).
- Produces (`chest_gate.h`):

```c
#include "chest_proto.h"                        /* CHEST_TAG */
void     chest_gate_publish(uint16_t pending_op, uint8_t instance);   /* link task only */
uint16_t chest_gate_pending(void);             /* op only (0 = none) */
bool     chest_gate_press(void);               /* SECURITY: key_processor.c only; stores CHEST_TAG(op, instance) seen at press time; true iff op != 0 */
uint32_t chest_gate_take_press(void);          /* link task: the tag (0 = none), cleared */
void     chest_gate_mode_next(void);           /* key_processor: a new K_CHEST_NEXT press (not a security gesture) */
bool     chest_gate_take_mode_next(void);      /* link task: consume the request */
/* Pure: is this keymap column one of the board's LOCAL (left) columns?
 * On a split master (keymap_cols > local_cols) the columns >= local_cols come
 * over the inter-half radio, which is unauthenticated: K_SEC_CONFIRM there is
 * ignored (Mae, 2026-09-29). Always true on a non-split board. */
bool     sec_confirm_from_local(uint8_t col, uint8_t local_cols, uint8_t keymap_cols);
```

- [ ] **Step 1: Failing tests.** In `test/test_keycode_report.c`: update every `chest_gate_publish(x)` call to `chest_gate_publish(x, 0)`; the existing `TEST_ASSERT_EQ(chest_gate_take_press(), 1, …)` stay valid (tag of op 1 instance 0 == 1). Add:

```c
/* The press carries the ARMING seen on screen, not just the op code. */
static void test_kp_sec_confirm_records_the_instance(void)
{
    reset_kp_state();
    chest_gate_publish(1, 7);
    keymaps[0][0][0] = T_K_SEC_CONFIRM;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_press(), CHEST_TAG(1, 7), "op 1 instance 7 recorded");
    chest_gate_publish(0, 0);
}

/* Left half only: a K_SEC_CONFIRM at a remote (right-half) column reaches
 * neither the chest nor the local gate. The host build sets
 * SEC_CONFIRM_LOCAL_COLS=4 so columns >= 4 are "remote". */
static void test_kp_sec_confirm_ignored_from_the_right_half(void)
{
    reset_kp_state();
    sec_confirm_reset();
    sec_confirm_arm(2, 0);
    chest_gate_publish(1, 3);
    keymaps[0][0][5] = T_K_SEC_CONFIRM;
    press_key(0, 0, 5);
    build_keycode_report();
    TEST_ASSERT_EQ(chest_gate_take_press(), 0, "no chest press from the right half");
    uint8_t slot = 0xFF;
    TEST_ASSERT(sec_confirm_poll(1, &slot) != SEC_CONFIRM_AUTHORIZED, "local gate not authorized either");
    chest_gate_publish(0, 0);
    keymaps[0][0][5] = 0;
}

static void test_kp_chest_next_requests_a_mode_change_once(void)
{
    reset_kp_state();
    (void)chest_gate_take_mode_next();
    keymaps[0][0][0] = T_K_CHEST_NEXT;
    press_key(0, 0, 0);
    build_keycode_report();
    build_keycode_report();
    TEST_ASSERT(chest_gate_take_mode_next(), "a new press requests the next mode");
    TEST_ASSERT(!chest_gate_take_mode_next(), "a held key requests it once");
    TEST_ASSERT_EQ(keycodes[0], 0, "absorbed, not typed");
}

static void test_sec_confirm_from_local(void)
{
    TEST_ASSERT(sec_confirm_from_local(0, 7, 14), "left col 0");
    TEST_ASSERT(sec_confirm_from_local(6, 7, 14), "left col 6");
    TEST_ASSERT(!sec_confirm_from_local(7, 7, 14), "right col 7");
    TEST_ASSERT(!sec_confirm_from_local(13, 7, 14), "right col 13");
    TEST_ASSERT(sec_confirm_from_local(12, 13, 13), "non-split board: every column");
}
```

(`T_K_CHEST_NEXT 0x3E01u` defined next to `T_K_SEC_CONFIRM` in that file; `reset_kp_state` also calls `(void)chest_gate_take_mode_next();`. Register the four tests.) In `test/CMakeLists.txt` add `target_compile_definitions(test_runner PRIVATE SEC_CONFIRM_LOCAL_COLS=4)`. Check first that no existing test places K_SEC_CONFIRM at a column >= 4; if one does, move it to column 0.

- [ ] **Step 2: Run** → RED.

- [ ] **Step 3: Implement.** `chest_gate.c`:

```c
#include "chest_gate.h"
static uint32_t s_pending_tag;   /* CHEST_TAG(op, instance), written by the link task */
static uint32_t s_press_tag;     /* tag stored at press time; 0 = none */
static bool     s_mode_next;

void chest_gate_publish(uint16_t pending_op, uint8_t instance)
{ __atomic_store_n(&s_pending_tag, pending_op ? CHEST_TAG(pending_op, instance) : 0u, __ATOMIC_RELEASE); }
uint16_t chest_gate_pending(void) { return CHEST_TAG_OP(__atomic_load_n(&s_pending_tag, __ATOMIC_ACQUIRE)); }
bool chest_gate_press(void)
{
    uint32_t tag = __atomic_load_n(&s_pending_tag, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s_press_tag, tag, __ATOMIC_RELEASE);
    return CHEST_TAG_OP(tag) != 0;
}
uint32_t chest_gate_take_press(void) { return __atomic_exchange_n(&s_press_tag, 0u, __ATOMIC_ACQ_REL); }
void chest_gate_mode_next(void) { __atomic_store_n(&s_mode_next, true, __ATOMIC_RELEASE); }
bool chest_gate_take_mode_next(void) { return __atomic_exchange_n(&s_mode_next, false, __ATOMIC_ACQ_REL); }
bool sec_confirm_from_local(uint8_t col, uint8_t local_cols, uint8_t keymap_cols)
{ return keymap_cols <= local_cols || col < local_cols; }
```

`key_definitions.h`, under `K_SEC_CONFIRM`: `#define K_CHEST_NEXT 0x3E01  /* cycle the Niphar_chest USB mode (none->storage->pgp->otp->fido->oath) */`.
`docs/KEYCODE_MAP.md`: add after the `0x3E00` row `| \`0x3E01\` | Chest Next | Cycle the Niphar_chest USB mode: none → storage → pgp → otp → fido → oath → none (Niphargus left) | \`K_CHEST_NEXT\` |` and change the block row's "only `0x3E00` is defined today" to "`0x3E00` and `0x3E01` are defined".
`key_processor.c`: near the includes

```c
#ifndef SEC_CONFIRM_LOCAL_COLS
#define SEC_CONFIRM_LOCAL_COLS MATRIX_COLS   /* host tests override it (test/CMakeLists.txt) */
#endif
```

and the branches:

```c
    if (kc == K_SEC_CONFIRM) {
        /* Left half only (Mae, 2026-09-29): a remote column arrived over the
         * unauthenticated inter-half radio — it confirms nothing. Then a chest
         * op pending takes the press; otherwise the local gate. */
        if (is_new_press(row, col) && sec_confirm_from_local(col, SEC_CONFIRM_LOCAL_COLS, KEYMAP_COLS)
            && !chest_gate_press()) sec_confirm_authorize();
        return 0;
    }
    if (kc == K_CHEST_NEXT) { if (is_new_press(row, col)) chest_gate_mode_next(); return 0; }
```

Keep `is_new_press(row, col)` the FIRST operand (the existing mid-hold test pins it). Check that no generic `K_IS_SEC` branch earlier in `process_advanced_key` swallows `0x3E01`.

`COMPORTEMENTS.md`: extend the chest bullet — the press carries (op, instance) [test:test_kp_sec_confirm_records_the_instance]; K_SEC_CONFIRM from the right half is ignored, local gate included [test:test_kp_sec_confirm_ignored_from_the_right_half] [test:test_sec_confirm_from_local]; K_CHEST_NEXT requests the next chest mode once per press [test:test_kp_chest_next_requests_a_mode_change_once].

- [ ] **Step 4: Run** fast check GREEN; the `keycode-map` brick must be green (the new row); prove bites: drop `sec_confirm_from_local(...)` from the branch → RED; move `is_new_press` after it → the mid-hold test RED; revert. Then the full six-board check in the devshell (`nix develop /home/mae/nixos-config#esp-idf -c ./scripts/check.sh --force`) — `chest_gate_publish` changed signature, Task 4 updates `chest_link.c`; if the left does not build because of `chest_link.c`'s old call, update that ONE call to `chest_gate_publish(st.pending_op, st.instance)` / `chest_gate_publish(0, 0)` here (mechanical), and note it in the report.

- [ ] **Step 5: Commit** — `feat(chest): K_CHEST_NEXT cycles the chest mode; the press carries (op, instance); K_SEC_CONFIRM ignored from the right half`.

---

### Task 3: screen — the active mode, pending in lower case, ERR on a persisting refusal

**Files:** `main/display/memlcd/memlcd_model.h`, `main/display/memlcd/memlcd_backend.c`, `test/test_memlcd_model.c`, `COMPORTEMENTS.md`, `docs/HARDWARE_SMOKE_TEST.md`

**Interfaces:** Consumes `chest_mode_label`, `chest_mode_state_t`, `CHEST_MODE_*` (Task 1); `chest_link_view()` new signature from Task 4 is used under `#if CONFIG_KASE_CHEST_LINK` — this task defines the model side only and wires `lire_modele` to the Task 4 signature below.

- [ ] **Step 1: Failing test** in `test/test_memlcd_model.c` — replace the "USB" expectations of `test_lignes_coffre`:

```c
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE };
    char l[3][MEMLCD_COFFRE_BUF];
    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_USB;
    m.coffre_mode_state = CHEST_MODE_ARRIVED; m.coffre_mode_active = CHEST_MODE_PGP; m.coffre_mode_wanted = CHEST_MODE_PGP;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[0], "P4") == 0 && strcmp(l[1], "SD") == 0 && strcmp(l[2], "PGP") == 0, "P4 / SD / PGP");
    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY;
    m.coffre_mode_state = CHEST_MODE_PENDING; m.coffre_mode_active = CHEST_MODE_IN_FLIGHT; m.coffre_mode_wanted = CHEST_MODE_OATH;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[1], "oath") == 0 && l[2][0] == '\0', "no SD: pending mode moves up, lower case");
    m.coffre_mode_state = CHEST_MODE_FAULT;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[1], "ERR") == 0, "persisting refusal");
    m.coffre_mode_state = CHEST_MODE_ARRIVED; m.coffre_mode_active = CHEST_MODE_NONE; m.coffre_mode_wanted = CHEST_MODE_NONE;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(l[1][0] == '\0', "no mode: nothing");
```

and in `test_model_diff`: `b = a; b.coffre_mode_state = CHEST_MODE_FAULT; TEST_ASSERT(memlcd_model_diff(&a, &b), "mode state → redraw");` plus the same for `coffre_mode_active` and `coffre_mode_wanted`.

- [ ] **Step 2: RED.**
- [ ] **Step 3: Implement.** Model fields (before `is_left`): `uint8_t coffre_mode_active, coffre_mode_wanted, coffre_mode_state;` — in the diff. `memlcd_lignes_coffre`: after "P4" and "SD", the mode line from `chest_mode_label(m->coffre_mode_state, m->coffre_mode_active, m->coffre_mode_wanted, …)` if non-empty (replacing the old `CHEST_STATE_USB` → "USB" line). `MEMLCD_COFFRE_BUF` stays 5 (4 + NUL = `CHEST_MODE_LABEL_BUF`; add `_Static_assert(MEMLCD_COFFRE_BUF >= CHEST_MODE_LABEL_BUF, …)`). Backend `lire_modele()` under `CONFIG_KASE_CHEST_LINK`:

```c
    { chest_view_t v; chest_link_view(&v);
      m->coffre = v.bits; m->coffre_op = v.op;
      m->coffre_mode_active = v.mode_active; m->coffre_mode_wanted = v.mode_wanted; m->coffre_mode_state = v.mode_state; }
```

(`chest_view_t` is defined in Task 4's `chest_link.h`; if Task 4 has not landed, this block is compiled out — `CONFIG_KASE_CHEST_LINK` is on for niphar_left, so land Task 3 and Task 4 builds in order: Task 3 must still build the left. Resolve by defining `chest_view_t` and the new `chest_link_view(chest_view_t *)` prototype in `chest_link.h` in THIS task, and adapting `chest_link.c`'s `chest_link_view` to fill `bits`/`op` and zeros for the mode fields; Task 4 fills them for real.)

```c
/* chest_link.h */
typedef struct { uint8_t bits; uint16_t op; uint8_t mode_active, mode_wanted, mode_state; } chest_view_t;
void chest_link_view(chest_view_t *v);
```

`COMPORTEMENTS.md`: the screen bullet — the chest's ACTIVE mode in upper case, the wanted one in lower case while a switch is in flight (0xFF) or not yet taken, `ERR` after 8 reads of two known modes disagreeing [test:test_memlcd_model] [test:test_chest_mode_track]. `docs/HARDWARE_SMOKE_TEST.md` chest item: K_CHEST_NEXT to `pgp` shows `pgp` then `PGP`.

- [ ] **Step 4:** fast GREEN; both halves build (`idf.py` for niphar_left and niphar_right in the devshell).
- [ ] **Step 5: Commit** — `feat(memlcd): the chest line shows the ACTIVE USB mode (upper), a pending one (lower), ERR on a persisting refusal`.

---

### Task 4: `chest_link` v2 transport

**Files:** `main/comm/chest/chest_link.c` (and `chest_link.h` view already reshaped in Task 3), `COMPORTEMENTS.md`

**Interfaces:** Consumes everything above.

- [ ] **Step 1: Implement** (no host test — ESP-IDF glue; the logic it calls is tested):
  - `static uint8_t s_mode_wanted = CHEST_MODE_NONE; static chest_mode_track_t s_mode_track; static uint8_t s_mode_state;`
  - `write_confirm(uint8_t instance)`: `s_tx[0] = CHEST_CONFIRM_MAGIC; s_tx[1] = instance;` WRBUF at `CHEST_REG_USER_CONFIRM`, `.length = 16`.
  - `write_mode(uint8_t mode)`: `s_tx[0] = mode;` WRBUF at `CHEST_REG_MODE_REQ`, `.length = 8`. A separate transaction, never merged with the confirm.
  - In the task, every round: `uint32_t pressed = chest_gate_take_press(); bool next = chest_gate_take_mode_next();` (both drained whatever happens, like the press today).
  - On `CHEST_BLOCK_OK`:
    ```c
    chest_gate_publish(st.pending_op, st.instance);
    if (next && (st.state & CHEST_STATE_READY)) s_mode_wanted = chest_mode_next(s_mode_wanted);
    if (chest_mode_needs_write(s_rx, s_mode_wanted)) write_mode(s_mode_wanted);   /* also the self-heal after a chest reboot */
    s_mode_state = chest_mode_track(&s_mode_track, st.active_mode, s_mode_wanted);
    if (chest_press_matches(pressed, blk, &st)) chest_confirm_request(&s_confirm, &st, now);
    if (chest_confirm_step(&s_confirm, &st, now)) write_confirm(s_confirm.instance);
    s_view = CHEST_VIEW_PRESENT | (st.state & 0x07);  s_view_op = st.pending_op;
    s_view_mode_active = st.active_mode;
    ```
  - Non-OK rounds and `go_absent()`: `chest_gate_publish(0, 0)`; `go_absent()` also resets `s_mode_wanted = CHEST_MODE_NONE`, `s_mode_track`, `s_mode_state`, `s_view_mode_active`.
  - Log once per session when the mode state first becomes `CHEST_MODE_FAULT` (`ESP_LOGW(TAG, "chest refuses mode %u (active %u)", …)`), reset in `go_absent`.
  - `chest_link_view(chest_view_t *v)`: `bits = s_view`, `op = s_view_op`, `mode_active = s_view_mode_active`, `mode_wanted = s_mode_wanted`, `mode_state = s_mode_state`.
  - BAD_VERSION message now reads "chest speaks protocol %u, we speak %u" with `CHEST_PROTO_VERSION` 2 — a v1 chest shows `P4?`.
- [ ] **Step 2:** fast GREEN; full six-board check in the devshell GREEN; `grep -c chest_link build_niphar_left/compile_commands.json` > 0.
- [ ] **Step 3:** `COMPORTEMENTS.md` `[smoke:Chest link]` bullet: v2 — confirm `{0x5A, instance}` in one write at 0x10, mode alone at 0x12 rewritten whenever the read-back differs (chest reboot self-heal, contract §6.2), wanted mode back to none on unplug.
- [ ] **Step 4: Commit** — `feat(chest): protocol v2 on the wire — instance-echoed confirm, mode selection with self-heal, active mode on screen`.

---

### Task 5: bench (controller + Mae; not a subagent)

- [ ] The chest runs `14f9352` (ask the chest session to confirm the running commit first).
- [ ] Flash the left **through its USB-C** (the ESP-Prog is reserved for another project): check the port is the left's CDC before flashing.
- [ ] Screen `P4 SD`; `K_CHEST_NEXT` → `msc` then `MSC` (lsblk shows the card) → `pgp` then `PGP` (`gpg --card-status` sees the card).
- [ ] `echo t | gpg --sign` → `SIGN / OK ?`; K_SEC_CONFIRM on the LEFT signs; no press → 6985; K_SEC_CONFIRM placed on the RIGHT does nothing.
- [ ] Unplug USB → chest lines gone; sleep current unchanged.
- [ ] Memory note `chest-link-s3.md` updated with what the bench proved.
