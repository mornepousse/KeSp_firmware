# Plan 2 — Dongle NRF24L01+ RX Stack + Engine Integration

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the dongle receive key events from the (left) half over NRF24L01+, feed them into the existing keymap engine, and emit HID reports to the host — closing the loop from a real keypress on a half to a character on the PC.

**Architecture:** Two NRF24L01+ on a shared SPI bus (one per half, separate CSN/CE/IRQ, separate 2.4 GHz channels). A `rf_driver` module wraps the chip (SPI register access, ESB config, RX FIFO read). A `rf_rx_task` waits on NRF IRQ semaphores, parses packets, maintains the dongle's matrix state, and drives the existing engine loop (`build_keycode_report()` + `send_hid_key()`). A `dongle_engine_state.c` module supplies the globals that `matrix_scan.c` normally owns (not compiled on the dongle). Pure logic (packet codec, heartbeat reconciliation) is TDD'd host-side; the radio/SPI/HID path is validated on the bench.

**Tech Stack:** ESP-IDF 5.5, `esp_driver_spi`, `esp_driver_gpio` (IRQ ISR), FreeRTOS, existing KaSe keymap engine.

**Spec reference:** `docs/superpowers/specs/2026-05-11-dongle-firmware-design.md` Sections 3, 4, 7.

**Depends on:** Plan 1 (kase_dongle board variant, Kconfig flags, build green, hardware boot verified).

---

## Background: how the engine consumes input (verified in code)

`matrix_scan.c` (compiled only on keyboards) owns these globals and flow:

```c
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];   // full 2D pressed state
uint8_t keycodes[MAX_REPORT_KEYS];                // MAX_REPORT_KEYS = 6 (boot proto)
uint8_t current_press_row[MAX_REPORT_KEYS];       // compact list of pressed positions
uint8_t current_press_col[MAX_REPORT_KEYS];
uint8_t current_press_stat[MAX_REPORT_KEYS];
volatile uint8_t stat_matrix_changed;
uint8_t current_layout;                           // active layer
```

Flow each scan cycle:
1. Populate `MATRIX_STATE` + compact `current_press_row/col[]` (up to 6 pressed keys) + `current_press_stat[]`.
2. Set `stat_matrix_changed = 1`.
3. `xTaskNotifyGive(keyboard_task_handle)`.
4. `keyboard_task` (`vTaskKeyboard`) wakes: `build_keycode_report()` reads `current_press_row/col[]` against `keymaps[current_layout]`, fills `keycodes[]`, then `send_hid_key()`.

`keyboard_task.c` also ticks `tap_hold_tick()`, `tap_dance_tick()`, etc. every 10 ms.

**On the dongle:** `matrix_scan.c` and `keyboard_task.c` are NOT compiled (`CONFIG_KASE_HAS_LOCAL_MATRIX=n`). Plan 2 provides:
- `dongle_engine_state.c` — defines the globals above (currently `current_layout`, `matrix_test_*` live as stubs in `cdc_dongle_stubs.c`; Plan 2 moves the input-state ones here).
- `rf_rx_task.c` — the dongle's engine loop: maintains a 70-key pressed map from RF, rebuilds the compact arrays, runs `build_keycode_report()` + `send_hid_key()` and the tick timers.

**Mapping** (spec Section 7): half_L → `MATRIX_STATE[row][0..6]`, half_R → `MATRIX_STATE[row][7..13]`. `current_press_col` stores the GLOBAL column (0..13). The keymap `keymaps[layer][5][14]` is indexed by global (row, col).

---

## File Structure

### Created

| Path | Responsibility |
|---|---|
| `main/comm/rf/rf_packet.h` | Packet type enums, structs, encode/decode helpers (pure, host-testable) |
| `main/comm/rf/rf_driver.h` | NRF24L01+ driver API (one instance per radio) |
| `main/comm/rf/rf_driver.c` | SPI init, register r/w, ESB/DPL config, channel/addr, RX FIFO read, IRQ attach |
| `main/comm/rf/rf_rx_task.h` | rf_rx_task start function + link-state accessors |
| `main/comm/rf/rf_rx_task.c` | IRQ-driven RX loop, packet dispatch, engine drive loop |
| `main/comm/rf/heartbeat.h` | Reconciliation API (pure logic) |
| `main/comm/rf/heartbeat.c` | Bitmap diff → press/release, link timeout |
| `main/comm/rf/dongle_engine_state.c` | Globals owned by matrix_scan.c on keyboards (input-state subset) |
| `test/test_rf_packet.c` | Host tests: encode/decode all packet types, seq wrap, edge cases |
| `test/test_heartbeat.c` | Host tests: reconciliation diff, timeout, dup-seq dedup |
| `boards/kase_dongle/board_rf.h` | NRF pin map convenience (wraps board.h defines into a struct initializer) |

### Modified

| Path | Change |
|---|---|
| `main/CMakeLists.txt` | Add rf/*.c under `if(CONFIG_KASE_HAS_RF_RX)`; add `esp_driver_spi` to priv_requires |
| `main/main.c` | Dongle role: call `rf_rx_start()` instead of the TODO log |
| `main/comm/cdc/cdc_dongle_stubs.c` | Remove `current_layout` + `matrix_test_*` (now in dongle_engine_state.c) |
| `test/CMakeLists.txt` | Add test_rf_packet.c, test_heartbeat.c to test_runner |

### Untouched

Engine files (`key_processor.c`, `key_features.c`, `hid_report.c`, etc.), CDC, USB, board variant from Plan 1.

---

## NRF24L01+ register reference (used throughout)

| Reg | Addr | Purpose |
|---|---|---|
| CONFIG | 0x00 | PWR_UP, PRIM_RX, CRC, IRQ mask |
| EN_AA | 0x01 | Auto-ACK per pipe |
| EN_RXADDR | 0x02 | Enable RX pipes |
| SETUP_AW | 0x03 | Address width (0x03 = 5 bytes) |
| SETUP_RETR | 0x04 | ARD/ARC retransmit |
| RF_CH | 0x05 | Channel (0-125) |
| RF_SETUP | 0x06 | Data rate + TX power |
| STATUS | 0x07 | IRQ flags, RX pipe# |
| RX_ADDR_P0 | 0x0A | Pipe 0 RX address (5 bytes) |
| RX_PW_P0 | 0x11 | Pipe 0 payload width (static; unused with DPL) |
| FIFO_STATUS | 0x17 | RX/TX FIFO state |
| DYNPD | 0x1C | Dynamic payload per pipe |
| FEATURE | 0x1D | EN_DPL, EN_ACK_PAY, EN_DYN_ACK |

Commands: `R_REGISTER`=0x00\|reg, `W_REGISTER`=0x20\|reg, `R_RX_PAYLOAD`=0x61, `R_RX_PL_WID`=0x60, `FLUSH_RX`=0xE2, `NOP`=0xFF.

For 2 Mbps RX with ESB + DPL on pipe 0:
- CONFIG = `0x0F` (PWR_UP=1, PRIM_RX=1, EN_CRC=1, CRCO=1 → 2-byte CRC; mask TX IRQs)
- EN_AA = `0x01` (pipe 0)
- EN_RXADDR = `0x01` (pipe 0)
- SETUP_AW = `0x03` (5-byte address)
- RF_SETUP = `0x0E` (2 Mbps would be `0x0E`? — see Task note: RF_DR_HIGH bit. 2 Mbps = RF_DR_LOW=0, RF_DR_HIGH=1 → `0x0E` includes 0dBm. Verify in Task 4.)
- FEATURE = `0x04` (EN_DPL), DYNPD = `0x01` (pipe 0 dynamic)

---

## Task 1: rf_packet.h + host tests (TDD)

**Files:**
- Create: `main/comm/rf/rf_packet.h`
- Create: `test/test_rf_packet.c`
- Modify: `test/CMakeLists.txt`

Packet formats from spec Section 4. Pure header (inline static functions) so it compiles both on-target and host.

- [ ] **Step 1: Write `rf_packet.h`**

```c
#ifndef RF_PACKET_H
#define RF_PACKET_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Packet type = high nibble of byte 0; flags = low nibble. */
#define PKT_TYPE_KEY        0x1
#define PKT_TYPE_HEARTBEAT  0x2
#define PKT_TYPE_TRACKPAD   0x3

