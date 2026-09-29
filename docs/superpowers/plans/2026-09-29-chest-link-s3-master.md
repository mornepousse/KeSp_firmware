# Chest Link (S3 master) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Niphargus left half reads the Niphar_chest (ESP32-P4) status over the shared SPI bus, shows it on its screen, and relays a real `K_SEC_CONFIRM` press to the chest when an operation is pending.

**Architecture:** Three layers. `chest_proto` (pure, host-tested): register-block parse, op labels, confirm retry rule — pinned against golden vectors generated from the chest's own `link_proto.c`. `chest_gate` (pure, always compiled): the hand-off between the keyboard engine (a press) and the link task (a bus write), so `key_processor` never touches the bus. `chest_link` (firmware, niphar_left only): presence from the sleep task's 1 s USB check, SPI device add/remove on GPIO3, GPIO46 rising-edge IRQ, 250 ms reads under `rf_bus_lock`, the confirm write. The memory-LCD model gains the chest status and the prompt.

**Tech Stack:** ESP-IDF 5.5 (`driver/spi_master.h` half-duplex with command/address/dummy phases, `driver/gpio.h`), FreeRTOS task notifications, LVGL 8 (memlcd backend), host CMake tests (`test/`).

**Spec:** `docs/superpowers/specs/2026-09-29-chest-link-s3-master-design.md`

## Global Constraints

