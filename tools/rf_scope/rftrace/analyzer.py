"""The state machine behind capture, replay and report: items in, statistics
and events out. One instance per run; feed it the items in arrival order.

Responsibilities, in the order an item meets them:
  1. unwrap the device time (per SOURCE port: one port = one device);
  2. route: TX (half) / RX (dongle) -> matcher; periodic -> sleep state,
     reboot detection, dongle liveness and channel;
  3. turn the matcher's outcomes into statistics, the per-emitter clock fit
     and the event list the TUI scrolls.
"""

from __future__ import annotations

from collections import deque

from . import codec
from .matcher import Matcher, MatcherConfig
from .offset import ClockFit
from .stats import Stats
from .unwrap import Unwrapper

NAMES = {0: "LEFT", 1: "RIGHT", 2: "DONGLE", 3: "MOUSE"}


def name_of(e) -> str:
    return NAMES.get(e, f"E{e}")


class Analyzer:
    def __init__(self, cfg: MatcherConfig | None = None, expect: tuple = ()) -> None:
        self.cfg = cfg or MatcherConfig()
        self.fits: dict[int, ClockFit] = {}
        self.matcher = Matcher(self.cfg, fits=lambda e: self.fits.get(e))
        self.stats = Stats(0)
        self.t0: int | None = None
        self.last_ns = 0
        self.meta: dict = {}
        self.expect = set(expect)             # emitters whose panel exists even before data
        self._unw: dict[str, Unwrapper] = {}
        self._uptime: dict[str, int] = {}
        self.sleep_since: dict[int, int] = {}
        self.trace: dict[str, str] = {}       # src -> off|starting|on|gave_up|stopping
        self.ports: dict[str, str] = {}
        self.half: dict[int, dict] = {}       # emitter -> {awake, batt_dV, channel, seen_ns}
        self.dongle: dict = {"channel": None, "seen_ns": None, "frames": 0}
        self.events: deque = deque(maxlen=1000)
        self.n_items = 0
        self.n_text = 0
        self.orphans: dict[int, int] = {}
        self.unflagged_dups: dict[int, int] = {}
        self.reboots = 0

    # -- helpers ----------------------------------------------------------------

    def fits_for(self, e: int) -> dict:
        return {e: self.fits[e]} if e in self.fits else {}

    def emitters(self) -> list[int]:
        return sorted(set(self.stats._b) | self.expect | set(self.half))

    def _unwrapper(self, src: str) -> Unwrapper:
        return self._unw.setdefault(src, Unwrapper())

    def _event(self, ns: int, sev: str, e, kind: str, text: str) -> None:
        self.events.append({"ns": ns, "sev": sev, "e": e, "kind": kind, "text": text})

    def rel_s(self, ns: int) -> float:
        return (ns - (self.t0 or ns)) / 1e9

    # -- input ------------------------------------------------------------------

    def feed(self, item: dict) -> None:
        t = item.get("type")
        if t == "meta":
            self.meta = item
            return
        ns = item.get("host_ns", self.last_ns)
        if self.t0 is None:
            self.t0 = ns
            self.stats.t0_ns = ns
        self.last_ns = max(self.last_ns, ns)
        self.n_items += 1
        if t == "rec":
            self.ports.setdefault(item["src"], item.get("port", ""))
            getattr(self, "_rec_" + item["kind"])(item, ns)
        elif t == "ctl":
            self._ctl(item, ns)
        elif t == "text":
            self.n_text += 1

    def _ctl(self, it: dict, ns: int) -> None:
        src, ev = it["src"], it["event"]
        self.ports.setdefault(src, it.get("port", ""))
        state = {"start_sent": "starting", "ack": "on", "giveup": "gave_up", "refused": "refused",
                 "stop_sent": "stopping", "stop_ack": "off"}.get(ev)
        if state:
            self.trace[src] = state
        if ev == "ack":
            self._event(ns, "info", None, "trace", f"{src}: trace on (ack after {it.get('attempts', '?')} request(s))")
        elif ev == "giveup":
            self._event(ns, "bad", None, "giveup", it.get("message", f"{src}: no answer"))
        elif ev == "refused":
            self._event(ns, "bad", None, "giveup", f"{src}: trace start refused (status {it.get('status')})")
        elif ev == "stop_ack":
            self._event(ns, "info", None, "trace", f"{src}: trace off")
        if src == "dongle" and ev in ("stop_sent", "stop_ack", "closed"):
            self._apply(self.matcher.dongle_off(ns), ns)

    def _rec_tx(self, it: dict, ns: int) -> None:
        e = it["emitter"]
        t = self._unwrapper(it["src"]).unwrap(it["t_us"])
        self.stats.add_tx(e, ns, it)
        self.half.setdefault(e, {"awake": True, "batt_dV": 0, "channel": None})
        self.half[e].update(seen_ns=ns, channel=it["channel"])
        if it["result"] == 1:
            self._event(ns, "warn", e, "max_rt", f"{name_of(e)} MAX_RT seq {it['seq']} (arc {it['arc']}, lost {it['plos_delta']})")
        elif it["result"] == 2:
            self._event(ns, "warn", e, "timeout", f"{name_of(e)} TX timeout seq {it['seq']}")
        self._apply(self.matcher.tx(dict(it, t=t, host_ns=ns)), ns)

    def _rec_rx(self, it: dict, ns: int) -> None:
        e = it["emitter"]
        t = self._unwrapper(it["src"]).unwrap(it["t_us"])
        self.dongle["seen_ns"] = ns
        self.dongle["frames"] += 1
        self.stats.add_rx(e, ns, it)
        self._apply(self.matcher.dongle_alive(ns), ns)
        self._apply(self.matcher.rx(dict(it, t=t, host_ns=ns)), ns)

    def _rec_periodic(self, it: dict, ns: int) -> None:
        src = it["src"]
        u = self._unwrapper(src)
        up = it["uptime_s"]
        if src in self._uptime and up + 5 < self._uptime[src]:
            self.reboots += 1
            u.reset()
            if src == "dongle":
                self.fits.clear()
            else:
                self.fits.pop(it["source"], None)
            self._event(ns, "bad", it["source"], "reboot", f"{src} rebooted (uptime {self._uptime[src]} s -> {up} s)")
        self._uptime[src] = up
        u.anchor(it["t_us"], up)
        if it["source"] == codec.EMIT_DONGLE:
            self.dongle.update(seen_ns=ns, channel=it["channel"])
            self.matcher.dongle_channel(it["channel"])
            self._apply(self.matcher.dongle_alive(ns), ns)
            self.stats.dongle_periodic(ns, it["frames"], it["dups"], it["drops"], it["channel"])
            if it["drops"]:
                self._event(ns, "bad", 2, "ring_drop", f"DONGLE trace ring dropped {it['drops']} record(s)")
            return
        e = it["source"]
        h = self.half.setdefault(e, {"awake": True, "batt_dV": 0, "channel": None})
        h.update(seen_ns=ns, batt_dV=it["batt_dV"], channel=it["channel"])
        if it["drops"]:
            self.stats.ring_drops(e, ns, it["drops"])
            self._event(ns, "bad", e, "ring_drop", f"{name_of(e)} trace ring dropped {it['drops']} record(s)")
        st = it["state"]
        if st == codec.ST_SLEEPING:
            u.mark_sleep()
            h["awake"] = False
            self.sleep_since[e] = ns
            self.matcher.half_sleep(e, ns)
            self._event(ns, "info", e, "sleep", f"{name_of(e)} falls asleep")
        elif st == codec.ST_WOKE or (st == codec.ST_AWAKE and e in self.sleep_since):
            h["awake"] = True
            since = self.sleep_since.pop(e, None)
            self.matcher.half_wake(e, ns)
            if since is not None:
                self.stats.asleep(e, since, ns)
                self._event(ns, "info", e, "wake", f"{name_of(e)} wakes after {(ns - since) / 1e9:.1f} s")
        else:
            h["awake"] = True

    # -- outcomes ----------------------------------------------------------------

    def _apply(self, evs: list[dict], ns: int) -> None:
        for ev in evs:
            k, e = ev["ev"], ev.get("e")
            if k in ("matched", "recovered"):
                fit = self.fits.setdefault(e, ClockFit())
                fit.add(ev["tx_t"], ev["rx_t"])
                self.stats.outcome(e, ev["tx_host_ns"], k)
                self.stats.pair(e, ev["tx_host_ns"], ev["tx_t"], ev["rx_t"], fit)
                if k == "recovered":
                    self._event(ns, "info", e, "recovered", f"{name_of(e)} seq {ev['seq']} arrived late (counted lost, now recovered)")
            elif k == "lost":
                self.stats.outcome(e, ev["tx_host_ns"], "lost")
                self._event(ns, "warn", e, "lost", f"{name_of(e)} LOST seq {ev['seq']} (pkt {ev['pkt_kind']}): sent, never received")
            elif k in ("unobserved", "elsewhere"):
                self.stats.outcome(e, ev["tx_host_ns"], k)
            elif k == "gap":
                self.stats.gap(e, ev["host_ns"], ev["dur_ms"], ev["missing"])
                self._event(ns, "warn", e, "gap", f"{name_of(e)} outage {ev['dur_ms']:.0f} ms ({ev['missing']} frame(s) never seen by the dongle)")
            elif k == "dup" and not ev["flagged"]:
                self.unflagged_dups[e] = self.unflagged_dups.get(e, 0) + 1
            elif k == "orphan":
                self.orphans[e] = self.orphans.get(e, 0) + 1

    def finish(self) -> None:
        if self.t0 is None:
            return
        self._apply(self.matcher.finish(self.last_ns), self.last_ns)
        for e, since in list(self.sleep_since.items()):
            self.stats.asleep(e, since, self.last_ns)
        self.sleep_since.clear()
