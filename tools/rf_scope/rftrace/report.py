"""Text report of a finished run (the `report` command, also the replay fallback
when there is no terminal)."""

from __future__ import annotations

from . import stress as stress_mod
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
    san = getattr(an, "stress_view", None)
    if san is not None:
        add("")
        out.extend(render_stress(san, win_s))
    return "\n".join(out) + "\n"


def _params_line(p: dict | None) -> str:
    if not p:
        return "parameters unknown"
    parts = [f"pattern {p.get('pattern')}"]
    if p.get("pattern") == "rate":
        parts.append(f"{p.get('rate_hz')} Hz")
    if p.get("pattern") == "burst":
        parts.append(f"{p.get('burst_len')} frames / {p.get('burst_pause_ms')} ms pause")
    if p.get("count"):
        parts.append(f"count {p['count']}")
    parts.append(f"{p.get('duration_s')} s")
    return ", ".join(parts)


def render_stress(san: Analyzer, win_s: int = 60) -> list[str]:
    """The stress section: stress frames only (kind 0xA), matched like any frame.
    `san` is an Analyzer(only_pkt_kind=0xA) fed the whole run."""
    out: list[str] = []
    add = out.append
    scen = (san.meta or {}).get("scenario") or {}
    params = scen.get("params") or next((v.get("params") for v in san.stress.values() if v.get("params")), None)
    name = scen.get("name") or (params or {}).get("scenario", "?")
    add(f"STRESS  scenario {name}  ({_params_line(params)})")
    add(f"  dongle dropped (stress counter) {san.stress_counter.get('dongle', 0)} frames before the engine "
        f"(periodic `stress`, all halves): none reached the host")
    add("  fusion queue overflow (transitions_ecrasees): not in the trace — read it with "
        "`scripts/kesp_cdc.py rfstat` right after the run")
    if san.t0 is None:
        return out
    end_s = int((san.last_ns - san.t0) / NS) + 1
    d = san.stats.dongle_window(0, end_s)
    req = stress_mod.requested_rate(params or {})
    for e in sorted(san.tx_span):
        a, b = san.tx_span[e]
        lo_s, hi_s = (a - san.t0) / NS, (b - san.t0) / NS
        active = max(hi_s - lo_s, 1e-9)
        w = san.stats.window(e, 0, end_s, san.fits_for(e))
        acked = w.frames - w.max_rt - w.timeouts
        src = {0: "left", 1: "right"}.get(e, "")
        sess = san.stress.get(src, {}).get("session")
        add(f"{name_of(e)} (emitter {e})  stress session {sess if sess is not None else '-'}")
        add(f"  active {active:.1f} s (first -> last stress TX record)")
        add(f"  offered {w.frames} frames ({w.frames / active:.1f}/s)   "
            + (f"requested {req:.1f}/s" if req is not None else "requested: as fast as the pattern allows"))
        add(f"  delivered {w.matched} frames matched at the dongle ({w.matched / active:.1f}/s)   "
            f"loss {_f(w.loss_pct)} %   (lost {w.lost}, unobserved {w.unobserved}, other channel {w.elsewhere})")
        add(f"  half acked {acked}   MAX_RT {w.max_rt}   timeouts {w.timeouts}   PLOS {w.plos}   "
            f"retries mean {_f(w.retry_mean)}   arc: " + ("  ".join(f"{i}:{n}" for i, n in enumerate(w.arc_hist) if n) or "-"))
        add(f"  latency under load (n={w.lat_n}): p50 {_f(w.lat_p50)} ms   p95 {_f(w.lat_p95)} ms   "
            f"p99 {_f(w.lat_p99)} ms   FIFO full {w.fifo_full}   dup {_f(w.dup_pct)} %")
        add(f"  half sent (stress counter) {san.stress_counter.get(e, 0)}   "
            f"trace ring drops: half {w.ring_drops}, dongle {d.drops}"
            + ("   (records lost: the counts above are lower bounds)" if (w.ring_drops or d.drops) else ""))
        add(f"  per {win_s} s window:")
        add("    start    frames  matched  lost   loss%  MAX_RT   p50ms  p95ms  p99ms")
        wins = san.stats.windows(e, win_s, end_s, san.fits_for(e))
        while len(wins) > 1 and wins[-1].frames == 0:
            wins.pop()
        for ws in wins:
            add(f"    {_mmss(ws.start_s):>6} {ws.frames:8d} {ws.matched:8d} {ws.lost:5d} {_f(ws.loss_pct, '7.2f'):>7} "
                f"{ws.max_rt:7d} {_f(ws.lat_p50, '7.2f'):>7} {_f(ws.lat_p95, '6.2f'):>6} {_f(ws.lat_p99, '6.2f'):>6}")
    return out


def _mmss(s: float) -> str:
    return f"{int(s // 60):02d}:{s % 60:04.1f}"