- Code comments, docs and commit messages in English; log strings may stay French (CLAUDE.md "Language").
- One chip, one owner: every SPI transaction to the chest goes through `rf_bus_lock()` / `rf_bus_unlock()` (`main/comm/rf/rf_bus.h`); never call them from the radio TX path or an ISR.
- GPIO3 is driven ONLY while the chest is present (R48 10k pulls CS to the chest's rail — driving it high into a dead rail costs ~0.33 mA). Absent → SPI device removed, GPIO3 input, no pull.
- GPIO46: input, no internal pull (R49 10k is the pull-down), rising-edge interrupt, enabled only while present.
- `spi_slave_hd` protocol: WRBUF `0x01`, RDBUF `0x02`, 8-bit command, 8-bit address, 8 dummy cycles; SPI mode 0; 1 MHz.
- Register map (chest `main/link/link_proto.h`, protocol version 1): magic `NIPH` at 0x00, version 0x04, state 0x05 (bit0 SD, bit1 USB mounted, bit2 READY), pending op 0x06-0x07 LE, confirm count 0x08-0x0B LE, CRC16 (`cr_crc16`) over 0x00..0x0B at 0x0C-0x0D LE, user confirm at 0x10 (write `0x5A`), block size 0x14.
- Security invariant: `chest_gate_press()` is called ONLY from `main/input/key_processor.c` (and tests). Never from CDC.
- No new poller on battery: the link task blocks forever while the chest is absent (cadence rule, `main/power/cadence.h`).
- TDD: every pure function gets its host test first (red, then green); `./scripts/check.sh --fast` green at the end of every task; one commit per task; never push without Mae's "push".
- A source change comes with a test or a `COMPORTEMENTS.md` line (behaviour contract).

## Review Focus

- A chest that is powered but booting or hung returns a non-uniform, non-`NIPH` block: no prompt, no confirm, status `P4?`/nothing — pinned in Task 2 (`corrupt block is not a chest`).
- `K_SEC_CONFIRM` pressed with no chest operation pending must keep its old meaning (the local `sec_confirm` gate) — pinned in Task 3.
- The operation changes between the press and the write (op A times out, op B arms): the press must NOT authorize B — pinned in Task 2 (`op changed → no write`).
- A held `K_SEC_CONFIRM` confirms once, not on every scan — pinned in Task 3.
- USB unplugged while a prompt is shown: prompt gone, GPIO3 released — pinned by the `chest_link_view` contract in Task 5 (absent → 0) and the bench item.

---

### Task 1: Pinout — GPIO3/GPIO46 are the chest's, not "unwired"

**Files:**
- Modify: `boards/niphar_left/board.h` (after `BOARD_LCD_CS_ACTIVE_HIGH`, ~line 137; `BOARD_PINS` list ~line 175)
- Modify: `test/board_contract.inc:7-36` (strapping rule)
- Modify: `test/test_niphar_left_pins.c:64-70` and add a test
- Modify: `docs/NIPHARGUS_V2_HARDWARE.md` ("Forbidden" row, P4 section)
- Modify: `COMPORTEMENTS.md`

**Interfaces:**
- Produces: `BOARD_CHEST_CS` (= `GPIO_NUM_3`), `BOARD_CHEST_IRQ` (= `GPIO_NUM_46`), `BOARD_PINS_STRAPPING_WIRED(g)` (predicate macro).

- [ ] **Step 1: Write the failing test** — append to `test/test_niphar_left_pins.c` before the suite function, and register it with `TEST_RUN` in that file's suite:

```c
/* The chest link, from the netlist (kicad-cli export, 2026-09-29):
 * CS_P4 = U6 pin 15 = GPIO3 (R48 10k to the chest's P4_3V3 rail),
 * IRQ_P4 = U6 pin 16 = GPIO46 (R49 10k to GND). Both are S3 strapping pins,
 * wired on purpose by the PCB: the matrix must never land on them. */
static void test_left_chest_link_pins(void)
{
    TEST_ASSERT_EQ(BOARD_CHEST_CS, 3, "CS_P4 = GPIO3");
    TEST_ASSERT_EQ(BOARD_CHEST_IRQ, 46, "IRQ_P4 = GPIO46");
    const int matrix[] = { ROWS0, ROWS1, ROWS2, ROWS3,
                           COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6 };
    for (unsigned i = 0; i < sizeof matrix / sizeof matrix[0]; i++) {
        TEST_ASSERT(matrix[i] != BOARD_CHEST_CS, "no matrix line on the chest CS");
        TEST_ASSERT(matrix[i] != BOARD_CHEST_IRQ, "no matrix line on the chest IRQ");
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `./scripts/check.sh --fast`
Expected: RED — `BOARD_CHEST_CS` undeclared (detail in `.git/tripwire/last-fail.log`).

- [ ] **Step 3: Implement.** In `boards/niphar_left/board.h`, after the LCD CS lines:

```c
/* ── Chest link (Niphar_chest, ESP32-P4 as spi_slave_hd) ──
 * From the netlist (kicad-cli export of niphar.kicad_sch, 2026-09-29):
 * CS_P4 = U6 pin 15 = GPIO3, R48 10k pull-up to the CHEST's P4_3V3 rail —
 * so the S3 drives it only while the chest is powered (USB); IRQ_P4 = U6
 * pin 16 = GPIO46, R49 10k pull-down, active high, input-only pin.
 * Both are strapping pins wired on purpose: GPIO3 (JTAG source, ignored
 * without EFUSE_STRAP_JTAG_SEL) and GPIO46 (ROM print, "Ignored" with the
 * default eFuse, TRM v1.8 table 8.3-1 p. 536). */
#define BOARD_CHEST_CS   GPIO_NUM_3
#define BOARD_CHEST_IRQ  GPIO_NUM_46
#define BOARD_PINS_STRAPPING_WIRED(g) ((g) == 3 || (g) == 46)
```

Add `X(BOARD_CHEST_CS) X(BOARD_CHEST_IRQ)` at the end of `BOARD_PINS(X)`.

In `test/board_contract.inc`, make `bc_no_strapping` exempt the pins the board declares wired:

```c
static void bc_no_strapping(void)
{
#ifdef BOARD_PINS_WAIVE_STRAPPING
    TEST_ASSERT(1, BOARD_CONTRACT_NAME ": strapping rule waived by the board (legacy pinout)");
#else
    for (unsigned i = 0; i < bc_n; i++) {
#ifdef BOARD_PINS_STRAPPING_WIRED
        if (BOARD_PINS_STRAPPING_WIRED(bc_pins[i])) continue;   /* wired by the PCB, justified in board.h */
#endif
        TEST_ASSERT(!bc_strapping(bc_pins[i]), BOARD_CONTRACT_NAME ": strapping GPIO (0/3/45/46) in use");
    }
#endif
}
```

(Keep the existing loop variable names of the file — read lines 20-40 first and adapt `bc_n`/`bc_pins` to what is there.)

In `test/test_niphar_left_pins.c`, rename the concept: `is_forbidden` keeps 45/35/36/37 as unwired and treats 3/46 as "chest link, not for the matrix":

```c
/* Unwired GPIOs: strapping and octal PSRAM. GPIO3 and GPIO46 are strapping
 * pins too but WIRED to the chest (CS_P4, IRQ_P4 — test_left_chest_link_pins),
 * so they stay out of the matrix all the same. */
static int is_forbidden(int gpio)
{
    return gpio == 3 || gpio == 45 || gpio == 46 ||
           gpio == 35 || gpio == 36 || gpio == 37;
}
```

(Behaviour unchanged for the matrix list; `test_left_no_forbidden_gpio` must not include the two chest pins in its `pins[]`.)

In `docs/NIPHARGUS_V2_HARDWARE.md`: the "Forbidden" row becomes `| Forbidden | 45, 35-37 | strapping / octal PSRAM — not wired |`, add a row `| Chest link CS_P4 / IRQ_P4 (left) | 3 / 46 | CS: R48 10k pull-up to the CHEST's rail — drive only while the chest is powered; IRQ: R49 10k pull-down, active high. Strapping pins wired on purpose (netlist 2026-09-29) |`, and in the P4 section replace "Three slaves on one bus, each with its own CS" consequence text with the actual CS (GPIO3).

In `COMPORTEMENTS.md`, in the Niphargus hardware/pins area add:

```
- [test:test_left_chest_link_pins] The chest link uses GPIO3 (CS_P4, R48 to
  the chest's rail) and GPIO46 (IRQ_P4, R49 pull-down) on the left — from
  the netlist, 2026-09-29; the matrix never lands on them, and the generic
  board contract exempts exactly these two strapping pins
  (BOARD_PINS_STRAPPING_WIRED). The chest's own HARDWARE.md said IO7/IO11:
  those are the P4-side numbers.
```

- [ ] **Step 4: Run to verify it passes**

Run: `./scripts/check.sh --fast`
Expected: GREEN.

- [ ] **Step 5: Commit**

```bash
git add boards/niphar_left/board.h test/board_contract.inc test/test_niphar_left_pins.c docs/NIPHARGUS_V2_HARDWARE.md COMPORTEMENTS.md .tripwire-testcount
git commit -m "boards(niphar_left): the chest link pins from the netlist — CS_P4 GPIO3 (R48 to the chest's rail), IRQ_P4 GPIO46; 3 and 46 were never 'unwired'"
```

---

### Task 2: `chest_proto` — parse, labels, confirm rule, pinned to the chest's vectors

**Files:**
- Create: `main/comm/chest/chest_proto.h`, `main/comm/chest/chest_proto.c`
- Create: `scripts/gen/chest_link_vectors.c`, `scripts/gen_chest_link_vectors.sh`
- Create: `docs/contracts/chest_link_vectors.json` (generated)
- Create: `test/test_chest_proto.c`
- Modify: `test/CMakeLists.txt` (sources + include dir), `test/test_main.c` (declare + call)

**Interfaces:**
- Consumes: `cr_crc16()` from `main/security/cr_crc16.h` (`uint16_t cr_crc16(const uint8_t *data, uint16_t len)`).
- Produces (`chest_proto.h`):

```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Niphar_chest link, protocol version 1 — the chest's main/link/link_proto.h.
 * Re-implemented here, pinned byte for byte by test_chest_proto against
 * docs/contracts/chest_link_vectors.json (generated from the chest's code). */
#define CHEST_PROTO_VERSION     1
#define CHEST_REG_SIZE          0x14
#define CHEST_REG_CHEST_LEN     0x10
#define CHEST_REG_USER_CONFIRM  0x10
#define CHEST_CONFIRM_MAGIC     0x5A
#define CHEST_STATE_SD          0x01
#define CHEST_STATE_USB         0x02
#define CHEST_STATE_READY       0x04
#define CHEST_CMD_WRBUF         0x01   /* esp_spi_slave_protocol.rst, Supported Commands */
#define CHEST_CMD_RDBUF         0x02
#define CHEST_CONFIRM_RETRY_MS  200u   /* ten chest ticks (LINK_TICK_MS 20) */
#define CHEST_LABEL_BUF         8

typedef struct {
    uint8_t  version, state;
    uint16_t pending_op;
    uint32_t confirm_count;
} chest_status_t;

typedef enum {
    CHEST_BLOCK_OK = 0,
    CHEST_BLOCK_ABSENT,        /* uniform 0x00 / 0xFF: no chest — the ordinary case */
    CHEST_BLOCK_BAD_VERSION,   /* magic ok, version != 1: not half-understood */
    CHEST_BLOCK_CORRUPT,       /* anything else: magic, CRC, short */
} chest_block_t;

bool          chest_proto_is_absent(const uint8_t *regs, size_t len);
/* `out` is written only on CHEST_BLOCK_OK. */
chest_block_t chest_proto_parse(const uint8_t *regs, size_t len, chest_status_t *out);
/* Label of a chest sec_op_t code, 6 characters max (spec §6). */
void          chest_op_label(uint16_t op, char out[CHEST_LABEL_BUF]);

/* Confirm delivery: one write, one retry after CHEST_CONFIRM_RETRY_MS if the
 * chest's counter has not moved and the same op is still pending, never a
 * third. */
typedef struct { bool armed; uint8_t writes; uint16_t op; uint32_t count0, t_ms; } chest_confirm_t;
/* A real press reached the link task. False (dropped) when nothing is pending. */
bool chest_confirm_request(chest_confirm_t *c, uint16_t pending_op, uint32_t count, uint32_t now_ms);
/* One task round. True = write CHEST_CONFIRM_MAGIC now. */
bool chest_confirm_step(chest_confirm_t *c, uint16_t pending_op, uint32_t count, uint32_t now_ms);
```

- [ ] **Step 1: Write the generator** (`scripts/gen/chest_link_vectors.c`) — compiled against the CHEST's sources:

```c
/* Golden register blocks from Niphar_chest's own link_proto.c (host build).
 * Run scripts/gen_chest_link_vectors.sh; test/test_chest_proto.c holds the
 * same bytes. */
#include <stdio.h>
#include <string.h>
#include "link/link_proto.h"
static void block(const char *name, uint8_t st, uint16_t op, uint32_t count, int last)
{
    uint8_t r[LINK_REG_SIZE];
    memset(r, 0, sizeof r);
    link_status_t s = { .version = LINK_PROTO_VERSION, .state = st, .pending_op = op, .confirm_count = count };
    link_proto_pack_status(r, &s);
    printf("    \"%s\": \"", name);
    for (unsigned i = 0; i < sizeof r; i++) printf("%s%02x", i ? " " : "", r[i]);
    printf("\"%s\n", last ? "" : ",");
}
int main(void)
{
    printf("{\n  \"comment\": \"Register blocks packed by Niphar_chest main/link/link_proto.c (host build), 20 bytes, offset order. Master range 0x10-0x13 zero.\",\n");
    printf("  \"protocol_version\": %d,\n  \"blocks\": {\n", LINK_PROTO_VERSION);
    block("ready_sd_idle",          0x05, 0,  0,          0);
    block("ready_all_pending_sign", 0x07, 1,  3,          0);
    block("booting",                0x00, 0,  0,          0);
    block("usb_pending_reset_big",  0x06, 10, 0x12345678, 1);
    printf("  }\n}\n");
    return 0;
}
```

`scripts/gen_chest_link_vectors.sh`:

```bash
#!/usr/bin/env bash
# Regenerates docs/contracts/chest_link_vectors.json from the CHEST's
# link_proto.c (Niphar_chest, sibling checkout). Host compiler only.
set -euo pipefail
cd "$(dirname "$0")/.."
CHEST="${1:-$HOME/Documents/GitHub/Niphar_chest}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
cc -std=c11 -I "$CHEST/main" -I "$CHEST/main/sys" scripts/gen/chest_link_vectors.c \
   "$CHEST/main/link/link_proto.c" "$CHEST/main/sys/cr_crc16.c" -o "$tmp/gen"
"$tmp/gen" | python3 -m json.tool --indent 2 > docs/contracts/chest_link_vectors.json
echo "wrote docs/contracts/chest_link_vectors.json (chest at $(git -C "$CHEST" rev-parse --short HEAD))"
```

Run: `chmod +x scripts/gen_chest_link_vectors.sh && ./scripts/gen_chest_link_vectors.sh`
Expected: `wrote docs/contracts/chest_link_vectors.json (chest at 21d0dba)` and four 20-byte hex strings in the JSON, each starting `4e 49 50 48 01`.

- [ ] **Step 2: Write the failing test** `test/test_chest_proto.c`. Paste the four hex strings from the JSON into the four arrays marked `/* from JSON */` (these are generator output, not hand-computed):

```c
/* The chest link, byte for byte — Niphar_chest main/link/link_proto.h v1.
 * The four blocks come from docs/contracts/chest_link_vectors.json, packed
 * by the CHEST's code (scripts/gen_chest_link_vectors.sh): a change on
 * either side that the other did not follow breaks here, not on the bench. */
#include "test_framework.h"
#include "../main/comm/chest/chest_proto.h"

static const uint8_t v_ready_sd_idle[20]     = { /* from JSON "ready_sd_idle" */ };
static const uint8_t v_pending_sign[20]      = { /* from JSON "ready_all_pending_sign" */ };
static const uint8_t v_booting[20]           = { /* from JSON "booting" */ };
static const uint8_t v_pending_reset_big[20] = { /* from JSON "usb_pending_reset_big" */ };

static void test_chest_parse_vectors(void)
{
    chest_status_t s;
    TEST_ASSERT_EQ(chest_proto_parse(v_ready_sd_idle, 20, &s), CHEST_BLOCK_OK, "ready+sd parses");
    TEST_ASSERT_EQ(s.state, 0x05, "state SD|READY");
    TEST_ASSERT_EQ(s.pending_op, 0, "nothing pending");
    TEST_ASSERT_EQ(chest_proto_parse(v_pending_sign, 20, &s), CHEST_BLOCK_OK, "pending sign parses");
    TEST_ASSERT_EQ(s.pending_op, 1, "SIGN pending");
    TEST_ASSERT_EQ(s.confirm_count, 3, "count 3");
    TEST_ASSERT_EQ(chest_proto_parse(v_booting, 20, &s), CHEST_BLOCK_OK, "booting chest is a chest");
    TEST_ASSERT_EQ(s.state & CHEST_STATE_READY, 0, "not READY while booting");
    TEST_ASSERT_EQ(chest_proto_parse(v_pending_reset_big, 20, &s), CHEST_BLOCK_OK, "big count parses");
    TEST_ASSERT_EQ(s.pending_op, 10, "OATH_RESET pending");
    TEST_ASSERT_EQ(s.confirm_count, 0x12345678u, "count little-endian");
}

static void test_chest_absent_and_corrupt(void)
{
    uint8_t b[20]; chest_status_t s = { .pending_op = 0xBEEF };
    memset(b, 0x00, 20);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_ABSENT, "all 0x00 = no chest");
    memset(b, 0xFF, 20);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_ABSENT, "all 0xFF = no chest");
    memcpy(b, v_pending_sign, 20); b[0x0C] ^= 0x01;
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "bad CRC");
    memcpy(b, v_pending_sign, 20); b[0] = 'X';
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "bad magic");
    memcpy(b, v_pending_sign, 20); b[4] = 2;
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_BAD_VERSION, "version 2 is not half-understood");
    TEST_ASSERT_EQ(chest_proto_parse(v_pending_sign, 19, &s), CHEST_BLOCK_CORRUPT, "short read");
    /* Review focus: a booting/hung chest returns noise — not a chest, nothing acted on. */
    for (int i = 0; i < 20; i++) b[i] = (uint8_t)(0x31 * i + 7);
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_CORRUPT, "noise is not a chest");
    TEST_ASSERT_EQ(s.pending_op, 0xBEEF, "out untouched on any failure");
    /* The master's byte is outside the CRC: a pending 0x5A does not invalidate the block. */
    memcpy(b, v_pending_sign, 20); b[CHEST_REG_USER_CONFIRM] = CHEST_CONFIRM_MAGIC;
    TEST_ASSERT_EQ(chest_proto_parse(b, 20, &s), CHEST_BLOCK_OK, "master range not covered by the CRC");
}

static void test_chest_op_labels(void)
{
    char l[CHEST_LABEL_BUF];
    static const char *exp[] = { "", "SIGN", "DECRYP", "AUTH", "OTP", "FIDO +", "FIDO",
                                 "TOTP", "DELETE", "REPLAC", "RESET!" };
    for (uint16_t op = 1; op <= 10; op++) {
        chest_op_label(op, l);
        TEST_ASSERT(strcmp(l, exp[op]) == 0, "label of each chest sec_op_t code");
        TEST_ASSERT(strlen(l) <= 6, "6 characters max");
    }
    chest_op_label(42, l);
    TEST_ASSERT(strcmp(l, "OP 42") == 0, "unknown op shows its code");
}

static void test_chest_confirm_rule(void)
{
    chest_confirm_t c = {0};
    TEST_ASSERT(!chest_confirm_request(&c, 0, 3, 1000), "nothing pending: press dropped");
    TEST_ASSERT(!chest_confirm_step(&c, 0, 3, 1000), "and nothing written");

    TEST_ASSERT(chest_confirm_request(&c, 1, 3, 1000), "pending SIGN: accepted");
    TEST_ASSERT(chest_confirm_step(&c, 1, 3, 1000), "first write at once");
    TEST_ASSERT(!chest_confirm_step(&c, 1, 3, 1100), "no retry before 200 ms");
    TEST_ASSERT(chest_confirm_step(&c, 1, 3, 1200), "one retry after 200 ms, count unchanged");
    TEST_ASSERT(!chest_confirm_step(&c, 1, 3, 1400), "never a third write");
    TEST_ASSERT(!c.armed, "given up after the retry");

    chest_confirm_request(&c, 1, 3, 2000);
    TEST_ASSERT(chest_confirm_step(&c, 1, 3, 2000), "write");
    TEST_ASSERT(!chest_confirm_step(&c, 1, 4, 2050), "count moved: delivered, done");
    TEST_ASSERT(!c.armed, "disarmed on delivery");

    /* Review focus: op A timed out, op B armed before the retry — B must not be authorized. */
    chest_confirm_request(&c, 1, 3, 3000);
    TEST_ASSERT(chest_confirm_step(&c, 1, 3, 3000), "write for SIGN");
    TEST_ASSERT(!chest_confirm_step(&c, 2, 3, 3250), "op changed: no retry for DECRYPT");
    TEST_ASSERT(!c.armed, "disarmed on op change");
}

void test_chest_proto(void)
{
    TEST_SUITE("chest link protocol (S3 master)");
    TEST_RUN(test_chest_parse_vectors);
    TEST_RUN(test_chest_absent_and_corrupt);
    TEST_RUN(test_chest_op_labels);
    TEST_RUN(test_chest_confirm_rule);
}
```

In `test/CMakeLists.txt` add `test_chest_proto.c` and `../main/comm/chest/chest_proto.c` to `add_executable(test_runner ...)` and `${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/chest` to the include dirs. In `test/test_main.c` add `extern void test_chest_proto(void);` with the other externs and `test_chest_proto();` in the run list.

- [ ] **Step 3: Run to verify it fails**

Run: `./scripts/check.sh --fast`
Expected: RED — link error / missing `chest_proto.c`.

- [ ] **Step 4: Implement `main/comm/chest/chest_proto.c`**

```c
/* Niphar_chest link, S3 side — pure logic (spec 2026-09-29 §4, §6, §7). */
#include "chest_proto.h"
#include <stdio.h>
#include <string.h>
#include "cr_crc16.h"

static const uint8_t k_magic[4] = { 'N', 'I', 'P', 'H' };
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool chest_proto_is_absent(const uint8_t *regs, size_t len)
{
    if (!regs || len == 0) return true;
    if (regs[0] != 0x00 && regs[0] != 0xFF) return false;
    for (size_t i = 1; i < len; i++) if (regs[i] != regs[0]) return false;
    return true;
}

chest_block_t chest_proto_parse(const uint8_t *regs, size_t len, chest_status_t *out)
{
    if (!regs || len < CHEST_REG_SIZE) return CHEST_BLOCK_CORRUPT;
    if (chest_proto_is_absent(regs, CHEST_REG_SIZE)) return CHEST_BLOCK_ABSENT;
    if (memcmp(regs, k_magic, 4) != 0) return CHEST_BLOCK_CORRUPT;
    if (regs[0x04] != CHEST_PROTO_VERSION) return CHEST_BLOCK_BAD_VERSION;
    if (get_u16(&regs[0x0C]) != cr_crc16(regs, 0x0C)) return CHEST_BLOCK_CORRUPT;
    if (out) {
        out->version = regs[0x04];
        out->state = regs[0x05];
        out->pending_op = get_u16(&regs[0x06]);
        out->confirm_count = get_u32(&regs[0x08]);
    }
    return CHEST_BLOCK_OK;
}

void chest_op_label(uint16_t op, char out[CHEST_LABEL_BUF])
{
    /* The chest's sec_op_t codes, pinned by value: the enum lives in the other
     * repository (Niphar_chest main/security/sec_confirm.h). */
    static const char *const k[] = { "", "SIGN", "DECRYP", "AUTH", "OTP", "FIDO +", "FIDO",
                                     "TOTP", "DELETE", "REPLAC", "RESET!" };
    if (op < sizeof k / sizeof k[0]) snprintf(out, CHEST_LABEL_BUF, "%s", k[op]);
    else snprintf(out, CHEST_LABEL_BUF, "OP %u", (unsigned)(op % 1000u));
}

bool chest_confirm_request(chest_confirm_t *c, uint16_t pending_op, uint32_t count, uint32_t now_ms)
{
    if (pending_op == 0) return false;
    c->armed = true; c->writes = 0; c->op = pending_op; c->count0 = count; c->t_ms = now_ms;
    return true;
}

bool chest_confirm_step(chest_confirm_t *c, uint16_t pending_op, uint32_t count, uint32_t now_ms)
{
    if (!c->armed) return false;
    if (count != c->count0 || pending_op != c->op) { c->armed = false; return false; }
    if (c->writes == 0) { c->writes = 1; c->t_ms = now_ms; return true; }
    if ((uint32_t)(now_ms - c->t_ms) < CHEST_CONFIRM_RETRY_MS) return false;
    if (c->writes == 1) { c->writes = 2; c->t_ms = now_ms; return true; }
    c->armed = false;
    return false;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `./scripts/check.sh --fast`
Expected: GREEN. Then prove the vectors bite: flip one byte in `v_pending_sign`, run again → RED on "pending sign parses"; revert → GREEN.

- [ ] **Step 6: Commit**

```bash
git add main/comm/chest/chest_proto.[ch] scripts/gen/chest_link_vectors.c scripts/gen_chest_link_vectors.sh docs/contracts/chest_link_vectors.json test/test_chest_proto.c test/CMakeLists.txt test/test_main.c .tripwire-testcount
git commit -m "feat(chest): pure link protocol, S3 side — parse, op labels, confirm retry rule, pinned to golden blocks packed by the chest's own link_proto.c"
```

---

### Task 3: `chest_gate` — the press reaches the chest only from a real key

**Files:**
- Create: `main/comm/chest/chest_gate.h`, `main/comm/chest/chest_gate.c`
- Create: `scripts/tripwire.d/chest-confirm.sh`
- Modify: `main/input/key_processor.c:282` (the `K_SEC_CONFIRM` branch) and its includes
- Modify: `main/CMakeLists.txt` (base source list, all roles: `comm/chest/chest_gate.c`, `comm/chest/chest_proto.c`, and `security/cr_crc16.c` moved out of the dongle block; add `comm/chest` to include dirs)
- Modify: `test/test_keycode_report.c` (reset + two tests), `test/CMakeLists.txt` (`../main/comm/chest/chest_gate.c`)
- Modify: `COMPORTEMENTS.md`

**Interfaces:**
- Produces (`chest_gate.h`):

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Hand-off between the keyboard engine and the chest link task.
 * The link task publishes the chest's pending op; key_processor asks whether
 * a K_SEC_CONFIRM press is for the chest. Lock-free (one writer per field,
 * __atomic builtins), compiled on every keyboard board — without a chest,
 * nothing is ever published and every press stays local. */
void     chest_gate_publish(uint16_t pending_op);  /* link task only; 0 = none */
uint16_t chest_gate_pending(void);
/* SECURITY: called ONLY by key_processor.c on a new physical press
 * (scripts/tripwire.d/chest-confirm.sh). True = the press was for the chest. */
bool     chest_gate_press(void);
bool     chest_gate_take_press(void);              /* link task: consume one press */
```

- [ ] **Step 1: Write the failing tests** in `test/test_keycode_report.c`: add `#include "chest_gate.h"`, call `chest_gate_publish(0); (void)chest_gate_take_press();` in `reset_kp_state()`, then add and register:

```c
/* A chest operation pending: the press goes to the chest, not the local gate. */
static void test_kp_sec_confirm_routes_to_chest(void)
{
    reset_kp_state();
    sec_confirm_reset();
    sec_confirm_arm(2, 0);                 /* a local request exists too */
    chest_gate_publish(1);                 /* chest: SIGN pending */
    keymaps[0][0][0] = T_K_SEC_CONFIRM;
    press_key(0, 0, 0);
    build_keycode_report();
    TEST_ASSERT(chest_gate_take_press(), "press queued for the chest");
    TEST_ASSERT(!chest_gate_take_press(), "exactly one press");
    uint8_t slot = 0xFF;
    TEST_ASSERT(sec_confirm_poll(1, &slot) != SEC_CONFIRM_AUTHORIZED, "local gate NOT authorized");
    TEST_ASSERT_EQ(keycodes[0], 0, "absorbed");
    chest_gate_publish(0);
}

/* Held key: one press, not one per scan (review focus). */
static void test_kp_sec_confirm_held_confirms_chest_once(void)
{
    reset_kp_state();
    chest_gate_publish(1);
    keymaps[0][0][0] = T_K_SEC_CONFIRM;
    press_key(0, 0, 0);
    build_keycode_report();
    build_keycode_report();
    build_keycode_report();
    TEST_ASSERT(chest_gate_take_press(), "first cycle queues");
    TEST_ASSERT(!chest_gate_take_press(), "held key does not queue again");
    chest_gate_publish(0);
}
```

The existing `test_kp_sec_confirm_authorizes` stays: with nothing published it must still authorize locally (review focus).

Add `../main/comm/chest/chest_gate.c` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run to verify it fails**

Run: `./scripts/check.sh --fast`
Expected: RED — `chest_gate.h` missing.

- [ ] **Step 3: Implement** `main/comm/chest/chest_gate.c`:

```c
/* See chest_gate.h. */
#include "chest_gate.h"

static uint16_t s_pending;   /* written by the link task */
static bool     s_press;     /* set by key_processor, taken by the link task */

void chest_gate_publish(uint16_t pending_op) { __atomic_store_n(&s_pending, pending_op, __ATOMIC_RELEASE); }
uint16_t chest_gate_pending(void) { return __atomic_load_n(&s_pending, __ATOMIC_ACQUIRE); }

bool chest_gate_press(void)
{
    if (chest_gate_pending() == 0) return false;
    __atomic_store_n(&s_press, true, __ATOMIC_RELEASE);
    return true;
}

bool chest_gate_take_press(void) { return __atomic_exchange_n(&s_press, false, __ATOMIC_ACQ_REL); }
```

`main/input/key_processor.c`: add `#include "chest_gate.h"` with the other includes and change the branch to:

```c
    if (kc == K_SEC_CONFIRM) {
        /* A chest operation pending takes the press (spec 2026-09-29 §5);
         * otherwise the local gate, as before. One press, one destination. */
        if (is_new_press(row, col) && !chest_gate_press()) sec_confirm_authorize();
        return 0;
    }
```

`main/CMakeLists.txt`: `key_processor.c` is compiled for EVERY role (the dongle runs the fused engine), and the screen backend calls `chest_op_label()` unconditionally — so the gate and the pure protocol go in the base list, next to `"security/sec_confirm.c"` (lines 23-24):

```cmake
    # Security confirmation gate — compiled for all roles (key_processor calls it)
    "security/sec_confirm.c"
    # Chest link, pure half: key→link hand-off and the register protocol
    # (spec 2026-09-29). All roles: key_processor calls chest_gate_press().
    "comm/chest/chest_gate.c"
    "comm/chest/chest_proto.c"
    "security/cr_crc16.c"
```

and REMOVE `"security/cr_crc16.c"` from the `CONFIG_KASE_DEVICE_ROLE_DONGLE` block (line ~156) — listed twice, CMake would compile it twice and the link would fail on a duplicate symbol. Add `"comm/chest"` to `INCLUDE_DIRS` (next to `"comm/link"`).

`scripts/tripwire.d/chest-confirm.sh`:

```bash
#!/usr/bin/env bash
# KaSe brick: the chest confirm invariant (spec 2026-09-29 §5).
# A confirmation reaches the chest only from a real key press: chest_gate_press()
# may be called from key_processor.c alone. Sourced by scripts/check.sh.
check_chest_confirm_invariant() {
  local bad
  bad="$(grep -rl --include='*.c' --include='*.h' 'chest_gate_press' main 2>/dev/null \
         | grep -v -e '^main/input/key_processor.c$' -e '^main/comm/chest/chest_gate\.[ch]$' || true)"
  if [ -n "$bad" ]; then
    fail "chest confirm invariant: chest_gate_press() called outside key_processor.c — $bad"
    return 1
  fi
}
TW_PRE_FAST+=(check_chest_confirm_invariant)
```

Prove the rule bites: add a line `/* chest_gate_press */` to `main/comm/cdc/cdc_binary_cmds.c`, run `./scripts/check.sh --fast --force` → RED naming that file; remove it → GREEN.

`COMPORTEMENTS.md` (security area):

```
- [test:test_kp_sec_confirm_routes_to_chest] K_SEC_CONFIRM goes to the chest
  while it has an operation pending, to the local gate otherwise — one press,
  one destination; a held key confirms once
  [test:test_kp_sec_confirm_held_confirms_chest_once]. chest_gate_press() is
  called from key_processor.c only — never from CDC; enforced by
  scripts/tripwire.d/chest-confirm.sh (proven biting).
```

- [ ] **Step 4: Run to verify it passes**

Run: `./scripts/check.sh --fast`
Expected: GREEN.

- [ ] **Step 5: Commit**

```bash
git add main/comm/chest/chest_gate.[ch] main/input/key_processor.c main/CMakeLists.txt scripts/tripwire.d/chest-confirm.sh test/test_keycode_report.c test/CMakeLists.txt COMPORTEMENTS.md .tripwire-testcount
git commit -m "feat(chest): K_SEC_CONFIRM goes to the chest while it has an operation pending — lock-free gate, key_processor the only caller (tripwire rule)"
```

---

### Task 4: Screen — chest status under the logo, prompt in the bottom area

**Files:**
- Modify: `main/display/memlcd/memlcd_model.h` (model fields, `memlcd_lignes_coffre`, diff)
- Modify: `test/test_memlcd_model.c`, `test/CMakeLists.txt` (memlcd test needs `chest_proto.c` — already added in Task 2)
- Modify: `main/display/memlcd/memlcd_backend.c` (`construire`, `lire_modele`, `dessiner`)
- Modify: `COMPORTEMENTS.md`, `docs/HARDWARE_SMOKE_TEST.md`

**Interfaces:**
- Consumes: `chest_op_label()` (Task 2); `chest_link_view()` (Task 5 — guarded by `CONFIG_KASE_CHEST_LINK`, so this task builds before Task 5 exists).
- Produces: model fields `uint8_t coffre; uint16_t coffre_op;` with `MEMLCD_COFFRE_PRESENT 0x80`, `MEMLCD_COFFRE_BADVER 0x40`, low bits = `CHEST_STATE_*`; `void memlcd_lignes_coffre(const memlcd_model_t *m, char l[3][MEMLCD_COFFRE_BUF])` with `MEMLCD_COFFRE_BUF 5`.

- [ ] **Step 1: Write the failing test** — add to `test/test_memlcd_model.c` and register:

```c
/* Chest status, 3 lines of 4 UNSCII characters under the logo (spec §6). */
static void test_lignes_coffre(void)
{
    char l[3][MEMLCD_COFFRE_BUF];
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE };
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(!l[0][0] && !l[1][0] && !l[2][0], "absent: nothing");
    m.coffre = MEMLCD_COFFRE_PRESENT;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[0], "P4..") == 0 && !l[1][0], "present, booting");
    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_USB;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[0], "P4") == 0 && strcmp(l[1], "SD") == 0 && strcmp(l[2], "USB") == 0, "P4 / SD / USB");
    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_USB;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[1], "USB") == 0 && !l[2][0], "no SD: USB moves up");
    m.coffre = MEMLCD_COFFRE_PRESENT | MEMLCD_COFFRE_BADVER;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[0], "P4?") == 0 && !l[1][0], "unknown protocol version");
}
```

and in `test_model_diff` add:

```c
    b = a; b.coffre = MEMLCD_COFFRE_PRESENT; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest appears → redraw");
    b = a; b.coffre_op = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest prompt → redraw");
```

- [ ] **Step 2: Run to verify it fails** — `./scripts/check.sh --fast` → RED (`MEMLCD_COFFRE_BUF` undeclared).

- [ ] **Step 3: Implement the model** in `memlcd_model.h`: `#include "../../comm/chest/chest_proto.h"` at the top; add to `memlcd_model_t` (before `is_left`):

```c
    uint8_t  coffre;                   /* MEMLCD_COFFRE_* | CHEST_STATE_*; 0 = no chest */
    uint16_t coffre_op;                /* chest op awaiting confirmation, 0 = none */
```

then:

```c
#define MEMLCD_COFFRE_PRESENT 0x80
#define MEMLCD_COFFRE_BADVER  0x40
#define MEMLCD_COFFRE_BUF     5        /* 4 UNSCII 8 characters in the 35 px zone + NUL */

/* Chest status under the logo: "P4" ready ("P4.." booting, "P4?" unknown
 * protocol version), then "SD", then "USB", lines packed upwards. */
static inline void memlcd_lignes_coffre(const memlcd_model_t *m, char l[3][MEMLCD_COFFRE_BUF])
{
    for (int i = 0; i < 3; i++) l[i][0] = '\0';
    if (!(m->coffre & MEMLCD_COFFRE_PRESENT)) return;
    if (m->coffre & MEMLCD_COFFRE_BADVER) { strcpy(l[0], "P4?"); return; }
    if (!(m->coffre & CHEST_STATE_READY)) { strcpy(l[0], "P4.."); return; }
    int n = 0;
    strcpy(l[n++], "P4");
    if (m->coffre & CHEST_STATE_SD)  strcpy(l[n++], "SD");
    if (m->coffre & CHEST_STATE_USB) strcpy(l[n++], "USB");
}
```

and extend `memlcd_model_diff` with `|| a->coffre != b->coffre || a->coffre_op != b->coffre_op`.

- [ ] **Step 4: Run** — `./scripts/check.sh --fast` → GREEN.

- [ ] **Step 5: Backend.** In `memlcd_backend.c`:
  - Under `#if CONFIG_KASE_CHEST_LINK`, `#include "chest_link.h"`.
  - Declare `static lv_obj_t *s_l_coffre[3];` in the keyboard-role block.
  - In `construire()`, keyboard role, after the status lines:

```c
    /* Chest status (memlcd_lignes_coffre) under the logo — the zZ slot: the
     * left never sleeps while the chest exists (USB veto). */
    for (int i = 0; i < 3; i++)
        s_l_coffre[i] = texte_centre(scr, &lv_font_unscii_8, 0, 44 + i * 12, COL_X - 1);
```

  - In `lire_modele()`, keyboard role:

```c
#if CONFIG_KASE_CHEST_LINK
    m->coffre = chest_link_view(&m->coffre_op);
#endif
```

  - In `dessiner()`, keyboard role, replace the name/status block with:

```c
    char lc[3][MEMLCD_COFFRE_BUF];
    memlcd_lignes_coffre(m, lc);
    for (int i = 0; i < 3; i++) lv_label_set_text(s_l_coffre[i], m->veille ? "" : lc[i]);
    if (m->coffre_op) {
        /* The prompt names WHAT is authorized (spec §6): the operation, then OK ?. */
        char op[CHEST_LABEL_BUF];
        chest_op_label(m->coffre_op, op);
        lv_label_set_text(s_l_nom[0], op);
        lv_label_set_text(s_l_nom[1], "OK ?");
        lv_label_set_text(s_l_etat[0], "");
        lv_label_set_text(s_l_etat[1], "");
    } else {
        char lignes[MEMLCD_NOM_LIGNES][MEMLCD_NOM_BUF];
        memlcd_couper_nom(m->nom, lignes);
        for (int i = 0; i < MEMLCD_NOM_LIGNES; i++) lv_label_set_text(s_l_nom[i], lignes[i]);
        char e1[MEMLCD_ETAT_BUF], e2[MEMLCD_ETAT_BUF];
        memlcd_ligne_etat(m, e1, e2);
        lv_label_set_text(s_l_etat[0], e1);
        lv_label_set_text(s_l_etat[1], e2);
    }
```

  (Remove the code this replaces — the existing name and status lines of `dessiner()` — so nothing is set twice.)

- [ ] **Step 6: Build both halves**

Run: `nix develop /home/mae/nixos-config#esp-idf -c bash -c 'export IDF_CCACHE_ENABLE=1 IDF_COMPONENT_CHECK_NEW_VERSION=0; for b in niphar_left niphar_right; do idf.py -B build_$b -DBOARD=$b -DSDKCONFIG=build_$b/sdkconfig build 2>&1 | grep -E " error|warning: |Project build complete"; done'`
Expected: two "Project build complete", no warning from `memlcd_*`.

- [ ] **Step 7: Contract lines.** `COMPORTEMENTS.md`, screens section:

```
- [test:test_memlcd_model] The left shows the chest's status under the logo
  ("P4" ready, "P4.." booting, "P4?" unknown protocol version, then "SD",
  "USB"), nothing without a chest; while the chest has an operation pending
  the bottom area names it (SIGN, DECRYP, FIDO +, DELETE, RESET!…) over
  "OK ?" instead of the layer — the owner sees what she authorizes.
```

- [ ] **Step 8: Commit**

```bash
git add main/display/memlcd/memlcd_model.h main/display/memlcd/memlcd_backend.c test/test_memlcd_model.c COMPORTEMENTS.md .tripwire-testcount
git commit -m "feat(memlcd): chest status under the logo and the confirmation prompt naming the operation"
```

---

### Task 5: `chest_link` — the transport on the left half

**Files:**
- Create: `main/comm/chest/chest_link.h`, `main/comm/chest/chest_link.c`
- Modify: `main/Kconfig.projbuild` (after `KASE_LINK_WIRE`)
- Modify: `main/CMakeLists.txt` (conditional sources)
- Modify: `boards/niphar_left/sdkconfig.defaults` (+ `build_niphar_left/sdkconfig` for the local build)
- Modify: `main/power/veille_task.c` (presence hand-off, in the 1 s loop)
- Modify: `main/main.c` (start the link after `kbd_relay_init()` — the radio creates the bus)
- Modify: `COMPORTEMENTS.md`, `docs/HARDWARE_SMOKE_TEST.md`, `CLAUDE.md` (Architecture tree: `comm/chest/`)

**Interfaces:**
- Consumes: `chest_proto_parse`, `chest_confirm_request/step`, `CHEST_*` (Task 2); `chest_gate_publish`, `chest_gate_take_press` (Task 3); `rf_bus_lock/unlock/host` (`rf_bus.h`); `BOARD_CHEST_CS/IRQ` (Task 1).
- Produces (`chest_link.h`):

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>
void    chest_link_start(void);             /* after the radio created the SPI bus */
void    chest_link_presence(bool usb_host); /* from the sleep task, every 1 s; cheap when unchanged */
/* Screen view: 0 = no chest; else MEMLCD_COFFRE_PRESENT (0x80) | BADVER (0x40)
 * | CHEST_STATE_* bits; *op = pending op (0 none). */
uint8_t chest_link_view(uint16_t *op);
```

- [ ] **Step 1: Kconfig** — after `config KASE_LINK_WIRE ... help ...` block:

```
config KASE_CHEST_LINK
    bool "Chest link: SPI master of Niphar_chest (left half)"
    default n
    depends on KASE_KBD_WIRELESS
    help
        The left half talks to the Niphar_chest ESP32-P4 over the shared SPI
        bus (CS GPIO3, IRQ GPIO46) while a USB host is present: status on
        the screen, a real K_SEC_CONFIRM press relayed to a pending
        operation. Spec: docs/superpowers/specs/2026-09-29-chest-link-s3-master-design.md
```

`boards/niphar_left/sdkconfig.defaults`: append

```
# Chest link (Niphar_chest ESP32-P4): status + confirmation relay (2026-09-29)
CONFIG_KASE_CHEST_LINK=y
```

and in `build_niphar_left/sdkconfig` append the same `CONFIG_KASE_CHEST_LINK=y` line (defaults are only read for keys the sdkconfig does not have yet — here the key is new, so a reconfigure would also pick it up; appending is explicit).

`main/CMakeLists.txt`, next to the `KASE_LINK_WIRE` block:

```cmake
if(CONFIG_KASE_CHEST_LINK)
    list(APPEND srcs "comm/chest/chest_link.c")   # chest_proto.c/chest_gate.c are in the base list (Task 3)
endif()
```

- [ ] **Step 2: Implement `main/comm/chest/chest_link.c`**

```c
/* Chest link, S3 master — spec docs/superpowers/specs/2026-09-29-chest-link-s3-master-design.md.
 * Presence = a USB host (the chest is powered by the left half's USB only).
 * Absent: no SPI device, GPIO3 an input (R48 pulls CS to the chest's rail —
 * driving it into a dead rail costs ~0.33 mA), IRQ off, the task blocked.
 * Present: device added, IRQ armed, a read every CHEST_POLL_MS or at once on
 * an IRQ edge, every transaction under the radio owner's bus lock. */
#include "chest_link.h"
#include "chest_proto.h"
#include "chest_gate.h"
#include "rf_bus.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "chest";
#define CHEST_SPI_HZ   1000000
#define CHEST_POLL_MS  250u
#define CHEST_VIEW_PRESENT 0x80   /* = MEMLCD_COFFRE_PRESENT */
#define CHEST_VIEW_BADVER  0x40   /* = MEMLCD_COFFRE_BADVER */

static TaskHandle_t         s_task;
static spi_device_handle_t  s_dev;
static volatile bool        s_want;          /* presence asked by the sleep task */
static volatile uint8_t     s_view;
static volatile uint16_t    s_view_op;
static chest_confirm_t      s_confirm;
static WORD_ALIGNED_ATTR uint8_t s_rx[CHEST_REG_SIZE];
static WORD_ALIGNED_ATTR uint8_t s_tx[4];

static void cs_release(void)
{
    const gpio_config_t c = { .pin_bit_mask = 1ULL << BOARD_CHEST_CS, .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&c);
}

static void IRAM_ATTR irq_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static bool dev_add(void)
{
    const spi_device_interface_config_t d = {
        .command_bits = 8, .address_bits = 8, .dummy_bits = 8,   /* spi_slave_hd: cmd, addr, dummy */
        .mode = 0, .clock_speed_hz = CHEST_SPI_HZ,
        .spics_io_num = BOARD_CHEST_CS, .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    return spi_bus_add_device(rf_bus_host(), &d, &s_dev) == ESP_OK;
}

static bool xfer(spi_transaction_t *t)
{
    if (!s_dev || !rf_bus_lock(20)) return false;          /* radio busy: next round */
    bool ok = spi_device_polling_transmit(s_dev, t) == ESP_OK;
    rf_bus_unlock();
    return ok;
}

static bool read_block(void)
{
    spi_transaction_t t = { .cmd = CHEST_CMD_RDBUF, .addr = 0,
                            .rxlength = CHEST_REG_SIZE * 8, .rx_buffer = s_rx };
    return xfer(&t);
}

static void write_confirm(void)
{
    s_tx[0] = CHEST_CONFIRM_MAGIC;
    spi_transaction_t t = { .cmd = CHEST_CMD_WRBUF, .addr = CHEST_REG_USER_CONFIRM,
                            .length = 8, .tx_buffer = s_tx };
    if (!xfer(&t)) ESP_LOGW(TAG, "confirmation not written (bus busy) — retried by the rule");
}

static void go_absent(void)
{
    gpio_intr_disable(BOARD_CHEST_IRQ);
    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = NULL; }
    cs_release();
    chest_gate_publish(0);
    (void)chest_gate_take_press();
    s_confirm.armed = false;
    s_view = 0; s_view_op = 0;
    ESP_LOGI(TAG, "chest gone: CS released");
}

static void chest_task(void *arg)
{
    (void)arg;
    uint8_t badver_logged = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, s_dev ? pdMS_TO_TICKS(CHEST_POLL_MS) : portMAX_DELAY);
        if (s_want && !s_dev) {
            if (!dev_add()) { ESP_LOGE(TAG, "spi_bus_add_device failed"); continue; }
            gpio_intr_enable(BOARD_CHEST_IRQ);
            ESP_LOGI(TAG, "USB host present: talking to the chest (CS GPIO%d, IRQ GPIO%d)",
                     BOARD_CHEST_CS, BOARD_CHEST_IRQ);
        } else if (!s_want && s_dev) {
            go_absent();
            continue;
        }
        if (!s_dev || !read_block()) continue;

        chest_status_t st;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        switch (chest_proto_parse(s_rx, CHEST_REG_SIZE, &st)) {
        case CHEST_BLOCK_OK:
            s_view = CHEST_VIEW_PRESENT | (st.state & 0x07);
            s_view_op = st.pending_op;
            chest_gate_publish(st.pending_op);
            if (chest_gate_take_press()) chest_confirm_request(&s_confirm, st.pending_op, st.confirm_count, now);
            if (chest_confirm_step(&s_confirm, st.pending_op, st.confirm_count, now)) write_confirm();
            break;
        case CHEST_BLOCK_BAD_VERSION:
            if (!badver_logged) { ESP_LOGW(TAG, "chest speaks protocol %u, we speak %u: ignored", s_rx[4], CHEST_PROTO_VERSION); badver_logged = 1; }
            s_view = CHEST_VIEW_PRESENT | CHEST_VIEW_BADVER; s_view_op = 0;
            chest_gate_publish(0);
            break;
        default:   /* absent (booting, unpowered) or corrupt: nothing acted on, no log */
            s_view = 0; s_view_op = 0;
            chest_gate_publish(0);
            break;
        }
    }
}

