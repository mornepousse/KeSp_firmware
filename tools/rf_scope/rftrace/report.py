"""Text report of a finished run (the `report` command, also the replay fallback
when there is no terminal)."""

from __future__ import annotations

from .analyzer import Analyzer, name_of
from .stats import NS


def _f(v, spec=".2f", none="-"):
    return none if v is None else format(v, spec)


def render(an: Analyzer, title: str = "", win_s: int = 60) -> str:
    out: list[str] = []
    add = out.append
    if an.t0 is None:
        return "empty run\n"
    dur = (an.last_ns - an.t0) / NS
    end_s = int(dur) + 1
    add(f"rf_scope report {title}".rstrip())
    add(f"  duration {dur:.1f} s   records+ctl {an.n_items}   console lines {an.n_text}   reboots seen {an.reboots}")
    if an.ports:
        add("  sources: " + "  ".join(f"{s}={p}" for s, p in sorted(an.ports.items()) if p))
    add("")
    for e in an.emitters():
        if e == 2:
            continue
        w = an.stats.window(e, 0, end_s, an.fits_for(e))
        fit = an.fits.get(e)
        add(f"{name_of(e)} (emitter {e})")
        add(f"  TX frames {w.frames} (with seq {w.seq_frames}, NOSEQ {w.frames - w.seq_frames})   "
            f"matched {w.matched}   lost {w.lost}   unobserved {w.unobserved}   other channel {w.elsewhere}")
        add(f"  loss {_f(w.loss_pct)} %   dup {_f(w.dup_pct)} %   RPD >= -64 dBm {_f(w.rpd_strong_pct, '.1f')} %   "
            f"MAX_RT {w.max_rt}   timeouts {w.timeouts}   PLOS {w.plos}   FIFO full {w.fifo_full}")
        add("  retries (arc): " + ("  ".join(f"{i}:{n}" for i, n in enumerate(w.arc_hist) if n) or "-")
            + f"   mean {_f(w.retry_mean)}")
        add(f"  latency over the floor (n={w.lat_n}): p50 {_f(w.lat_p50)} ms   p95 {_f(w.lat_p95)} ms   p99 {_f(w.lat_p99)} ms")
        if fit is not None and fit.ready:
            add(f"  clock map to the dongle: slope {(fit.slope - 1) * 1e6:+.1f} ppm   jitter (MAD) {fit.jitter_us / 1000:.2f} ms   "
                f"pairs {fit.n}")
        else:
            add("  clock map: not enough matched pairs")
        add(f"  outages (first lost frame -> next received) > {an.cfg.gap_ms:.0f} ms: {w.gaps} (longest {w.gap_max_ms:.0f} ms)   "
            f"asleep {w.asleep_s:.1f} s ({100 * w.asleep_s / dur if dur else 0:.0f} %)   trace ring drops {w.ring_drops}")
        if an.orphans.get(e) or an.unflagged_dups.get(e):
            add(f"  dongle frames with no TX record: {an.orphans.get(e, 0)}   unflagged duplicates: {an.unflagged_dups.get(e, 0)}")
        add(f"  per {win_s} s window:")
        add("    start    frames  matched  lost   loss%   dup%  RPD%   p50ms  p95ms  p99ms  gaps  asleep_s")
        wins = an.stats.windows(e, win_s, end_s, an.fits_for(e))
        while len(wins) > 1 and wins[-1].frames == 0 and wins[-1].asleep_s == 0:
            wins.pop()
        for ws in wins:
            add(f"    {_mmss(ws.start_s):>6} {ws.frames:8d} {ws.matched:8d} {ws.lost:5d} {_f(ws.loss_pct, '7.2f'):>7} "
                f"{_f(ws.dup_pct, '6.1f'):>6} {_f(ws.rpd_strong_pct, '5.0f'):>5} {_f(ws.lat_p50, '7.2f'):>7} "
                f"{_f(ws.lat_p95, '6.2f'):>6} {_f(ws.lat_p99, '6.2f'):>6} {ws.gaps:5d} {ws.asleep_s:8.1f}")
        add("")
    d = an.stats.dongle_window(0, end_s)
    add("DONGLE")
    add(f"  frames read (periodic counters) {d.frames}   duplicates {d.dups}   trace ring drops {d.drops}   channel {_f(d.channel, 'd')}")
    add("")
    bad = [ev for ev in an.events if ev["kind"] in ("lost", "gap", "reboot", "ring_drop", "giveup", "max_rt")]
    add(f"notable events: {len(bad)}" + (" (first 20)" if len(bad) > 20 else ""))
    for ev in bad[:20]:
        add(f"  {_mmss(an.rel_s(ev['ns']))}  {ev['text']}")
    return "\n".join(out) + "\n"


def _mmss(s: float) -> str:
    return f"{int(s // 60):02d}:{s % 60:04.1f}"
