"""Terminal UI: a pure renderer (testable, dumpable) and a thin curses loop.

`render()` returns lines of (text, style) spans from an Analyzer; `plain()`
flattens them, which is how a "screenshot" is produced without a terminal.
Keys: q quit, p pause the event scroll, r reset the windows.
"""

from __future__ import annotations

import time
from typing import Callable

from .analyzer import Analyzer, name_of

SPARK = " ▁▂▃▄▅▆▇█"
WINDOW_S = 60
PANEL_H = 8

Span = tuple[str, str]


def sparkline(vals: list, width: int, vmin_scale: float = 5.0) -> str:
    if width <= 0:
        return ""
    n = len(vals)
    group = max(1, -(-n // width))
    cells = []
    for i in range(0, n, group):
        chunk = [v for v in vals[i:i + group] if v is not None]
        cells.append(max(chunk) if chunk else None)
    top = max([vmin_scale] + [c for c in cells if c is not None])
    out = []
    for c in cells:
        out.append("·" if c is None else SPARK[1 + min(7, int(c / top * 7.999))])
    return "".join(out)


def _fmt(v, spec=".1f", none="-"):
    return none if v is None else format(v, spec)


def _clip(spans: list[Span], width: int) -> list[Span]:
    out, used = [], 0
    for text, st in spans:
        if used + len(text) > width:
            text = text[:max(0, width - used)]
        if text:
            out.append((text, st))
            used += len(text)
        if used >= width:
            break
    return out


def _pad(spans: list[Span], width: int) -> list[Span]:
    spans = _clip(spans, width)
    used = sum(len(t) for t, _ in spans)
    return spans + ([(" " * (width - used), "")] if used < width else [])


def plain(lines: list[list[Span]]) -> str:
    return "\n".join("".join(t for t, _ in ln).rstrip() for ln in lines)


def _mmss(s: float) -> str:
    s = max(0.0, s)
    return f"{int(s // 60):02d}:{s % 60:04.1f}"


class View:
    """What the renderer reads from the analyzer for one half, over the last WINDOW_S."""

    def __init__(self, an: Analyzer, e: int, now_ns: int, reset_ns: int | None) -> None:
        st = an.stats
        t0 = an.t0 or now_ns
        end_i = max(0, int((now_ns - t0) // 1_000_000_000)) + 1
        lo = end_i - WINDOW_S
        if reset_ns is not None:
            lo = max(lo, int((reset_ns - t0) // 1_000_000_000))
        self.lo, self.end = max(0, lo), end_i
        self.span = max(1, self.end - self.lo)
        self.w = st.window(e, self.lo, self.end)
        self.series = st.lat_series(e, max(0, end_i - WINDOW_S), end_i)
        self.fit = an.fits.get(e)
        self.h = an.half.get(e)
        self.asleep_since = an.sleep_since.get(e)


def _panel(an: Analyzer, e: int, v: View, now_ns: int, pw: int) -> list[list[Span]]:
    w, h = v.w, v.h
    if h is None:
        state, st = "no data", "dim"
    elif v.asleep_since is not None:
        state, st = f"asleep {(now_ns - v.asleep_since) / 1e9:.0f} s", "dim"
    elif now_ns - h.get("seen_ns", now_ns) > 4e9:
        state, st = "silent", "warn"
    else:
        state, st = "awake", "good"
    batt = f"  {h['batt_dV'] / 10:.1f} V" if h and h.get("batt_dV") else ""
    ch = f"  ch {h['channel']}" if h and h.get("channel") is not None else ""
    lines = [[(f" {name_of(e)} ", "head"), (f" {state}{batt}{ch}", st)]]
    loss_st = "" if w.loss_pct is None or w.loss_pct < 1 else ("warn" if w.loss_pct < 5 else "bad")
    lines.append([(f" rate {w.frames / v.span:5.1f}/s", ""),
                  (f"  retry {_fmt(w.retry_mean, '.2f')}", ""),
                  (f"  MAX_RT {w.max_rt}", "warn" if w.max_rt else "")])
    lines.append([(f" loss {_fmt(w.loss_pct, '.1f')}%", loss_st),
                  (f" ({w.lost}/{w.lost + w.matched})", ""),
                  (f"  gaps {w.gaps}", "warn" if w.gaps else "")])
    lines.append([(f" dup {_fmt(w.dup_pct, '.1f')}%", ""), (f"  RPD>=-64dBm {_fmt(w.rpd_strong_pct, '.0f')}%", "")])
    # percentiles over the window come from the pairs themselves
    fits = an.fits_for(e)
    ww = an.stats.window(e, v.lo, v.end, fits)
    lines.append([(f" lat p50 {_fmt(ww.lat_p50)} ms  p95 {_fmt(ww.lat_p95)} ms", ""),
                  (f"  p99 {_fmt(ww.lat_p99)}", "dim")])
    lines.append([(" " + sparkline(v.series, pw - 3), "spark")])
    arcs = "  ".join(f"{i}:{n}" for i, n in enumerate(w.arc_hist) if n) or "-"
    lines.append([(f" arc {arcs}", "dim")])
    if v.fit is not None and v.fit.ready:
        lines.append([(f" clock {(v.fit.slope - 1) * 1e6:+.1f} ppm  jitter {v.fit.jitter_us / 1000:.2f} ms  n={v.fit.n}", "dim")])
    else:
        lines.append([(" clock: collecting pairs", "dim")])
    return [_pad(ln, pw) for ln in lines]


def _compact_half(an: Analyzer, e: int, v: View, now_ns: int) -> list[Span]:
    w = v.w
    state = "?" if v.h is None else ("zz" if v.asleep_since is not None else "up")
    ww = an.stats.window(e, v.lo, v.end, an.fits_for(e))
    return [(f"{name_of(e)[:1]}:{state} {w.frames / v.span:.1f}/s loss {_fmt(w.loss_pct)}% dup {_fmt(w.dup_pct)}% "
             f"p50 {_fmt(ww.lat_p50)} p95 {_fmt(ww.lat_p95)}", "")]


def _dongle_line(an: Analyzer, now_ns: int, span_lo: int, span_hi: int) -> list[Span]:
    d = an.stats.dongle_window(span_lo, span_hi)
    span = max(1, span_hi - span_lo)
    seen = an.dongle.get("seen_ns")
    ago = "-" if seen is None else f"{(now_ns - seen) / 1e9:.1f}s"
    return [(" DONGLE ", "head"),
            (f" {d.frames / span:5.1f} f/s  dups {d.dups}  ring drops {d.drops}  ch {_fmt(d.channel, 'd')}  heard {ago} ago", "bad" if d.drops else "")]


def status_line(an: Analyzer, now_ns: int, info: dict) -> list[Span]:
    tr = " ".join(f"{s}:{an.trace.get(s, '-')}" for s in sorted(set(an.ports) | set(an.trace)))
    ports = " ".join(f"{s}={p}" for s, p in sorted(an.ports.items()) if p)
    el = _mmss(an.rel_s(now_ns)) if an.t0 else "00:00.0"
    tot = f"/{_mmss(info['duration_s'])}" if info.get("duration_s") else ""
    pause = " [scroll paused]" if info.get("paused") else ""
    mode = info.get("mode", "")
    return [(f" {mode} {info.get('run_file', '')} {el}{tot} | {tr} | {ports}{pause} | q p r", "status")]


def render(an: Analyzer, now_ns: int, w: int, h: int, info: dict | None = None) -> list[list[Span]]:
    info = info or {}
    if w < 40 or h < 6:
        return [[("terminal too small (need >= 40x6)", "bad")]]
    halves = sorted(e for e in (set(an.emitters()) | an.expect) if e != 2) or [0, 1]
    reset_ns = info.get("reset_ns")
    views = {e: View(an, e, now_ns, reset_ns) for e in halves}
    first = views[halves[0]]
    body_h = h - 1
    wide = w >= 76 and len(halves) == 2
    full_h = PANEL_H * (1 if wide else len(halves)) + 1 + 3
    lines: list[list[Span]] = []
    if body_h >= full_h:
        if wide:
            pw = (w - 1) // 2
            pl = [_panel(an, e, views[e], now_ns, pw) for e in halves]
            for i in range(PANEL_H):
                lines.append(pl[0][i] + [(" ", "")] + pl[1][i])
        else:
            for e in halves:
                lines += [_clip(ln, w) for ln in _panel(an, e, views[e], now_ns, w)]
    else:
        for e in halves:
            lines.append(_clip(_compact_half(an, e, views[e], now_ns), w))
    lines.append(_clip(_dongle_line(an, now_ns, first.lo, first.end), w))
    room = body_h - len(lines)
    evs = info.get("frozen_events") if info.get("paused") and info.get("frozen_events") is not None else list(an.events)
    shown = evs[-room:] if room > 0 else []
    t0 = an.t0 or 0
    for ev in shown:
        lines.append(_clip([(f" {_mmss((ev['ns'] - t0) / 1e9)} ", "dim"), (ev["text"], ev["sev"] if ev["sev"] != "info" else "")], w))
    while len(lines) < body_h:
        lines.append([("", "")])
    lines = lines[:body_h]
    lines.append(_pad(status_line(an, now_ns, info), w))
    return lines


# ---- curses loop ---------------------------------------------------------------

def _attrs(curses):
    base = {"": 0, "bold": curses.A_BOLD, "dim": curses.A_DIM, "head": curses.A_REVERSE | curses.A_BOLD,
            "status": curses.A_REVERSE, "spark": 0, "good": 0, "warn": curses.A_BOLD, "bad": curses.A_BOLD}
    try:
        if curses.has_colors():
            curses.start_color()
            curses.use_default_colors()
            for i, (name, col) in enumerate((("good", curses.COLOR_GREEN), ("warn", curses.COLOR_YELLOW),
                                             ("bad", curses.COLOR_RED), ("spark", curses.COLOR_CYAN)), start=1):
                curses.init_pair(i, col, -1)
                base[name] = curses.color_pair(i) | (curses.A_BOLD if name in ("warn", "bad") else 0)
    except curses.error:
        pass
    return base


def run(stdscr, an: Analyzer, step: Callable[[], tuple[int, bool]], info: dict, fps: float = 8.0) -> None:
    """step() advances the source (capture queue / replay clock) and returns
    (now_ns, finished). Loops until q."""
    import curses
    attrs = _attrs(curses)
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    stdscr.nodelay(True)
    stdscr.keypad(True)
    while True:
        now_ns, finished = step()
        info["finished"] = finished
        key = stdscr.getch()
        if key in (ord("q"), ord("Q"), 27):
            return
        if key in (ord("p"), ord("P")):
            info["paused"] = not info.get("paused")
            info["frozen_events"] = list(an.events) if info["paused"] else None
        if key in (ord("r"), ord("R")):
            info["reset_ns"] = now_ns
        h, w = stdscr.getmaxyx()
        stdscr.erase()
        for y, ln in enumerate(render(an, now_ns, w, h, info)[:h]):
            x = 0
            for text, st in ln:
                if x >= w:
                    break
                try:
                    stdscr.addstr(y, x, text[:w - x], attrs.get(st, 0))
                except curses.error:
                    pass
                x += len(text)
        stdscr.refresh()
        time.sleep(1.0 / fps)