void chest_link_start(void)
{
    cs_release();
    const gpio_config_t irq = { .pin_bit_mask = 1ULL << BOARD_CHEST_IRQ, .mode = GPIO_MODE_INPUT,
                                .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                .intr_type = GPIO_INTR_POSEDGE };   /* R49 is the pull-down */
    gpio_config(&irq);
    gpio_intr_disable(BOARD_CHEST_IRQ);
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(e)); return; }
    gpio_isr_handler_add(BOARD_CHEST_IRQ, irq_isr, NULL);
    xTaskCreatePinnedToCore(chest_task, "chest", 3072, NULL, 3, &s_task, 1);
}

void chest_link_presence(bool usb_host)
{
    if (usb_host == s_want || !s_task) return;
    s_want = usb_host;
    xTaskNotifyGive(s_task);
}

uint8_t chest_link_view(uint16_t *op)
{
    if (op) *op = s_view_op;
    return s_view;
}
```

- [ ] **Step 3: Wire it.** `main/power/veille_task.c`: under the existing includes add

```c
#if CONFIG_KASE_CHEST_LINK
#include "chest_link.h"     /* the chest lives on the USB: presence hand-off */
#endif
```

and in the loop, right after `pm_dfs_usb_rattrapage(usb_presence_cable());` (inside its `#if CONFIG_PM_ENABLE` block or just after it):