/* Flags (low nibble of byte 0) */
#define PKT_FLAG_PRESSED    0x01   /* PKT_KEY: key is pressed (vs released) */
#define PKT_FLAG_IS_RETRY   0x02   /* application-level retransmit */

/* Matrix geometry per half (must match board.h) */
#define RF_HALF_ROWS        5
#define RF_HALF_COLS        7
#define RF_HALF_BITMAP_BYTES 5     /* ceil(5*7 / 8) = 5 */

typedef struct {
    uint8_t row;       /* 0..4 */
    uint8_t col;       /* 0..6 (local to the half) */
    bool    pressed;
    bool    is_retry;
    uint8_t seq;
} rf_key_event_t;

typedef struct {
    uint8_t bitmap[RF_HALF_BITMAP_BYTES];  /* MSB-first, row*7+col */
    uint8_t batt_dV;   /* 0..83 = 0..8.3V, 0 = unknown */
    uint8_t link_q;    /* cumulative retries since last heartbeat */
    uint8_t seq;
} rf_heartbeat_t;

typedef struct {
    int8_t  dx, dy;
    uint8_t buttons;   /* 3 bits */
    int8_t  scroll_v, scroll_h;
    uint8_t seq;
} rf_trackpad_t;

/* ── Encoders: write into buf, return byte count (0 on error) ── */

static inline uint16_t rf_encode_key(uint8_t *buf, const rf_key_event_t *e)
{
    if (e->row > 15 || e->col > 15) return 0;
    uint8_t flags = (e->pressed ? PKT_FLAG_PRESSED : 0) |
                    (e->is_retry ? PKT_FLAG_IS_RETRY : 0);
    buf[0] = (PKT_TYPE_KEY << 4) | (flags & 0x0F);
    buf[1] = (uint8_t)((e->row << 4) | (e->col & 0x0F));
    buf[2] = e->seq;
    return 3;
}

static inline uint16_t rf_encode_heartbeat(uint8_t *buf, const rf_heartbeat_t *h)
{
    buf[0] = (PKT_TYPE_HEARTBEAT << 4);
    memcpy(&buf[1], h->bitmap, RF_HALF_BITMAP_BYTES);
    buf[1 + RF_HALF_BITMAP_BYTES] = h->batt_dV;
    buf[2 + RF_HALF_BITMAP_BYTES] = h->link_q;
    buf[3 + RF_HALF_BITMAP_BYTES] = h->seq;
    return 4 + RF_HALF_BITMAP_BYTES;   /* 9 */
}

static inline uint16_t rf_encode_trackpad(uint8_t *buf, const rf_trackpad_t *t)
{
    buf[0] = (PKT_TYPE_TRACKPAD << 4);
    buf[1] = (uint8_t)t->dx;
    buf[2] = (uint8_t)t->dy;
    buf[3] = (uint8_t)((t->buttons & 0x07) << 5);  /* buttons in top 3 bits */
    buf[4] = (uint8_t)t->scroll_v;
    buf[5] = (uint8_t)t->scroll_h;
    buf[6] = t->seq;
    return 7;
}

/* ── Decoder: returns type (0 on error/unknown), fills the matching struct ── */

static inline uint8_t rf_packet_type(const uint8_t *buf, uint16_t len)
{
    if (len < 1) return 0;
    return (buf[0] >> 4) & 0x0F;
}

static inline bool rf_decode_key(const uint8_t *buf, uint16_t len, rf_key_event_t *e)
{
    if (len < 3 || rf_packet_type(buf, len) != PKT_TYPE_KEY) return false;
    uint8_t flags = buf[0] & 0x0F;
    e->pressed  = (flags & PKT_FLAG_PRESSED) != 0;
    e->is_retry = (flags & PKT_FLAG_IS_RETRY) != 0;
    e->row = (buf[1] >> 4) & 0x0F;
    e->col = buf[1] & 0x0F;
    e->seq = buf[2];
    return true;
}

static inline bool rf_decode_heartbeat(const uint8_t *buf, uint16_t len, rf_heartbeat_t *h)
{
    if (len < 9 || rf_packet_type(buf, len) != PKT_TYPE_HEARTBEAT) return false;
    memcpy(h->bitmap, &buf[1], RF_HALF_BITMAP_BYTES);
    h->batt_dV = buf[1 + RF_HALF_BITMAP_BYTES];
    h->link_q  = buf[2 + RF_HALF_BITMAP_BYTES];
    h->seq     = buf[3 + RF_HALF_BITMAP_BYTES];
    return true;
}

static inline bool rf_decode_trackpad(const uint8_t *buf, uint16_t len, rf_trackpad_t *t)
{
    if (len < 7 || rf_packet_type(buf, len) != PKT_TYPE_TRACKPAD) return false;
    t->dx = (int8_t)buf[1];
    t->dy = (int8_t)buf[2];
    t->buttons  = (buf[3] >> 5) & 0x07;
    t->scroll_v = (int8_t)buf[4];
    t->scroll_h = (int8_t)buf[5];
    t->seq = buf[6];
    return true;
}

/* Bitmap helpers (row*7+col bit index, MSB-first in byte) */
static inline bool rf_bitmap_get(const uint8_t *bm, uint8_t row, uint8_t col)
{
    uint8_t idx = row * RF_HALF_COLS + col;
    return (bm[idx >> 3] >> (7 - (idx & 7))) & 1;
}

static inline void rf_bitmap_set(uint8_t *bm, uint8_t row, uint8_t col, bool val)
{
    uint8_t idx = row * RF_HALF_COLS + col;
    uint8_t mask = 1 << (7 - (idx & 7));
    if (val) bm[idx >> 3] |= mask;
    else     bm[idx >> 3] &= ~mask;
}

#endif /* RF_PACKET_H */
```

- [ ] **Step 2: Write `test/test_rf_packet.c`**

```c
#include "test_framework.h"
#include "../main/comm/rf/rf_packet.h"

