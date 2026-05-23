# Finer RF signal gauge via ARC_CNT — design

**Date:** 2026-05-23
**Status:** approved
**Scope:** small (metric refinement, ~4 files + tests)

## Problem

The half→dongle signal bars (`rf_signal_bars`) derive `retry_score` from `link_q`,
which today is the **count of complete TX failures** (MAX_RT, i.e. all ARC=3 retries
exhausted) since the last heartbeat. With ARC=3 auto-retransmit, a marginal link
where packets succeed *on retry* registers `link_q = 0` → 4 bars. The gauge only
moves once packets are fully lost — too late to be a useful early-warning of a
degrading link.

## Goal

Make the signal bars reflect **retransmission effort**, not just hard losses, so a
link that is degrading (packets succeed but need 1–2 retries) shows fewer bars
*before* it starts dropping packets.

## Mechanism — NRF24 OBSERVE_TX

Register `OBSERVE_TX` (0x08):
- bits 7:4 `PLOS_CNT` — lost packets; resets only on RF_CH write → not per-interval useful.
- bits 3:0 `ARC_CNT` — retransmits for the **last** transmitted packet (0..ARC=3 here);
  resets at the start of each new TX. On a MAX_RT it reads 3 (maxed).

So reading `OBSERVE_TX & 0x0F` right after a TX completes gives that packet's retry count.

## Design

### 1. Accumulate — `main/comm/rf/rf_driver.c`

Add `#define REG_OBSERVE_TX 0x08`. In the polled `rf_driver_send()` (the TX path the
half uses for heartbeats + key data), after the TX_DS/MAX_RT poll completes and the
FIFO is handled:

```c
uint8_t arc = rf_driver_read_reg(r, REG_OBSERVE_TX) & 0x0F;
rf_tx_retr_sum += arc;
rf_tx_count++;
```

New globals (alongside the existing `rf_tx_max_rt_count`, which stays for the
FIFO-flush logic):
```c
uint32_t rf_tx_retr_sum = 0;   /* Σ ARC_CNT since last HB read */
uint32_t rf_tx_count    = 0;   /* # TX since last HB read */
```
On the dongle these stay 0 (it is PRX, never TXes data) — harmless.

### 2. Encode — `main/comm/rf/half_scan_task.c`

`link_q` changes from "MAX_RT count" to a **volume-independent retry percentage**
(0 = pristine, 100 = every packet maxing all 3 retries):

```c
uint32_t txc = rf_tx_count, rsum = rf_tx_retr_sum;
hb.link_q = (txc > 0) ? (uint8_t)((rsum * 100u) / (txc * 3u)) : 0;
rf_tx_retr_sum = 0;
rf_tx_count    = 0;
```
(Replaces the current `rf_tx_max_rt_count`-based line and its reset.) Same `u8`
wire field, no size change — only the semantics change. Half + dongle are always
flashed together, so there is no cross-version compatibility concern.

### 3. Interpret — `main/comm/rf/rf_rx_task.c` (`rf_signal_bars`)

`link_q` is now a retry % (0..100). New `retry_score` thresholds:

| link_q (retry %) | retry_score | meaning              |
|------------------|-------------|----------------------|
| `== 0`           | 4           | pristine, 0 retries  |
| `<= 15`          | 3           | occasional (≤0.45/pkt)|
| `<= 33`          | 2           | moderate (≤1/pkt)    |
| `<= 60`          | 1           | heavy                |
| `> 60`           | 0           | saturating           |

Function signature unchanged: `rf_signal_bars(bool link_up, uint32_t hb_age_ms,
uint8_t link_q)`. The link-down / age-timeout (≥1500 ms → 0) and `age_score` logic
are unchanged. Final result stays `min(age_score, retry_score)`.

### 4. Tests — `test/test_rf_signal_bars.c`

Rewrite the `retry_score` boundary cases for the new % semantics (TDD: update tests
first, confirm they fail against the old thresholds, then change `rf_signal_bars`).
Keep the link-down and age-timeout cases as-is. New boundaries to assert (age <200 so
age_score=4, isolating retry_score):
- `q=0` → 4
- `q=15` → 3, `q=16` → 2
- `q=33` → 2, `q=34` → 1
- `q=60` → 1, `q=61` → 0
- `q=100` → 0

## Files

- `main/comm/rf/rf_driver.c` — REG_OBSERVE_TX, accumulate ARC_CNT, new globals.
- `main/comm/rf/rf_driver.h` — extern `rf_tx_retr_sum`, `rf_tx_count` + doc.
- `main/comm/rf/half_scan_task.c` — `link_q` = retry %, reset both accumulators.
- `main/comm/rf/rf_rx_task.c` — `rf_signal_bars` retry_score thresholds + doc comment.
- `test/test_rf_signal_bars.c` — updated boundary tests.

## Validation

- Host tests: `cd test/build && cmake --build . && ./test_runner` — all pass.
- Build `kase_half_right` + (no separate dongle build needed; shared rf_rx_task compiles
  via the same tree) — must compile.
- Hardware (optional): move a half toward the edge of range → bars should step down
  4→3→2→1 *before* the link drops, then 0 when heartbeats time out.

## YAGNI

- No EWMA smoothing — a marginal link should read marginal; a good idle link sends
  0-retry heartbeats → stable 4 bars.
- PLOS_CNT not used (resets only on channel change).