```c
#if CONFIG_KASE_CHEST_LINK
        chest_link_presence(usb_presence_cable());   /* no new poller: this 1 s loop already reads it */
#endif
```

`main/main.c`: after `kbd_relay_init();` add

```c
#if CONFIG_KASE_CHEST_LINK
  chest_link_start();   /* after the radio: it creates the shared SPI bus */
#endif
```

with `#include "chest_link.h"` under `#if CONFIG_KASE_CHEST_LINK` near the other includes.

- [ ] **Step 4: Build every board** (the gate and key_processor change touch all keyboard boards)

Run: `nix develop /home/mae/nixos-config#esp-idf -c ./scripts/check.sh --force`
Expected: `✓ check.sh: tout vert`, all six boards built.

- [ ] **Step 5: Contract and smoke.** `COMPORTEMENTS.md`, new section `## Chest link (Niphar_chest)`:

```
- [smoke:Chest link] Presence = a USB host (the chest is powered by the
  left's USB): absent, the SPI device is removed and GPIO3 is an input —
  R48 pulls CS to the chest's rail, driving it into a dead rail would cost
  ~0.33 mA; no poller on battery (the link task blocks, the sleep task's
  1 s USB check hands presence over). Present: a read every 250 ms or at
  once on a GPIO46 rising edge, under the radio owner's bus lock; a block
  that is absent/corrupt is never acted on; a real K_SEC_CONFIRM press
  writes 0x5A at 0x10, delivered when the chest's counter moves, one retry
  after 200 ms at most.
```