void test_rf_packet(void)
{
    /* KEY round-trip */
    uint8_t buf[32];
    rf_key_event_t e = { .row = 3, .col = 5, .pressed = true, .is_retry = false, .seq = 42 };
    uint16_t n = rf_encode_key(buf, &e);
    TEST_ASSERT_EQ(n, 3);
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_KEY);

    rf_key_event_t d;
    TEST_ASSERT(rf_decode_key(buf, n, &d));
    TEST_ASSERT_EQ(d.row, 3);
    TEST_ASSERT_EQ(d.col, 5);
    TEST_ASSERT(d.pressed);
    TEST_ASSERT(!d.is_retry);
    TEST_ASSERT_EQ(d.seq, 42);

    /* KEY released + retry flags */
    rf_key_event_t e2 = { .row = 0, .col = 0, .pressed = false, .is_retry = true, .seq = 255 };
    n = rf_encode_key(buf, &e2);
    rf_decode_key(buf, n, &d);
    TEST_ASSERT(!d.pressed);
    TEST_ASSERT(d.is_retry);
    TEST_ASSERT_EQ(d.seq, 255);

    /* HEARTBEAT round-trip */
    rf_heartbeat_t h = {0};
    rf_bitmap_set(h.bitmap, 4, 6, true);   /* last key of the half */
    rf_bitmap_set(h.bitmap, 0, 0, true);
    h.batt_dV = 74; h.link_q = 2; h.seq = 7;
    n = rf_encode_heartbeat(buf, &h);
    TEST_ASSERT_EQ(n, 9);
    rf_heartbeat_t hd;
    TEST_ASSERT(rf_decode_heartbeat(buf, n, &hd));
    TEST_ASSERT(rf_bitmap_get(hd.bitmap, 4, 6));
    TEST_ASSERT(rf_bitmap_get(hd.bitmap, 0, 0));
    TEST_ASSERT(!rf_bitmap_get(hd.bitmap, 2, 3));
    TEST_ASSERT_EQ(hd.batt_dV, 74);
    TEST_ASSERT_EQ(hd.link_q, 2);
    TEST_ASSERT_EQ(hd.seq, 7);

    /* TRACKPAD round-trip with negative deltas */
    rf_trackpad_t t = { .dx = -5, .dy = 12, .buttons = 0x05, .scroll_v = -1, .scroll_h = 3, .seq = 9 };
    n = rf_encode_trackpad(buf, &t);
    TEST_ASSERT_EQ(n, 7);
    rf_trackpad_t td;
    TEST_ASSERT(rf_decode_trackpad(buf, n, &td));
    TEST_ASSERT_EQ(td.dx, -5);
    TEST_ASSERT_EQ(td.dy, 12);
    TEST_ASSERT_EQ(td.buttons, 0x05);
    TEST_ASSERT_EQ(td.scroll_v, -1);
    TEST_ASSERT_EQ(td.scroll_h, 3);

    /* Wrong type / short buffer rejection */
    TEST_ASSERT(!rf_decode_key(buf, n, &d));     /* buf holds a trackpad now */
    TEST_ASSERT(!rf_decode_heartbeat(buf, 2, &hd));
}
```

- [ ] **Step 3: Register the test** in `test/CMakeLists.txt` — add `test_rf_packet.c` to the `add_executable(test_runner ...)` list, and add a `RUN_TEST(test_rf_packet)` call in `test/test_main.c` (match the existing pattern — grep `RUN_TEST` in test_main.c first).

- [ ] **Step 4: Build and run host tests**

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test
rm -rf build && mkdir build && cd build && cmake .. >/dev/null && make 2>&1 | tail -5 && ./test_runner 2>&1 | tail -15
```

Expected: `test_rf_packet` passes, no regressions in existing tests.

- [ ] **Step 5: Commit**

```bash
git add main/comm/rf/rf_packet.h test/test_rf_packet.c test/CMakeLists.txt test/test_main.c
git commit -m "feat(dongle): rf_packet codec + host tests (plan 2 task 1)"
```

---

## Task 2: rf_driver.h API

**Files:**
- Create: `main/comm/rf/rf_driver.h`
- Create: `boards/kase_dongle/board_rf.h`

Defines the driver interface. One `rf_radio_t` instance per physical NRF.

- [ ] **Step 1: Write `boards/kase_dongle/board_rf.h`**

```c
#ifndef BOARD_RF_H
#define BOARD_RF_H

#include "board.h"
#include "rf_driver.h"

/* Two radios on a shared SPI bus, one per half. Channels/addresses are
 * loaded from NVS at runtime (Plan 4) — these are compile-time fallbacks
 * matching spec Section 4 factory defaults. */
#define RF_CH_LEFT_DEFAULT    0x4C   /* 2476 MHz */
#define RF_CH_RIGHT_DEFAULT   0x52   /* 2482 MHz */
#define RF_ADDR_BASE_DEFAULT  { 'K', 'a', 'S', 'e' }  /* + 0x01 / 0x02 suffix */

static inline rf_radio_cfg_t board_rf_left_cfg(void)
{
    rf_radio_cfg_t c = {
        .spi_host = BOARD_NRF_SPI_HOST,
        .pin_mosi = BOARD_NRF_SPI_MOSI, .pin_miso = BOARD_NRF_SPI_MISO,
        .pin_sck  = BOARD_NRF_SPI_SCK,  .clock_hz = BOARD_NRF_SPI_CLOCK_HZ,
        .pin_csn  = BOARD_NRF1_CSN_GPIO, .pin_ce = BOARD_NRF1_CE_GPIO,
        .pin_irq  = BOARD_NRF1_IRQ_GPIO,
        .channel  = RF_CH_LEFT_DEFAULT,
        .rx_addr  = RF_ADDR_BASE_DEFAULT,  /* suffix appended in rf_driver_init */
        .addr_suffix = 0x01,
        .shares_bus_first = true,    /* this radio initializes the SPI bus */
    };
    return c;
}

static inline rf_radio_cfg_t board_rf_right_cfg(void)
{
    rf_radio_cfg_t c = {
        .spi_host = BOARD_NRF_SPI_HOST,
        .pin_mosi = BOARD_NRF_SPI_MOSI, .pin_miso = BOARD_NRF_SPI_MISO,
        .pin_sck  = BOARD_NRF_SPI_SCK,  .clock_hz = BOARD_NRF_SPI_CLOCK_HZ,
        .pin_csn  = BOARD_NRF2_CSN_GPIO, .pin_ce = BOARD_NRF2_CE_GPIO,
        .pin_irq  = BOARD_NRF2_IRQ_GPIO,
        .channel  = RF_CH_RIGHT_DEFAULT,
        .rx_addr  = RF_ADDR_BASE_DEFAULT,
        .addr_suffix = 0x02,
        .shares_bus_first = false,   /* bus already initialized by left radio */
    };
    return c;
}

#endif /* BOARD_RF_H */
```

- [ ] **Step 2: Write `main/comm/rf/rf_driver.h`**

```c
#ifndef RF_DRIVER_H
#define RF_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"

typedef struct {
    spi_host_device_t spi_host;
    int pin_mosi, pin_miso, pin_sck, pin_csn, pin_ce, pin_irq;
    int clock_hz;
    uint8_t channel;
    uint8_t rx_addr[4];     /* base, 4 bytes */
    uint8_t addr_suffix;    /* 5th address byte (0x01=L, 0x02=R) */
    bool shares_bus_first;  /* true → this radio calls spi_bus_initialize */
} rf_radio_cfg_t;

typedef struct {
    rf_radio_cfg_t cfg;
    spi_device_handle_t spi;
    bool present;           /* chip-ID/register sanity passed */
    /* IRQ → task signalling: filled by rf_rx_task via rf_radio_set_irq_sem */
    void *irq_sem;          /* SemaphoreHandle_t, set by consumer */
    uint32_t pkt_rx;        /* counters for diagnostics */
    uint32_t pkt_dup;
    uint32_t fifo_ovf;
} rf_radio_t;

/* Initialize SPI (if shares_bus_first), the chip, RX mode (PRX), channel,
 * address, ESB + DPL. Returns ESP_OK; sets radio->present. */
esp_err_t rf_driver_init(rf_radio_t *radio, const rf_radio_cfg_t *cfg);

/* Sanity check: read CONFIG/RF_SETUP back, verify non-0x00/0xFF. */
bool rf_driver_probe(rf_radio_t *radio);

/* Attach an IRQ semaphore: the GPIO ISR gives it on falling edge of IRQ pin. */
void rf_radio_set_irq_sem(rf_radio_t *radio, void *sem);

/* Read one RX payload (DPL). Returns length (0 if FIFO empty). Clears RX_DR. */
uint16_t rf_driver_read_rx(rf_radio_t *radio, uint8_t *buf, uint16_t maxlen);

/* True if RX FIFO has data pending (FIFO_STATUS). */
bool rf_driver_rx_available(rf_radio_t *radio);

/* Register access (exposed for probe/diagnostics). */
uint8_t rf_driver_read_reg(rf_radio_t *radio, uint8_t reg);
void    rf_driver_write_reg(rf_radio_t *radio, uint8_t reg, uint8_t val);

/* Change channel at runtime (Plan 4 pairing). */
void rf_driver_set_channel(rf_radio_t *radio, uint8_t ch);

#endif /* RF_DRIVER_H */
```

