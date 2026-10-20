# rf-scope — seeing both ends of the radio link, live and after the fact

Status: design (2026-10-20). Requested by Mae after mounting the dongle (REV1,
USB) in the chassis: tools to judge reception quality and latency, with the
view of BOTH ends at once, correlated, in real time and in replay.

## Decisions (Mae)

1. **Sources.** Keyboard side: the ESP-Prog UART on the half under test
   (console UART0 of the left half by default; the right when the ESP-Prog is
   moved). Dongle side: its **CDC ACM** port (binary KS/KR frames — the
   project's CDC rule: no ASCII). The remapping software must not hold the
   dongle port during a capture.
2. **Display:** a **terminal UI** (TUI). Live, and the same TUI replays a
   recording.
3. **Trace mode:** compiled into the normal builds, **off by default**,
   switched on by the tool and **auto-off** after a timeout (default 10 min) —
   no battery cost outside a measurement.

## What already exists

- Every RF frame carries an 8-bit `seq` (`main/comm/rf/rf_packet.h`), per
  emitter: half matrix (left, right), status, events.
- Dongle `RF_STATUS` (0xB3, 51 bytes, decoded by `scripts/kesp_cdc.py
  rfstat`): link state, counters, engine and USB diagnostics. `MONITOR`
  (0xB7), `BATTERY` (0xB6).
- `rf_probe` / `rf_line_test` (bench builds): SPI wiring and radio presence.
- Left console: heartbeat `HB …`, relay counters (`HID->dongle: N delivered`).

## Data model — one record per radio frame

Correlation key: **(emitter, seq, epoch)** — `epoch` disambiguates the 8-bit
wrap (the host keeps a per-emitter 256-frame window and the receiver's arrival
order). Times are each side's own `esp_timer` µs **and** the host's capture
time; latency is computed from host times corrected by a per-run offset fitted
on matched pairs (UART and USB add jitter in the ms range; the tool reports it
rather than hiding it).

**Emitter record** (half, over UART0 while tracing):
`t_us, emitter, seq, kind, payload_len, result (ACK | MAX_RT), arc (retries,
OBSERVE_TX.ARC_CNT), plos_delta (OBSERVE_TX.PLOS_CNT since last), channel,
excursion (left relay PRX→PTX→PRX: yes/no)`.

**Receiver record** (dongle, over CDC while tracing):
`t_us, emitter (pipe → half), seq, kind, rpd (RPD bit: ≥ −64 dBm), channel,
fifo_depth_at_read, delivered (consumed by the fusion engine: yes/no),
dup (same seq already seen)`.

**Periodic records** (both, ~1 Hz while tracing): counters snapshot, battery,
sleep state (a sleeping half does not emit — the TUI must show "asleep", not
"loss").

## Firmware

- **Wire format.** Dongle → host: a new KS_CMD stream (e.g. `KS_CMD_RF_TRACE`
  0xBA: start(duration_s) / stop / data frames pushed unsolicited while on,
  each a batch of fixed-size binary records). Half → host over UART0: the same
  binary record layout, framed KS-style (magic + len + CRC-8) so the host parser
  is shared and console text can interleave without corruption.
- **Switching the half's trace on.** Over UART0 RX (the ESP-Prog TX line): the
  same KS framing, `RF_TRACE start/stop`. No ASCII command parser. On USB the
  half's CDC may accept it too.
- **Cost.** Records are produced in the radio owner / rx task by copying a few
  bytes into a static ring buffer; a low-priority task drains it to UART/CDC.
  No logging in ISR, no malloc, no change of radio timing; the ring overflows
  by counting drops (reported), never by blocking the radio. The trace keeps a
  wake veto? **No** — tracing must observe the real behaviour, sleep included;
  it only stops records while asleep.
- **Auto-off** after the requested duration, and on any reboot.
- **Pure logic, host-tested first:** record encode/decode (shared vectors with
  the host tool), ring buffer drop accounting, seq/epoch matcher, the latency
  offset fit, the per-window statistics.

## Host tool — `tools/rf_scope/` (Python)

- `rf_scope capture --left /dev/ttyUSBx --dongle /dev/ttyACMx [--right …]
  [--duration 10m] [-o run.jsonl]`: switches tracing on on both ends, captures,
  timestamps, writes JSONL (raw records + host times), shows the TUI live.
- `rf_scope replay run.jsonl [--speed 4x]`: the same TUI from a file.
- `rf_scope report run.jsonl`: a text summary (loss %, retries, RPD-strong %,
  latency p50/p95/p99, gaps, per half and per 1-min window).
- `rf_scope scan --dongle …`: channel noise survey (RPD sweep, dongle radio 2
  or between captures), table per channel.
- **TUI panels:** per half — frames/s, loss % (sent-but-never-received), dup %,
  retries histogram, RPD-strong %, latency p50/p95 + sparkline, asleep/awake;
  a scrolling event list (lost frame, MAX_RT, gap > N ms, excursion); the
  per-run offset and its jitter.
- Port access rules: never toggle DTR/RTS on the ESP-Prog (it holds the board
  in reset) — `dtr=rts=False` before open; check `fuser` first and refuse a busy
  port; never open a port the user did not name.

## Stress tests (Mae, 2026-10-20)

Typing by hand cannot load the link reproducibly. A **stress generator** on the
half under test emits synthetic frames on demand:
`RF_STRESS start(kind, rate_hz, count | duration, pattern)` over the same UART0
framing — patterns: constant rate, bursts (N back-to-back then pause), maximum
rate (next frame as soon as the previous TX completes), and a "held keys"
pattern that mimics the 100 ms re-assertions of held keys. Both halves can run
it at once (two ESP-Progs, or the right driven by the dongle-side tool later).

**Hard rule: a stress frame can never reach the host as a keystroke.** Stress
frames use a dedicated kind (or a flag bit) that the dongle counts and traces,
then DROPS before the fusion engine — tested host-side (the engine never sees a
stress frame), and the dongle refuses to forward any frame of that kind even if
tracing is off. On the left, stress never goes through the local engine either.

The tool drives scenarios and reports them like a capture:
`rf_scope stress --left … --dongle … --scenario burst|max|held|soak
[--duration …]` — loss, retries, latency percentiles under load, and the
dongle's queue overflow counter (`transitions_ecrasees`); `soak` runs for hours
and reports per-10-min windows.

## Out of scope (for now)

- End-to-end key→host latency (needs a stimulus and host HID timestamps) —
  noted for a later round; this tool measures the radio hop.
- The REV2 dongle (abandoned, branch `dongle-rev2`).

## Tests

Host: encode/decode against shared vectors (firmware C ↔ Python), ring
buffer, matcher (wrap, dup, reorder, asleep), offset fit, statistics. Bench:
`[smoke:RF trace]` — capture 2 min typing on both halves, loss and latency
sane, trace auto-off, sleep current unchanged with trace off.