`docs/HARDWARE_SMOKE_TEST.md`, "Half (left / right)" section, new item:

```
- [ ] Chest link (left, chest flashed, USB-C plugged): status shows "P4",
      "SD" (card in), "USB" once a mode is mounted; `gpg --card-status`, then
      `echo t | gpg --sign` → the prompt shows "SIGN / OK ?", a press on
      K_SEC_CONFIRM signs; no press → 6985 after 15 s and the prompt goes;
      radio and screen keep working meanwhile; unplug USB → status gone; on
      battery the sleep current is unchanged (GPIO3 released).
```

`CLAUDE.md` Architecture tree, under `comm/`: `│   ├── chest/            # Niphar_chest link (left): chest_proto (pure), chest_gate (key→link), chest_link (SPI master)`.

- [ ] **Step 6: Commit**

```bash
git add main/comm/chest/chest_link.[ch] main/Kconfig.projbuild main/CMakeLists.txt boards/niphar_left/sdkconfig.defaults main/power/veille_task.c main/main.c COMPORTEMENTS.md docs/HARDWARE_SMOKE_TEST.md CLAUDE.md
git commit -m "feat(chest): SPI master of Niphar_chest on the left half — presence from USB, GPIO3 driven only while the chest is powered, IRQ on GPIO46, confirm relay"
```