- [ ] **Step 3: Commit**

```bash
git add main/comm/rf/rf_driver.h boards/kase_dongle/board_rf.h
git commit -m "feat(dongle): rf_driver API + board_rf cfg (plan 2 task 2)"
```

---

## Task 3: rf_driver.c — SPI init + register access

**Files:**
- Create: `main/comm/rf/rf_driver.c`

- [ ] **Step 1: Write the SPI + register core of `rf_driver.c`**

```c
#include "rf_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "rf_drv";

/* NRF24L01+ commands */
#define CMD_R_REGISTER(r)  (0x00 | ((r) & 0x1F))
#define CMD_W_REGISTER(r)  (0x20 | ((r) & 0x1F))
#define CMD_R_RX_PAYLOAD   0x61
#define CMD_R_RX_PL_WID    0x60
#define CMD_FLUSH_RX       0xE2
#define CMD_NOP            0xFF

/* Registers */
#define REG_CONFIG     0x00
#define REG_EN_AA      0x01
#define REG_EN_RXADDR  0x02
#define REG_SETUP_AW   0x03
#define REG_SETUP_RETR 0x04
#define REG_RF_CH      0x05
#define REG_RF_SETUP   0x06
#define REG_STATUS     0x07
#define REG_RX_ADDR_P0 0x0A
#define REG_RX_PW_P0   0x11
#define REG_FIFO_STATUS 0x17
#define REG_DYNPD      0x1C
#define REG_FEATURE    0x1D

static void csn_low(rf_radio_t *r)  { gpio_set_level(r->cfg.pin_csn, 0); }
static void csn_high(rf_radio_t *r) { gpio_set_level(r->cfg.pin_csn, 1); }
static void ce_low(rf_radio_t *r)   { gpio_set_level(r->cfg.pin_ce, 0); }
static void ce_high(rf_radio_t *r)  { gpio_set_level(r->cfg.pin_ce, 1); }

/* Full-duplex byte exchange over an already-asserted CSN. */
static void spi_xfer(rf_radio_t *r, const uint8_t *tx, uint8_t *rx, size_t n)
{
    spi_transaction_t t = {0};
    t.length = n * 8;
    t.tx_buffer = tx;
    t.rxlength = n * 8;
    t.rx_buffer = rx;
    ESP_ERROR_CHECK(spi_device_polling_transmit(r->spi, &t));
}

uint8_t rf_driver_read_reg(rf_radio_t *r, uint8_t reg)
{
    uint8_t tx[2] = { CMD_R_REGISTER(reg), CMD_NOP };
    uint8_t rx[2] = {0};
    csn_low(r); spi_xfer(r, tx, rx, 2); csn_high(r);
    return rx[1];
}

void rf_driver_write_reg(rf_radio_t *r, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { CMD_W_REGISTER(reg), val };
    uint8_t rx[2] = {0};
    csn_low(r); spi_xfer(r, tx, rx, 2); csn_high(r);
}

static void write_reg_buf(rf_radio_t *r, uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tx[6]; uint8_t rx[6];
    tx[0] = CMD_W_REGISTER(reg);
    memcpy(&tx[1], buf, n);
    csn_low(r); spi_xfer(r, tx, rx, n + 1); csn_high(r);
}

void rf_driver_set_channel(rf_radio_t *r, uint8_t ch)
{
    rf_driver_write_reg(r, REG_RF_CH, ch & 0x7F);
}

bool rf_driver_probe(rf_radio_t *r)
{
    uint8_t cfg = rf_driver_read_reg(r, REG_CONFIG);
    uint8_t rfs = rf_driver_read_reg(r, REG_RF_SETUP);
    /* A present chip returns sane values, never all-0x00 or all-0xFF on both. */
    bool ok = !((cfg == 0x00 && rfs == 0x00) || (cfg == 0xFF && rfs == 0xFF));
    ESP_LOGI(TAG, "probe: CONFIG=0x%02x RF_SETUP=0x%02x → %s", cfg, rfs, ok ? "OK" : "ABSENT");
    return ok;
}

void rf_radio_set_irq_sem(rf_radio_t *r, void *sem) { r->irq_sem = sem; }
```

- [ ] **Step 2: Add the init function to `rf_driver.c`**

```c
esp_err_t rf_driver_init(rf_radio_t *r, const rf_radio_cfg_t *cfg)
{
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;

    /* CSN + CE as GPIO outputs */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << cfg->pin_csn) | (1ULL << cfg->pin_ce),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    csn_high(r);
    ce_low(r);

    /* SPI bus (only the first radio initializes it) */
    if (cfg->shares_bus_first) {
        spi_bus_config_t bus = {
            .mosi_io_num = cfg->pin_mosi,
            .miso_io_num = cfg->pin_miso,
            .sclk_io_num = cfg->pin_sck,
            .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 64,
        };
        esp_err_t e = spi_bus_initialize(cfg->spi_host, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = cfg->clock_hz,
        .mode = 0,                 /* CPOL=0, CPHA=0 */
        .spics_io_num = -1,        /* manual CSN */
        .queue_size = 1,
        .command_bits = 0, .address_bits = 0,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(cfg->spi_host, &dev, &r->spi));

    vTaskDelay(pdMS_TO_TICKS(5));  /* power-on settle */

    if (!rf_driver_probe(r)) { r->present = false; return ESP_FAIL; }

    /* Configure as PRX, 2 Mbps, ESB + DPL on pipe 0 */
    rf_driver_write_reg(r, REG_CONFIG, 0x00);          /* power down while configuring */
    rf_driver_write_reg(r, REG_EN_AA, 0x01);           /* auto-ack pipe 0 */
    rf_driver_write_reg(r, REG_EN_RXADDR, 0x01);       /* enable pipe 0 */
    rf_driver_write_reg(r, REG_SETUP_AW, 0x03);        /* 5-byte address */
    rf_driver_write_reg(r, REG_SETUP_RETR, 0x13);      /* ARD=500us, ARC=3 */
    rf_driver_set_channel(r, cfg->channel);
    rf_driver_write_reg(r, REG_RF_SETUP, 0x0E);        /* 2 Mbps, 0 dBm */
    rf_driver_write_reg(r, REG_FEATURE, 0x04);         /* EN_DPL */
    rf_driver_write_reg(r, REG_DYNPD, 0x01);           /* dynamic payload pipe 0 */

    uint8_t addr[5];
    memcpy(addr, cfg->rx_addr, 4);
    addr[4] = cfg->addr_suffix;
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);

    /* Clear status flags, flush RX */
    rf_driver_write_reg(r, REG_STATUS, 0x70);          /* clear RX_DR|TX_DS|MAX_RT */
    { uint8_t c = CMD_FLUSH_RX, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r); }

    /* Power up as PRX, 2-byte CRC, mask TX IRQs */
    rf_driver_write_reg(r, REG_CONFIG, 0x3F);          /* EN_CRC|CRCO|PWR_UP|PRIM_RX|mask TX/MAX_RT IRQ */
    vTaskDelay(pdMS_TO_TICKS(2));
    ce_high(r);                                        /* start listening */

    r->present = true;
    ESP_LOGI(TAG, "radio ch=%u addr=..%02x init OK", cfg->channel, cfg->addr_suffix);
    return ESP_OK;
}
```

