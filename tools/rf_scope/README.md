# rf_scope — both ends of the KeSp radio link

Host tool for the rf-scope trace (design: `docs/superpowers/specs/2026-10-20-rf-scope-design.md`,
plan: `docs/superpowers/plans/2026-10-20-rf-scope.md`). It switches the trace on
at the dongle (CDC) and at the halves (UART0 through an ESP-Prog), timestamps
every record on arrival, writes a JSONL run file, matches each frame a half
sent to the frame the dongle read, and shows loss, retries, signal and delay
live (curses) or afterwards (`replay`, `report`).

## Install

Python 3.10+, standard library plus **pyserial** (and `curses`, in the standard
library on Linux/macOS). No package to install: run it in place.

```sh
nix shell --impure --expr 'with import <nixpkgs> {}; python3.withPackages (p: [p.pytest p.pyserial])' \
  --command tools/rf_scope/rf_scope --help
# or, with pyserial available:
python3 tools/rf_scope/rf_scope.py --help      # same thing: rf_scope, rf_scope.py
cd tools/rf_scope && python3 -m rftrace --help # same thing
```

## Commands

Shortest form — the dongle and the ESP-Prog are found by their USB names
(`/dev/serial/by-id/usb-KaSe_KaSe_Dongle*`, the ESP-Prog's UART channel `if01`),
the ESP-Prog is taken as the LEFT half unless `--half right`, and the wrapper
fetches pyserial through `nix shell` when the system python lacks it:

```sh
rf_scope capture                      # 10 min, TUI, run file rf_run_<date>.jsonl
rf_scope capture --duration 2m
rf_scope stress --scenario burst
rf_scope report rf_run_<date>.jsonl
```

Explicit ports always win over detection (and a busy port is still refused):

```sh
# Capture 10 minutes, dongle + left half (ESP-Prog on /dev/ttyUSB0), TUI on:
rf_scope capture --dongle /dev/ttyACM0 --left /dev/ttyUSB0 --duration 10m -o run.jsonl
# Both halves (two ESP-Progs), no TUI (a status line every 5 s on stderr):
rf_scope capture --dongle /dev/ttyACM0 --left /dev/ttyUSB0 --right /dev/ttyUSB1 --duration 2m --no-tui

# Load the link: the left's stress generator, 30 s of bursts, traced at both ends:
rf_scope stress --dongle /dev/ttyACM0 --left /dev/ttyUSB0 --scenario burst --duration 30
rf_scope stress --dongle /dev/ttyACM0 --left /dev/ttyUSB0 --right /dev/ttyUSB1 --scenario max
rf_scope stress --dongle /dev/ttyACM0 --left /dev/ttyUSB0 --scenario rate --rate 200 -o rate200.jsonl
rf_scope stress --dongle /dev/ttyACM0 --left /dev/ttyUSB0 --scenario soak --duration 4h --no-tui

rf_scope replay run.jsonl --speed 4          # the same TUI, fed from the file
rf_scope replay run.jsonl --dump 110x34 --at 60   # one rendered frame as text (no terminal needed)
rf_scope report run.jsonl [--window 60]      # text summary + per-window table (+ "stress" section)
```

`capture` ends on Ctrl-C or when `--duration` elapses; either way it sends
**stop** to every source, waits (4 s at most) for the acks, prints the report
and leaves the boards with the trace off. The firmware also switches the trace
off by itself (`duration + 30 s`, at most 65535 s) and at every reboot, so a
crashed tool cannot leave a half tracing.

TUI keys: `q` quit, `p` pause the event scroll, `r` reset the windows (the
panels then count from now). It degrades on a small terminal: below 76 columns
the halves stack, below the height of the full panels they shrink to one line
each, below 40x6 it just says so.

### Stress runs

`stress` is a capture (same ports, same safety, same TUI, same run file) that
also drives the **stress generator** of each half named (`KS_CMD_RF_STRESS`,
0xBB, on its UART0 — `docs/CDC_BINARY_PROTOCOL.md`). Scenarios:

| `--scenario` | firmware pattern | default |
|---|---|---|
| `burst` | `--burst-len` frames back to back, then `--burst-pause` ms | 10 frames / 200 ms, 30 s |
| `max`   | the next frame as soon as the previous TX completed (≥ 200 µs gap) | 30 s |
| `held`  | one frame every 100 ms, like a held key's re-assertion | 30 s |
| `rate`  | `--rate` frames per second on a grid (1..2000) | 50 Hz, 30 s |
| `soak`  | `--rate` frames per second for hours; report windows of 10 min | 20 Hz, 1 h |

`--count N` ends the run after N frames (the firmware still caps a count-only
run at 600 s). The order is fixed: trace on (dongle and halves) → stress start
on each half, **repeated until its `KR BB` ack** (a half hears UART0 only
awake) → the run → **stress stop first**, then the trace stop, so the last
stress frames and records are in the file. The firmware ends the run by itself
at its duration (and the tool captures 3 s more), on its count, and if the
half sleeps; while it runs the half posts the sleep veto `stress` (`HB …
vetos=stress`). Parameters out of range are refused before any port is opened;
a half that refuses (`ERR_UNKNOWN`: not paired, no radio) is reported
(`stress_refused`) and the capture goes on.

**A stress frame can never be typed on the host.** It is of its own kind
(`PKT_TYPE_STRESS` 0xA); the dongle drops it before every handler, traced or
not, and counts it (periodic `stress`). The smoke item "RF stress" checks it
with a text editor focused during every scenario.

The report adds a **stress** section built from the stress frames alone
(kind 0xA; matched exactly like the full run): per half, the time the run was
active, frames **offered** (TX records, per second) against the rate
**requested**, frames **delivered** (matched at the dongle, per second),
**loss**, ACKs, MAX_RT, retries, **latency under load** (p50/p95/p99), the
trace ring drops of both ends (records lost: the counts become lower bounds),
the dongle's `stress` counter (frames it dropped before the engine) against
the half's, and a per-window table (10 min for a soak). The fusion queue's
overflow counter (`transitions_ecrasees`) is not in the trace: read it with
`scripts/kesp_cdc.py rfstat` right after the run.

At `max` a half sends far more frames than its 115200-baud trace can carry
(~800 TX records/s): expect half ring drops, and read loss on the frames that
were recorded.

### Port safety

* Only the ports named on the command line are ever opened.
* Before opening anything, every port is checked with `fuser` (or a
  `/proc/*/fd` scan when `fuser` is missing); a port held by another process is
  refused with the holder's name and pid, and **nothing** is opened.
* Half ports (`--left`, `--right`) are opened with `dtr=False; rts=False` set
  on the unopened `serial.Serial` and never touched again: an ESP-Prog wires
  them to EN / IO0. (The kernel may still raise both for an instant inside
  `open(2)`, before pyserial can lower them; that is outside userland's reach.)
  The dongle's CDC port keeps pyserial's defaults, like `scripts/kesp_cdc.py`:
  **DTR must stay asserted** there, the dongle ties the trace to it (DTR dropped
  = trace stopped and flushed), and the host must keep reading (a reader thread
  per port does).
* Do not run the remapping software, `kesp_cdc.py` or a terminal on the same
  port during a capture.

### Starting a half that sleeps

A half hears UART0 only while awake (no UART driver, no wake source: the sleep
current stays at its baseline). The start request is therefore **repeated every
250 ms** until the `KR BA` ack (or, if the ack got lost among console
text, the first trace frame) comes back. The ack carries the session number
(every periodic record of the run repeats it) and the duration granted; an
error reply (`refused`) ends the attempt at once. If nothing answers in 20 s the tool
says so (`press a key on the half to wake it`) and goes on with the other
sources. The dongle never sleeps and answers at once.

## Run file (JSONL)

One JSON object per line, self-contained:

| `type` | fields |
|---|---|
| `meta` | `version`, `sources` (role -> port), `started`, `duration_s`; a stress run adds `scenario` (`name`, `params`) |
| `rec` | `src` (`dongle`/`left`/`right`), `port`, `host_ns`, `kind` (`tx`/`rx`/`periodic`) and the record's fields exactly as in `main/comm/rf/rf_trace.h` |
| `text` | `src`, `host_ns`, `text`: a console line of a half (kept so the run explains itself) |
| `ctl` | `src`, `host_ns`, `event`: `start_sent`, `ack` (`attempts`, `session`, `remaining_s`), `refused` (`status`), `giveup` (`message`), `stop_sent`, `stop_ack` (`session`), `closed`; stress runs: `stress_start_sent` (`scenario`, `params`), `stress_ack` (`attempts`, `session`, `remaining_s`), `stress_refused` (`status`, `message`), `stress_giveup`, `stress_stop_sent`, `stress_stop_ack` |

`host_ns` is `time.monotonic_ns()` when the chunk carrying the record was read
(records of one chunk share it). A truncated last line (killed capture) is
ignored on read.

## What is measured, and what it is not

**Matching.** A TX record (half) and an RX record (dongle) are the same frame
when `(emitter, pkt_kind, seq)` agree. The 8-bit seq wraps every 256 frames, so
the tool also requires the two to be *plausible neighbours in time*: through
the fitted clock map once it exists (excess delay between -20 ms and 300 ms),
through host arrival times (+-1 s) before that. A TX whose seq comes round again
while the previous frame with it is still unmatched closes the old one. An RX
that arrives before its TX line (the half's UART is slower than the dongle's USB)
waits as an orphan and is claimed when the TX arrives. Frames flagged `NOSEQ`
(HIDREPORT, SYNC_REQ...) carry no seq: they are counted as frames, retries and
MAX_RT, but never matched, never lost.

**Device time.** `t_us` is 32 bits and wraps every 71.6 min. Each source's
timeline is unwrapped by small signed steps, anchored at every periodic record
by its `uptime_s` (so a sleep of hours is handled), and a SLEEPING record makes
the next step forward-only. A drop of `uptime_s` is a reboot: that source's
timeline and clock fit restart.

**Loss %** = lost / (matched + lost), per half, over frames carrying a seq. A
frame is *lost* only when (1) the half sent it, (2) the dongle was tracing over
the whole `[tx, tx + 1 s]` (it was heard at least every 3 s, with the dongle's
own host clock closing the window: a stalled USB stream closes nothing), and
(3) nothing arrived. Otherwise it is *unobserved* (dongle not tracing yet) or
*other channel* (the half transmitted on a channel other than the dongle's, e.g.
the right talking to the left): neither counts as loss. A sleeping half sends
nothing, so no loss accrues while it sleeps; an outage never spans a sleep.
A frame declared lost that shows up within 5 s is *recovered* and moved to
matched. MAX_RT with the frame received anyway (lost ACK) is a match.

**Outage** = the unbroken run of frames the dongle never saw right before one it
did, measured from the first of them to the next received one (default
threshold 100 ms): how long the receiver was in the dark. One lost frame of a
held key (re-asserted every 100 ms) is therefore a ~100-200 ms outage.

**Dup %** = dongle RX records flagged duplicate / RX records. **RPD %** = RX
records with the RPD bit: the *last valid packet* was at least -64 dBm (the
nRF24 latches it; it is a coarse indicator, not RSSI). **Retries** = the `arc`
histogram (OBSERVE_TX.ARC_CNT: retransmissions of that frame), **PLOS** = the
packets-lost counter delta (the chip's 4-bit counter saturates at 15 until the
next channel write: on a very bad link count failures from MAX_RT, not from
PLOS). **FIFO full** = RX records read at depth 3: a frame
arriving then was not acknowledged.

### Latency, and why it is relative

The two chips have unrelated clocks: the dongle's `t_us` and a half's differ by
their boot times (hours, days) and drift apart by tens of ppm. For each matched
pair the tool has `x` = the half's TX stamp and `y` = the dongle's RX stamp, so
`y = c + d*x + delay`. Everything the tool says about delay comes from that
equation:

1. **slope d** is the Theil-Sen estimate (median of pairwise slopes on up to 160
   evenly spread pairs, at least 1/8 of the run apart): the crystals' drift,
   robust to outliers. Under a 5 s span it is 1. It is only the *baseline*: the
   offset between two chips is not a straight line over minutes (2026-10-21
   soak: -210 ppm for 290 s, flat for 125 s, -210 ppm again; the single line
   left +-45 ms of residual and a 11.8 ms p50 on a link that gives 0.7 ms in 20 s).
2. **c** is a piecewise-linear curve through the median residual of each group of
   consecutive 5 s bins (merged until >= 64 pairs), so a slow or changing drift is
   followed; a short run has one group, i.e. a constant. **jitter** is the MAD of
   the residuals around the curve (the typical spread of the delay, unscaled).
3. **latency** printed everywhere is `residual - 1st percentile of residuals`:
   *how much later than the run's fastest frames the dongle read this one*,
   after removing the clock offset and drift. The fit is refreshed as pairs arrive
   (and `report` uses the final one for every pair); memory is bounded by
   decimation, so a multi-hour soak is fine.

What it **is**: the spread of airtime + retries + the ACK path + the time the
dongle's task takes to read its FIFO — i.e. the jitter and the tail (p95/p99)
that cause a late keystroke. What it **is not**:

* *Not an absolute one-way latency.* Without a common clock the constant part
  of the delay (a few hundred microseconds of airtime and SPI) cannot be
  separated from the clock offset; the floor is set to zero. Add roughly
  0.3-1 ms for the real minimum.
* The half stamps `t_us` *after* the transmission completes (ACK received, or
  MAX_RT), the dongle stamps when its task *reads the FIFO*. Retries delay the
  first and a busy dongle task delays the second; both are in the number.
* Host arrival times (UART/USB batching: an FTDI latency timer of 16 ms is
  common) are used only for matching and liveness, never for latency.
* Fewer than 8 matched pairs: **no clock map**, so latency prints `-` (and the
  report says why) instead of the meaningless difference of two boot times.
  A drift that changes within a group (a few seconds) still widens the jitter; a reboot of
  either end restarts the fit, and the first ~8 pairs after it are matched on
  host time only.
* End-to-end key-to-host latency is out of scope (it needs a stimulus and host
  HID timestamps).

**Residual by retries** (report, per emitter): the median residual of the
clock-mapped frames that needed `k` retries, minus that of the frames sent at
the first attempt (`arc` 0), e.g. `arc1 -0.77 ms, arc2 -1.52 ms`. The half
stamps `t_us` after the last attempt, the dongle when it reads its FIFO: if the
dongle got the *retransmission*, arc=k frames sit at about 0 like arc=0; if
they sit about `-k x (ARD + airtime)` below, the dongle had received the
*first* attempt and it was the ACKs that were lost (this is how it was shown
on 2026-10-21 with ARD 750 us). Needs >= 5 frames per arc.

**Asleep** = time between a SLEEPING and the next WOKE record (host arrival
times), or to the end of the run. **Ring drops** = records the firmware's trace
ring dropped (`drops` of the periodic records): the run then has holes, and
frames missing from them show as *unobserved/orphan*, not as loss only if the
TX record itself survived.

## Tests and demo data

```sh
nix shell --impure --expr 'with import <nixpkgs> {}; python3.withPackages (p: [p.pytest p.pyserial])' \
  --command python3 -m pytest tools/rf_scope/tests -q
```

`scripts/check.sh --fast` runs them through the `rf-scope-pytest` brick when
`python3` can import pytest (otherwise it announces the skip).

`tests/fake_run.py` writes a synthetic run (two halves typing, sleeping,
8-bit seq wrap, 32-bit `t_us` wrap, drift, losses, a burst outage, duplicates,
a ring drop); `samples/demo_run.jsonl` is one of them (`--seed 1 --seconds 120
--wrap-at 50`), so `replay` and `report` can be tried without hardware:

```sh
rf_scope replay tools/rf_scope/samples/demo_run.jsonl --speed 8
rf_scope report tools/rf_scope/samples/demo_run.jsonl
```
