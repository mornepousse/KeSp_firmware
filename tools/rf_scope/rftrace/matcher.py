"""Matching of the halves' TX records to the dongle's RX records.

Key: (emitter, pkt_kind, seq8). The 8-bit sequence wraps every 256 frames, so
a key alone is ambiguous; three things keep it honest:

* a half sends at most one *pending* frame per key: when a TX arrives with a
  seq whose previous frame is still unmatched, the old one is closed first
  (the receiver cannot still be coming back for a frame sent 256 frames ago);
* an RX only pairs with a TX whose time is plausible: through the fitted
  clock map once it is ready (excess delay in [-20 ms, max_latency]), through
  host arrival times (+-window) before that;
* an RX that finds no TX is kept as an *orphan* for a window (the half's UART
  is slower than the dongle's USB, the RX often arrives first).

Loss is a statement about the radio, so it needs three conditions: the TX
carries a seq (NOSEQ frames are counted elsewhere, never matched), the dongle
was tracing over the whole [tx, tx + window] (`_Liveness`), and the window is
closed by the DONGLE's host clock — a stalled USB stream closes nothing.
A half that sleeps sends nothing, so nothing is pending against it: no loss
accrues while it sleeps (`half_sleep` only ends the current run of failures for the gap detector).

Events are plain dicts with an "ev" key; the analyzer feeds statistics and the
event list from them. Times: `*_t` are unwrapped device microseconds,
`*host_ns` the host's monotonic arrival times.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass

NOSEQ = 0x02
RXF_DUP = 0x02
RXF_NOSEQ = 0x04         # format v2: the dongle says the kind carries no seq


@dataclass
class MatcherConfig:
    window_ms: float = 1000.0         # how long the dongle has to show a TX's frame
    max_latency_ms: float = 300.0     # plausibility bound once the clock map is fitted
    alive_gap_ms: float = 3000.0      # dongle silence that ends a tracing stretch
    alive_slack_ms: float = 50.0      # a TX may precede the first dongle record by this much
    gap_ms: float = 100.0             # outage worth an event
    channel_filter: bool = True       # TX on another channel than the dongle's: not a loss
    late_ms: float = 5000.0           # an RX this late after a "lost" still recovers it


class _Entry:
    __slots__ = ("tx", "idx", "state", "rx_n", "tx_n", "key", "rx", "epoch")

    def __init__(self, tx: dict, idx: int, key: tuple, epoch: int) -> None:
        self.tx, self.idx, self.key, self.epoch = tx, idx, key, epoch
        self.state = "pending"
        self.rx_n = 0
        self.tx_n = 1
        self.rx: dict | None = None


class _Liveness:
    """Stretches of host time over which the dongle was heard at least every alive_gap."""

    def __init__(self, gap_ns: int) -> None:
        self.gap_ns = gap_ns
        self.segs: list[list[int]] = []
        self.closed = True

    def heard(self, ns: int) -> None:
        if self.segs and not self.closed and ns - self.segs[-1][1] <= self.gap_ns:
            self.segs[-1][1] = max(self.segs[-1][1], ns)
        else:
            self.segs.append([ns, ns])
            self.closed = False

    def off(self, ns: int) -> None:
        if self.segs and not self.closed:
            self.segs[-1][1] = max(self.segs[-1][1], ns)
        self.closed = True

    def covers(self, a: int, b: int, slack_ns: int) -> bool:
        for s, e in reversed(self.segs):
            if s - slack_ns <= a and e >= b:
                return True
            if e < a:
                break
        return False


class Matcher:
    def __init__(self, cfg: MatcherConfig | None = None, fits=None) -> None:
        """`fits`: callable emitter -> object with .ready and .excess_us(x, y), or None."""
        self.cfg = cfg or MatcherConfig()
        self.fits = fits
        w = int(self.cfg.window_ms * 1e6)
        self._w = w
        self._live = _Liveness(int(self.cfg.alive_gap_ms * 1e6))
        self._frames: dict[tuple, _Entry] = {}      # (e, kind, seq) -> latest entry
        self._last_seq: dict[tuple, int] = {}       # (e, kind) -> seq of the previous TX
        self._idx: dict[int, int] = {}              # emitter -> next logical index
        self._by_idx: dict[tuple, _Entry] = {}      # (e, idx) -> entry (bounded)
        self._pending: deque[_Entry] = deque()
        self._orphans: deque[dict] = deque()
        self._lost: dict[tuple, _Entry] = {}        # recently closed as lost, to recover
        self._epoch: dict[int, int] = {}            # emitter -> count of sleeps (a run of failures never spans one)
        self._chan: int | None = None
        self._noseq_kinds: set[int] = {5, 9}        # HIDREPORT, SYNC_REQ carry no seq; more are learned
        self._now = 0

    # -- helpers -------------------------------------------------------------

    def dongle_channel(self, ch: int) -> None:
        self._chan = ch

    def _gate(self, tx: dict, rx: dict) -> bool:
        fit = self.fits(tx["emitter"]) if self.fits else None
        if fit is not None and fit.ready:
            ex = fit.excess_us(tx["t"], rx["t"])
            return -20_000 <= ex <= self.cfg.max_latency_ms * 1000
        return abs(rx["host_ns"] - tx["host_ns"]) <= self._w

    def _match(self, ent: _Entry, rx: dict, out: list, ev: str = "matched") -> None:
        ent.state, ent.rx = "matched", rx
        ent.rx_n = 1
        tx = ent.tx
        e = tx["emitter"]
        out.append({"ev": ev, "e": e, "tx_t": tx["t"], "rx_t": rx["t"],
                    "tx_host_ns": tx["host_ns"], "rx_host_ns": rx["host_ns"],
                    "seq": tx["seq"], "arc": tx["arc"], "rpd": rx.get("rpd", 0)})
        run: list[_Entry] = []                  # the unbroken run of failures right before this frame
        i = ent.idx - 1
        while True:
            m = self._by_idx.get((e, i))
            if m is None or m.epoch != ent.epoch or m.state not in ("pending", "lost"):
                break
            run.append(m)
            i -= 1
        if run:
            first = run[-1]
            dur_ms = (tx["t"] - first.tx["t"]) / 1000.0     # first frame the dongle never saw -> this one
            if dur_ms >= self.cfg.gap_ms:
                out.append({"ev": "gap", "e": e, "missing": len(run), "dur_ms": dur_ms,
                            "host_ns": first.tx["host_ns"]})

    def _close(self, ent: _Entry, now: int, out: list) -> None:
        """The window of a still-pending entry is over."""
        if ent.state != "pending":
            return
        tx = ent.tx
        if self.cfg.channel_filter and self._chan is not None and tx.get("channel", self._chan) != self._chan:
            ent.state = "elsewhere"
        elif self._live.covers(tx["host_ns"], min(tx["host_ns"] + self._w, now), int(self.cfg.alive_slack_ms * 1e6)):
            ent.state = "lost"
            self._lost[(ent.key)] = ent
        else:
            ent.state = "unobserved"
        out.append({"ev": ent.state, "e": tx["emitter"], "seq": tx["seq"],
                    "tx_host_ns": tx["host_ns"], "tx_t": tx["t"], "pkt_kind": tx["pkt_kind"]})

    def _advance(self, now: int, out: list) -> None:
        self._now = max(self._now, now)
        while self._pending and self._pending[0].tx["host_ns"] + self._w <= now:
            self._close(self._pending.popleft(), now, out)
        while self._orphans and self._orphans[0]["host_ns"] + self._w <= now:
            o = self._orphans.popleft()
            out.append({"ev": "orphan", "e": o["emitter"], "seq": o["seq"], "rx_host_ns": o["host_ns"]})
        horizon = now - int(self.cfg.late_ms * 1e6)
        for k in [k for k, v in self._lost.items() if v.tx["host_ns"] < horizon]:
            del self._lost[k]

    # -- inputs ---------------------------------------------------------------

    def tx(self, obs: dict) -> list[dict]:
        out: list[dict] = []
        e = obs["emitter"]
        if obs["flags"] & NOSEQ:
            self._noseq_kinds.add(obs["pkt_kind"])
            out.append({"ev": "noseq", "e": e, "tx_host_ns": obs["host_ns"]})
            return out
        key2 = (e, obs["pkt_kind"])
        key = key2 + (obs["seq"],)
        old = self._frames.get(key)
        if old is not None and self._last_seq.get(key2) == obs["seq"]:
            old.tx_n += 1                                # same frame sent again
            out.append({"ev": "resend", "e": e, "seq": obs["seq"]})
            return out
        self._last_seq[key2] = obs["seq"]
        if old is not None and old.state == "pending":
            try:
                self._pending.remove(old)
            except ValueError:
                pass
            self._close(old, self._now, out)
        idx = self._idx.get(e, 0)
        self._idx[e] = idx + 1
        ent = _Entry(obs, idx, key, self._epoch.get(e, 0))
        self._frames[key] = ent
        self._by_idx[(e, idx)] = ent
        self._by_idx.pop((e, idx - 512), None)
        claimed = [o for o in self._orphans
                   if (o["emitter"], o["pkt_kind"], o["seq"]) == key and self._gate(obs, o)]
        if not claimed:
            self._pending.append(ent)
            return out
        for n, o in enumerate(claimed):          # the RX side ran ahead of the UART: first = match, rest = dups
            self._orphans.remove(o)
            if n == 0:
                self._match(ent, o, out)
            else:
                ent.rx_n += 1
                out.append({"ev": "dup", "e": o["emitter"], "seq": o["seq"],
                            "flagged": bool(o["flags"] & RXF_DUP), "rx_host_ns": o["host_ns"]})
        return out

    def rx(self, obs: dict) -> list[dict]:
        out: list[dict] = []
        if obs.get("flags", 0) & RXF_NOSEQ or obs["pkt_kind"] in self._noseq_kinds:
            out.append({"ev": "noseq_rx", "e": obs["emitter"], "rx_host_ns": obs["host_ns"]})
            return out
        key = (obs["emitter"], obs["pkt_kind"], obs["seq"])
        ent = self._frames.get(key)
        if ent is not None and self._gate(ent.tx, obs):
            if ent.state == "pending":
                try:
                    self._pending.remove(ent)
                except ValueError:
                    pass
                self._match(ent, obs, out)
            elif ent.state in ("matched",):
                ent.rx_n += 1
                out.append({"ev": "dup", "e": obs["emitter"], "seq": obs["seq"],
                            "flagged": bool(obs["flags"] & RXF_DUP), "rx_host_ns": obs["host_ns"]})
            elif ent.state == "lost":
                self._lost.pop(key, None)
                self._match(ent, obs, out, "recovered")
            else:                                       # unobserved / elsewhere: it did arrive
                self._match(ent, obs, out)
        else:
            lost = self._lost.get(key)
            if lost is not None and self._gate_late(lost.tx, obs):
                self._lost.pop(key, None)
                self._match(lost, obs, out, "recovered")
            else:
                self._orphans.append(obs)
        return out

    def _gate_late(self, tx: dict, rx: dict) -> bool:
        fit = self.fits(tx["emitter"]) if self.fits else None
        if fit is not None and fit.ready:
            return self._gate(tx, rx)
        return abs(rx["host_ns"] - tx["host_ns"]) <= self.cfg.late_ms * 1e6

    def dongle_alive(self, host_ns: int) -> list[dict]:
        out: list[dict] = []
        self._live.heard(host_ns)
        self._advance(host_ns, out)
        return out

    def dongle_off(self, host_ns: int) -> list[dict]:
        out: list[dict] = []
        self._advance(host_ns, out)
        self._live.off(host_ns)
        return out

    def half_sleep(self, emitter: int, host_ns: int) -> None:
        self._epoch[emitter] = self._epoch.get(emitter, 0) + 1

    def half_wake(self, emitter: int, host_ns: int) -> None:
        self._epoch[emitter] = self._epoch.get(emitter, 0) + 1

    def finish(self, host_ns: int | None = None) -> list[dict]:
        out: list[dict] = []
        end = max(self._now, host_ns or 0)
        self._advance(end, out)
        return out