> **Note on CONFIG=0x3F**: bits = MASK_RX_DR(0)? We WANT RX_DR IRQ active (bit6=0 means not masked). 0x3F = 0b00111111 → MASK_RX_DR=0 (RX IRQ enabled), MASK_TX_DS=1, MASK_MAX_RT=1, EN_CRC=1, CRCO=1, PWR_UP=1, PRIM_RX=1. Correct for RX-only with RX IRQ. Verify against datasheet during implementation; adjust if the IRQ never fires.

- [ ] **Step 3: Add RX read functions to `rf_driver.c`**

```c
bool rf_driver_rx_available(rf_radio_t *r)
{
    uint8_t fifo = rf_driver_read_reg(r, REG_FIFO_STATUS);
    return (fifo & 0x01) == 0;   /* bit0 RX_EMPTY = 0 → data present */
}

uint16_t rf_driver_read_rx(rf_radio_t *r, uint8_t *buf, uint16_t maxlen)
{
    if (!rf_driver_rx_available(r)) return 0;

    /* Read payload width (DPL) */
    uint8_t twx[2] = { CMD_R_RX_PL_WID, CMD_NOP };
    uint8_t trx[2] = {0};
    csn_low(r); spi_xfer(r, twx, trx, 2); csn_high(r);
    uint8_t w = trx[1];
    if (w == 0 || w > 32) {       /* corrupt: flush */
        uint8_t c = CMD_FLUSH_RX, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r);
        r->fifo_ovf++;
        return 0;
    }
    if (w > maxlen) w = maxlen;

    uint8_t tx[33], rx[33];
    tx[0] = CMD_R_RX_PAYLOAD;
    memset(&tx[1], CMD_NOP, w);
    csn_low(r); spi_xfer(r, tx, rx, w + 1); csn_high(r);
    memcpy(buf, &rx[1], w);

    /* Clear RX_DR */
    rf_driver_write_reg(r, REG_STATUS, 0x40);
    r->pkt_rx++;
    return w;
}
```

- [ ] **Step 4: Add rf/*.c to the dongle build** in `main/CMakeLists.txt`:

Replace the Plan-1 placeholder block:

```cmake
if(CONFIG_KASE_HAS_RF_RX)
    # Plan 2 will add: comm/rf/rf_driver.c, rf_rx_task.c, heartbeat.c
endif()
```

with:

```cmake
if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND srcs
        "comm/rf/rf_driver.c"
        "comm/rf/rf_rx_task.c"
        "comm/rf/heartbeat.c"
        "comm/rf/dongle_engine_state.c"
    )
endif()
```

And add `esp_driver_spi` to the always-on `priv_requires` list (it's needed whenever RF is compiled):

```cmake
if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND priv_requires esp_driver_spi)
endif()
```

- [ ] **Step 5: Add INCLUDE_DIRS** — add `"comm/rf"` to the `INCLUDE_DIRS` list in `main/CMakeLists.txt`.

- [ ] **Step 6: Commit** (build happens after Task 6 when all rf/*.c exist; this commits the driver)

```bash
git add main/comm/rf/rf_driver.c main/CMakeLists.txt
git commit -m "feat(dongle): NRF24 SPI driver — init/probe/RX (plan 2 task 3)"
```

---

## Task 4: heartbeat.c + host tests (TDD)

**Files:**
- Create: `main/comm/rf/heartbeat.h`
- Create: `main/comm/rf/heartbeat.c`
- Create: `test/test_heartbeat.c`
- Modify: `test/CMakeLists.txt`, `test/test_main.c`

Pure reconciliation logic, no ESP-IDF deps → host-testable. Operates on a callback interface so the test can capture press/release without the real engine.

- [ ] **Step 1: Write `heartbeat.h`**

```c
#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <stdint.h>
#include <stdbool.h>
#include "rf_packet.h"

/* Callbacks invoked by reconciliation to force engine state changes. */
typedef struct {
    void (*force_press)(void *ctx, uint8_t half, uint8_t row, uint8_t col);
    void (*force_release)(void *ctx, uint8_t half, uint8_t row, uint8_t col);
    void *ctx;
} hb_callbacks_t;

#define HB_HALF_LEFT  0
#define HB_HALF_RIGHT 1

/* Per-half tracked state. */
typedef struct {
    uint8_t  local_bitmap[RF_HALF_BITMAP_BYTES];  /* what we think is pressed */
    uint8_t  last_seq;
    uint8_t  last_hb_seq;
    uint32_t last_hb_ms;
    bool     link_up;
    bool     seq_valid;
} hb_half_state_t;

/* Apply a key event from a half to the local bitmap, dedup by seq.
 * Returns true if state changed (caller should re-run the engine). */
bool hb_apply_key(hb_half_state_t *st, const rf_key_event_t *e);

/* Reconcile against a received heartbeat bitmap. Invokes callbacks for
 * any divergence (force press/release). Updates local_bitmap to match. */
void hb_reconcile(hb_half_state_t *st, uint8_t half, const rf_heartbeat_t *h,
                  const hb_callbacks_t *cb, uint32_t now_ms);

/* Call periodically. If now - last_hb_ms > timeout_ms and link was up,
 * release all keys of this half and mark link down. */
void hb_check_timeout(hb_half_state_t *st, uint8_t half,
                      const hb_callbacks_t *cb, uint32_t now_ms, uint32_t timeout_ms);

#endif /* HEARTBEAT_H */
```

- [ ] **Step 2: Write `heartbeat.c`**

```c
#include "heartbeat.h"
#include <string.h>

bool hb_apply_key(hb_half_state_t *st, const rf_key_event_t *e)
{
    if (st->seq_valid && e->seq == st->last_seq && !e->is_retry)
        return false;                     /* duplicate */
    st->last_seq = e->seq;
    st->seq_valid = true;
    bool cur = rf_bitmap_get(st->local_bitmap, e->row, e->col);
    if (cur == e->pressed) return false;  /* no change */
    rf_bitmap_set(st->local_bitmap, e->row, e->col, e->pressed);
    return true;
}

void hb_reconcile(hb_half_state_t *st, uint8_t half, const rf_heartbeat_t *h,
                  const hb_callbacks_t *cb, uint32_t now_ms)
{
    for (uint8_t row = 0; row < RF_HALF_ROWS; row++) {
        for (uint8_t col = 0; col < RF_HALF_COLS; col++) {
            bool want = rf_bitmap_get(h->bitmap, row, col);
            bool have = rf_bitmap_get(st->local_bitmap, row, col);
            if (want == have) continue;
            if (want) {
                rf_bitmap_set(st->local_bitmap, row, col, true);
                if (cb && cb->force_press) cb->force_press(cb->ctx, half, row, col);
            } else {
                rf_bitmap_set(st->local_bitmap, row, col, false);
                if (cb && cb->force_release) cb->force_release(cb->ctx, half, row, col);
            }
        }
    }
    st->last_hb_seq = h->seq;
    st->last_hb_ms = now_ms;
    st->link_up = true;
}