---

### Task 6: Bench bring-up (with Mae) and the chest's documentation

**Files:**
- Modify (Niphar_chest repository): `docs/HARDWARE.md` ("S3↔chest link — the pinout exists in the PCB" table: S3 side IO7/IO11 → GPIO3/GPIO46, R48/R49)
- Create: memory note `chest-link-s3.md` + `MEMORY.md` pointer

- [ ] **Step 1: Flash the left** (MAC check `d0:cf:13:21:92:60`, app at 0x20000), USB-C plugged, chest powered through the hub. Capture the console (`scripts/console-capture.py /dev/ttyUSB2 <scratch>/chest.log`, detached) and read: `USB host present: talking to the chest`. Ask Mae what the screen shows under the logo. Expected `P4` (+ `SD`, `USB`).
- [ ] **Step 2: If nothing shows** (reads absent/corrupt): the dummy phase is the first suspect — rebuild with `.dummy_bits = 8` only on reads (use `SPI_TRANS_VARIABLE_DUMMY` with `spi_transaction_ext_t`, `dummy_bits = 0` on the WRBUF transaction) and compare. Record which one the chest answers in the spec §4 and in `chest_link.c`.
- [ ] **Step 3: Confirmation, both halves of the proof** — with Mae: `echo t | gpg --sign` → prompt `SIGN / OK ?`, press `K_SEC_CONFIRM` → signature (`gpg: Good signature` on `--verify`); again without pressing → `6985` after 15 s and the prompt goes away.
- [ ] **Step 4: Battery check** — unplug USB, let the left sleep; the ammeter reading stays at the known value (0.67 mA light sleep): GPIO3 is not back-powering the chest.
- [ ] **Step 5: Chest documentation** — in Niphar_chest `docs/HARDWARE.md`, the table of "S3↔chest link — the pinout exists in the PCB": S3 side `GPIO3` for `CS_P4` (with "R48 10k pull-up to P4_3V3") and `GPIO46` for `IRQ_P4` (with "R49 10k pull-down"), citing the netlist export of 2026-09-29, and a line that IO7/IO11 were the P4-side numbers. Commit there:

```bash
git -C ~/Documents/GitHub/Niphar_chest add docs/HARDWARE.md
git -C ~/Documents/GitHub/Niphar_chest commit -m "docs(hardware): S3 side of the chest link is GPIO3 (CS_P4, R48 to P4_3V3) and GPIO46 (IRQ_P4, R49) — from the netlist; IO7/IO11 were the P4-side numbers"
```

- [ ] **Step 6: Memory note** `chest-link-s3.md` (project): pinout from the netlist, the R48 rule, the dummy-phase finding from Step 2, what the bench proved; pointer line in `MEMORY.md`.