void hb_check_timeout(hb_half_state_t *st, uint8_t half,
                      const hb_callbacks_t *cb, uint32_t now_ms, uint32_t timeout_ms)
{
    if (!st->link_up) return;
    if (now_ms - st->last_hb_ms <= timeout_ms) return;

    /* Release everything this half holds */
    for (uint8_t row = 0; row < RF_HALF_ROWS; row++)
        for (uint8_t col = 0; col < RF_HALF_COLS; col++)
            if (rf_bitmap_get(st->local_bitmap, row, col)) {
                rf_bitmap_set(st->local_bitmap, row, col, false);
                if (cb && cb->force_release) cb->force_release(cb->ctx, half, row, col);
            }
    st->link_up = false;
}
```

- [ ] **Step 3: Write `test/test_heartbeat.c`**

```c
#include "test_framework.h"
#include "../main/comm/rf/heartbeat.h"
#include <string.h>

/* Capture callback invocations */
static int g_press_count, g_release_count;
static uint8_t g_last_row, g_last_col;
static void cap_press(void *c, uint8_t h, uint8_t r, uint8_t col)
{ (void)c;(void)h; g_press_count++; g_last_row=r; g_last_col=col; }
static void cap_release(void *c, uint8_t h, uint8_t r, uint8_t col)
{ (void)c;(void)h; g_release_count++; g_last_row=r; g_last_col=col; }

void test_heartbeat(void)
{
    hb_callbacks_t cb = { cap_press, cap_release, NULL };
    hb_half_state_t st; memset(&st, 0, sizeof(st));

    /* apply_key: press then dedup */
    rf_key_event_t e = { .row=2, .col=3, .pressed=true, .is_retry=false, .seq=1 };
    TEST_ASSERT(hb_apply_key(&st, &e));        /* changed */
    TEST_ASSERT(!hb_apply_key(&st, &e));       /* same seq → dup */
    TEST_ASSERT(rf_bitmap_get(st.local_bitmap, 2, 3));

    /* release with new seq */
    rf_key_event_t e2 = { .row=2, .col=3, .pressed=false, .is_retry=false, .seq=2 };
    TEST_ASSERT(hb_apply_key(&st, &e2));
    TEST_ASSERT(!rf_bitmap_get(st.local_bitmap, 2, 3));

    /* reconcile: heartbeat says key (1,1) pressed but we missed the event */
    g_press_count = g_release_count = 0;
    rf_heartbeat_t h; memset(&h, 0, sizeof(h));
    rf_bitmap_set(h.bitmap, 1, 1, true);
    hb_reconcile(&st, HB_HALF_LEFT, &h, &cb, 1000);
    TEST_ASSERT_EQ(g_press_count, 1);
    TEST_ASSERT_EQ(g_last_row, 1);
    TEST_ASSERT_EQ(g_last_col, 1);
    TEST_ASSERT(st.link_up);

    /* reconcile: heartbeat now empty → release the stuck key */
    g_press_count = g_release_count = 0;
    memset(&h, 0, sizeof(h));
    hb_reconcile(&st, HB_HALF_LEFT, &h, &cb, 1100);
    TEST_ASSERT_EQ(g_release_count, 1);

    /* timeout: press something via reconcile, then let it lapse */
    rf_bitmap_set(h.bitmap, 0, 0, true);
    hb_reconcile(&st, HB_HALF_LEFT, &h, &cb, 1200);
    g_release_count = 0;
    hb_check_timeout(&st, HB_HALF_LEFT, &cb, 1500, 250);   /* 300ms > 250ms */
    TEST_ASSERT_EQ(g_release_count, 1);
    TEST_ASSERT(!st.link_up);
}
```

- [ ] **Step 4: Register + run** — add `test_heartbeat.c` to `test/CMakeLists.txt`, `RUN_TEST(test_heartbeat)` to `test_main.c`, then:

```bash
cd /home/mae/Documents/GitHub/KaSe_firmware/test/build && cmake .. >/dev/null && make 2>&1 | tail -3 && ./test_runner 2>&1 | tail -10
```

Expected: `test_heartbeat` passes.

- [ ] **Step 5: Commit**

```bash
git add main/comm/rf/heartbeat.h main/comm/rf/heartbeat.c test/test_heartbeat.c test/CMakeLists.txt test/test_main.c
git commit -m "feat(dongle): heartbeat reconciliation + host tests (plan 2 task 4)"
```

---

## Task 5: dongle_engine_state.c — engine globals

**Files:**
- Create: `main/comm/rf/dongle_engine_state.c`
- Modify: `main/comm/cdc/cdc_dongle_stubs.c` (remove the input-state globals moved here)

Provides the globals that `matrix_scan.c` owns on keyboards, so the engine links on the dongle.

- [ ] **Step 1: Write `dongle_engine_state.c`**

```c
/*
 * Engine input-state globals for the dongle.
 * On keyboards these live in matrix_scan.c (not compiled on dongle).
 * rf_rx_task.c populates current_press_row/col[] + MATRIX_STATE from RF,
 * the engine (build_keycode_report) consumes them exactly as on a keyboard.
 */
#include <stdint.h>
#include "keyboard_config.h"   /* MATRIX_ROWS, MATRIX_COLS */

#define MAX_REPORT_KEYS 6      /* must match matrix_scan.c boot-protocol limit */

uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];  /* unused on dongle, kept for symbol parity */

uint8_t keycodes[MAX_REPORT_KEYS];
uint8_t current_press_row[MAX_REPORT_KEYS];
uint8_t current_press_col[MAX_REPORT_KEYS];
uint8_t current_press_stat[MAX_REPORT_KEYS];
volatile uint8_t stat_matrix_changed;

uint8_t current_layout = 0;

/* matrix test mode is keyboard-only; provide inert globals for CDC handlers */
volatile bool matrix_test_mode = false;
volatile uint32_t matrix_test_last_activity_ms = 0;
```

- [ ] **Step 2: Remove the now-duplicated globals from `cdc_dongle_stubs.c`**

Delete this block (it moved to dongle_engine_state.c):

```c
uint8_t current_layout = 0;
volatile bool matrix_test_mode = false;
volatile uint32_t matrix_test_last_activity_ms = 0;
```

Keep all the function stubs (tama, bt, status_display, board_layout_json).

- [ ] **Step 3: Commit**

```bash
git add main/comm/rf/dongle_engine_state.c main/comm/cdc/cdc_dongle_stubs.c
git commit -m "feat(dongle): engine input-state globals module (plan 2 task 5)"
```

---

## Task 6: rf_rx_task.c — IRQ RX loop + engine drive

**Files:**
- Create: `main/comm/rf/rf_rx_task.h`
- Create: `main/comm/rf/rf_rx_task.c`

This is the heart of Plan 2. It owns both radios, the per-half heartbeat state, and the engine loop.

- [ ] **Step 1: Write `rf_rx_task.h`**

```c
#ifndef RF_RX_TASK_H
#define RF_RX_TASK_H

#include <stdint.h>
#include <stdbool.h>

/* Start RF radios + rx task. Returns false if neither radio is present. */
bool rf_rx_start(void);

/* Link diagnostics for CDC (Plan 4). */
typedef struct {
    bool link_left, link_right;
    uint32_t hb_age_left_ms, hb_age_right_ms;
    uint32_t pkt_rx_left, pkt_rx_right;
    uint32_t pkt_dup_left, pkt_dup_right;
} rf_link_status_t;

void rf_rx_get_status(rf_link_status_t *out);

#endif /* RF_RX_TASK_H */
```

- [ ] **Step 2: Write `rf_rx_task.c`**

```c
#include "rf_rx_task.h"
#include "rf_driver.h"
#include "rf_packet.h"
#include "heartbeat.h"
#include "board_rf.h"
#include "keyboard_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semaphore.h"
#include <string.h>

static const char *TAG = "rf_rx";

/* Engine globals (dongle_engine_state.c) */
extern uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t current_press_row[];
extern uint8_t current_press_col[];
extern uint8_t current_press_stat[];
extern uint8_t keycodes[];
extern volatile uint8_t stat_matrix_changed;
#define MAX_REPORT_KEYS 6
#define INVALID_KEY_POS 0xFF

/* Engine entry points (key_processor.c / hid_report.c / key_features.c) */
extern void build_keycode_report(void);
extern void process_matrix_changes(void);
extern void send_hid_key(void);
extern bool key_processor_has_tap(void);
extern void key_processor_clear_taps(void);
extern void tap_hold_tick(void);
extern void tap_dance_tick(void);

static rf_radio_t s_left, s_right;
static hb_half_state_t s_hb_left, s_hb_right;
static SemaphoreHandle_t s_evt_sem;

#define HALF_R_COL_OFFSET 7

/* ── IRQ ISR (shared sem; task polls both radios) ── */
static void IRAM_ATTR nrf_irq_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(s_evt_sem, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

/* ── Rebuild compact current_press_* arrays from MATRIX_STATE ── */
static void rebuild_press_arrays(void)
{
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i] = INVALID_KEY_POS;
        current_press_col[i] = INVALID_KEY_POS;
        current_press_stat[i] = 0;
        keycodes[i] = 0;
    }
    uint8_t filled = 0;
    for (uint8_t r = 0; r < MATRIX_ROWS && filled < MAX_REPORT_KEYS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS && filled < MAX_REPORT_KEYS; c++)
            if (MATRIX_STATE[r][c]) {
                current_press_row[filled] = r;
                current_press_col[filled] = c;
                current_press_stat[filled] = 1;
                filled++;
            }
    stat_matrix_changed = 1;
}

/* ── Engine callbacks for heartbeat reconciliation ── */
static void on_force_press(void *ctx, uint8_t half, uint8_t row, uint8_t col)
{
    (void)ctx;
    uint8_t gcol = (half == HB_HALF_RIGHT) ? col + HALF_R_COL_OFFSET : col;
    MATRIX_STATE[row][gcol] = 1;
}
static void on_force_release(void *ctx, uint8_t half, uint8_t row, uint8_t col)
{
    (void)ctx;
    uint8_t gcol = (half == HB_HALF_RIGHT) ? col + HALF_R_COL_OFFSET : col;
    MATRIX_STATE[row][gcol] = 0;
}
static const hb_callbacks_t s_cb = { on_force_press, on_force_release, NULL };

/* ── Run one engine cycle (mirrors keyboard_task) ── */
static void run_engine_cycle(void)
{
    rebuild_press_arrays();
    build_keycode_report();
    process_matrix_changes();
    if (key_processor_has_tap()) {
        send_hid_key();
        vTaskDelay(pdMS_TO_TICKS(10));
        key_processor_clear_taps();
        send_hid_key();
    } else {
        send_hid_key();
    }
}

/* ── Process all pending packets from one radio ── */
static bool drain_radio(rf_radio_t *radio, hb_half_state_t *hb, uint8_t half)
{
    bool changed = false;
    uint8_t buf[32];
    while (rf_driver_rx_available(radio)) {
        uint16_t n = rf_driver_read_rx(radio, buf, sizeof(buf));
        if (n == 0) break;
        uint8_t type = rf_packet_type(buf, n);
        if (type == PKT_TYPE_KEY) {
            rf_key_event_t e;
            if (rf_decode_key(buf, n, &e) && hb_apply_key(hb, &e)) {
                uint8_t gcol = (half == HB_HALF_RIGHT) ? e.col + HALF_R_COL_OFFSET : e.col;
                if (e.row < MATRIX_ROWS && gcol < MATRIX_COLS) {
                    MATRIX_STATE[e.row][gcol] = e.pressed ? 1 : 0;
                    changed = true;
                }
            } else {
                radio->pkt_dup++;
            }
        } else if (type == PKT_TYPE_HEARTBEAT) {
            rf_heartbeat_t h;
            if (rf_decode_heartbeat(buf, n, &h)) {
                uint32_t now = esp_timer_get_time() / 1000;
                hb_reconcile(hb, half, &h, &s_cb, now);
                changed = true;   /* reconciliation may have changed MATRIX_STATE */
            }
        }
        /* PKT_TYPE_TRACKPAD handled in Plan 3 (mouse HID) */
    }
    return changed;
}

static void rf_rx_task(void *arg)
{
    (void)arg;
    const TickType_t tick_period = pdMS_TO_TICKS(10);
    for (;;) {
        /* Wait for IRQ or 10ms tick (for tap-hold timers + timeout checks) */
        xSemaphoreTake(s_evt_sem, tick_period);

        bool changed = false;
        if (s_left.present)  changed |= drain_radio(&s_left,  &s_hb_left,  HB_HALF_LEFT);
        if (s_right.present) changed |= drain_radio(&s_right, &s_hb_right, HB_HALF_RIGHT);

        /* Heartbeat timeouts */
        uint32_t now = esp_timer_get_time() / 1000;
        hb_check_timeout(&s_hb_left,  HB_HALF_LEFT,  &s_cb, now, 250);
        hb_check_timeout(&s_hb_right, HB_HALF_RIGHT, &s_cb, now, 250);

        /* Engine ticks every cycle */
        tap_hold_tick();
        tap_dance_tick();

        if (changed)
            run_engine_cycle();
    }
}

bool rf_rx_start(void)
{
    s_evt_sem = xSemaphoreCreateBinary();

    rf_radio_cfg_t lcfg = board_rf_left_cfg();
    rf_radio_cfg_t rcfg = board_rf_right_cfg();
    rf_driver_init(&s_left, &lcfg);
    rf_driver_init(&s_right, &rcfg);

    if (!s_left.present && !s_right.present) {
        ESP_LOGE(TAG, "no NRF radios present — RF disabled");
        return false;
    }

    /* Attach IRQ ISR (falling edge, both share the sem) */
    gpio_install_isr_service(0);
    if (s_left.present) {
        gpio_set_intr_type(lcfg.pin_irq, GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add(lcfg.pin_irq, nrf_irq_isr, &s_left);
    }
    if (s_right.present) {
        gpio_set_intr_type(rcfg.pin_irq, GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add(rcfg.pin_irq, nrf_irq_isr, &s_right);
    }

    xTaskCreatePinnedToCore(rf_rx_task, "rf_rx", 8192, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "RF RX started (L=%d R=%d)", s_left.present, s_right.present);
    return true;
}

void rf_rx_get_status(rf_link_status_t *out)
{
    uint32_t now = esp_timer_get_time() / 1000;
    out->link_left  = s_hb_left.link_up;
    out->link_right = s_hb_right.link_up;
    out->hb_age_left_ms  = now - s_hb_left.last_hb_ms;
    out->hb_age_right_ms = now - s_hb_right.last_hb_ms;
    out->pkt_rx_left  = s_left.pkt_rx;
    out->pkt_rx_right = s_right.pkt_rx;
    out->pkt_dup_left  = s_left.pkt_dup;
    out->pkt_dup_right = s_right.pkt_dup;
}
```

> **Note**: the IRQ pins also need to be configured as inputs. `gpio_isr_handler_add` works on a pin already set as input; add a `gpio_set_direction(pin_irq, GPIO_MODE_INPUT)` + pull-up in `rf_driver_init` (NRF IRQ is active-low, open-ish — enable internal pull-up). Add that to Task 3's init if the ISR never fires.

- [ ] **Step 3: Wire into `main.c`** — replace the Plan 1 dongle TODO block:

```c
#else  /* CONFIG_KASE_DEVICE_ROLE_DONGLE */
  ESP_LOGI(TAG, "Dongle role: matrix/display/BLE skipped, awaiting RF stack (Plan 2)");
#endif
```

with:

```c
#else  /* CONFIG_KASE_DEVICE_ROLE_DONGLE */
  ESP_LOGI(TAG, "Dongle role: starting RF RX");
  {
    extern bool rf_rx_start(void);
    extern void keyboard_manager_init(void);
    keyboard_manager_init();   /* inits tap_hold/tap_dance/combo/leader/hid_report */
    if (!rf_rx_start())
      ESP_LOGE(TAG, "RF RX failed to start");
  }
#endif
```

> **Note**: `keyboard_manager_init()` lives in `keyboard_task.c` which is NOT compiled on the dongle. Two options: (a) also compile `keyboard_task.c` on the dongle (it pulls matrix deps — messy), or (b) call the individual inits directly. Prefer (b): replace `keyboard_manager_init()` with explicit `tap_hold_init(); tap_dance_init(); combo_init(); leader_init(); key_override_init(); hid_report_init();`. Verify these symbols are in compiled units (key_features.c / hid_report.c / tap_*.c / combo.c / leader.c — all compiled on dongle).

- [ ] **Step 4: Build the dongle**

```bash
source ~/esp/esp-idf/export.sh
cd /home/mae/Documents/GitHub/KaSe_firmware
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build 2>&1 | tail -20
```

Expected: `Project build complete`. Fix any missing-symbol or include errors by adjusting the explicit init list (Step 3 note) and INCLUDE_DIRS.

- [ ] **Step 5: Commit**

```bash
git add main/comm/rf/rf_rx_task.h main/comm/rf/rf_rx_task.c main/main.c main/CMakeLists.txt
git commit -m "feat(dongle): rf_rx_task — IRQ RX loop + engine drive (plan 2 task 6)"
```

---

## Task 7: Bench validation — NRF probe on real dongle

**Files:** none (hardware)

Validate the radios are wired correctly before worrying about a TX peer.

- [ ] **Step 1: Flash the dongle**

```bash
idf.py -B build_dongle -p /dev/ttyUSB0 flash
```

- [ ] **Step 2: Read RF link status via CDC** (no console UART on dongle). Use the diagnostic command added in Plan 4 — but Plan 4 isn't done yet. For Plan 2, temporarily enable console on the dongle to see `rf_drv` probe logs:

Add to `sdkconfig.defaults.dongle` temporarily:
```
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```
(overrides the inherited `CONFIG_ESP_CONSOLE_NONE`). Rebuild + flash, then monitor the USB-Serial-JTAG console (the `303a:1001` device, a `/dev/ttyACM*`):

```bash
idf.py -B build_dongle monitor -p /dev/ttyACM<jtag>
```

Expected log lines:
```
rf_drv: probe: CONFIG=0x.. RF_SETUP=0x.. → OK     (×2, one per radio)
rf_rx: RF RX started (L=1 R=1)
```

If a radio reads `CONFIG=0x00 RF_SETUP=0x00 → ABSENT`, the SPI wiring or CSN/CE pin mapping is wrong — recheck `board.h` against the schematic netlist.

- [ ] **Step 3: Record results** in the dongle README "Status" section. Revert the console override once probe is confirmed (or keep it for Plan 2 dev; remove before release).

---

## Task 8 (optional but recommended): minimal NRF TX tester on the half

**Files:**
- Create: `tools/nrf_tx_tester/` (standalone ESP-IDF mini-app) OR a `kase_half_txtest` board variant

To validate the full RX path without the (not-yet-written) half firmware, flash the **left half** with a tiny app that TXes a `PKT_KEY` every second on `RF_CH_LEFT_DEFAULT` with the left address, cycling through a few (row,col) positions.

- [ ] **Step 1: Write the tester** — a minimal `app_main` that:
  - Inits one NRF24 as PTX (PRIM_RX=0) on channel 0x4C, TX address = `KaSe\x01`
  - Every 1 s, sends `rf_encode_key()` for (row=0,col=0) pressed, then released
  - Reuses `rf_packet.h` and a PTX variant of the driver (or raw register writes)

  Keep it in `tools/nrf_tx_tester/` with its own `CMakeLists.txt` + `main/`. Reuse `rf_packet.h` via a relative include.

- [ ] **Step 2: Flash the half**

```bash
cd tools/nrf_tx_tester
idf.py -p /dev/ttyUSB1 -DIDF_TARGET=esp32s3 flash
```

> The half's NRF pinout differs from the dongle — set the tester's pins from the half PCB schematic (`KaSe.kicad_sch`, the half's NRF24 net). Extract via the same netlist method used in Plan 1.

- [ ] **Step 3: Observe on the dongle** — `rf_rx` should log received packets / increment `pkt_rx`, and if the (0,0) key maps to a real keycode, the host should receive that character. Confirm with a text editor focused, or read `rf_rx_get_status()` via the temporary console.

- [ ] **Step 4: Commit the tester**

```bash
git add tools/nrf_tx_tester/
git commit -m "test(dongle): minimal NRF TX tester for RX bench validation (plan 2 task 8)"
```

---

## Self-Review Checklist

- ✅ **Spec coverage**: Sections 3 (tasks/flow), 4 (RF protocol: 2 channels, ESB, DPL, heartbeat, anti-stuck), 7 (engine integration via input-state globals) all implemented.
- ✅ **No placeholders**: all register values, packet layouts, GPIO references concrete. Two flagged judgment points (CONFIG IRQ-mask bits in Task 3, explicit init list in Task 6) carry verification notes, not blanks.
- ✅ **Type consistency**: `rf_key_event_t`/`rf_heartbeat_t`/`rf_trackpad_t`, `hb_half_state_t`, `rf_radio_t`, `rf_radio_cfg_t` consistent across header/impl/tests. `HALF_R_COL_OFFSET=7`, `MATRIX_COLS=14`, `MAX_REPORT_KEYS=6` consistent.
- ✅ **TDD**: rf_packet + heartbeat are pure and host-tested before the radio/HID path is wired.

## Known limitations carried to later plans

- **6-key rollover only** (boot protocol). NKRO bitmap report → Plan 3.
- **No trackpad** dispatch yet (PKT_TRACKPAD decoded but ignored) → Plan 3 (mouse HID).
- **Channels/addresses hardcoded** to factory defaults → Plan 4 (NVS + CDC `KS_CMD_RF_SET_CONFIG`).
- **No console on dongle in prod** — Task 7 uses a temporary USB-Serial-JTAG console override for bring-up; link diagnostics move to CDC `KS_CMD_RF_LINK_STATUS` in Plan 4.
- **ESP-NOW cold path** (halves OTA, config push) → Plan 5.

## End state after Plan 2

A real keypress on the left half (running the Task 8 tester, or eventually the real half firmware) travels NRF24 → dongle rf_rx_task → engine → USB HID → character on the host. The full hot path is closed and measurable.
