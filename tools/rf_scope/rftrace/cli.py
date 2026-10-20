"""rf_scope command line: capture / stress / replay / report."""

from __future__ import annotations

import argparse
import datetime
import queue
import re
import sys
import threading
import time

from . import jsonl, ports, report, stress, tui
from .analyzer import Analyzer
from .control import StartController
from .source import SourceRunner

ROLES = ("dongle", "left", "right")
EMITTER_OF = {"left": 0, "right": 1}


def parse_duration(s: str) -> int:
    s = s.strip().lower()
    if re.fullmatch(r"\d+", s):
        return int(s)
    m = re.fullmatch(r"(?:(\d+)h)?(?:(\d+)m)?(?:(\d+)s)?", s)
    if not m or not any(m.groups()):
        raise argparse.ArgumentTypeError(f"bad duration {s!r} (use 90, 90s, 10m, 1h30m)")
    h, mi, se = (int(x or 0) for x in m.groups())
    return h * 3600 + mi * 60 + se


def _is_stress(it: dict) -> bool:
    return (it.get("type") == "rec" and it.get("pkt_kind") == stress.PKT_STRESS) or \
        str(it.get("event", "")).startswith("stress_")


def analyze_file(path, expect: tuple = ()) -> Analyzer:
    """The run's analyzer; `.stress_view` is a second one restricted to the
    stress frames when the run has any (the report's stress section)."""
    an = Analyzer(expect=expect)
    san = Analyzer(only_pkt_kind=stress.PKT_STRESS)
    has = False
    for it in jsonl.read(path):
        an.feed(it)
        san.feed(it)
        has = has or _is_stress(it)
    an.finish()
    san.finish()
    an.stress_view = san if has else None
    return an


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="rf_scope", description="See both ends of the KeSp radio link.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture", help="trace the dongle and the halves, write a run file")
    c.add_argument("--dongle", metavar="/dev/ttyACMx", help="dongle CDC port")
    c.add_argument("--left", metavar="/dev/ttyUSBx", help="left half's UART0 (ESP-Prog)")
    c.add_argument("--right", metavar="/dev/ttyUSBy", help="right half's UART0 (ESP-Prog)")
    c.add_argument("--duration", type=parse_duration, default=600, metavar="10m")
    c.add_argument("-o", "--output", default=None, help="run file (default rf_run_<date>.jsonl)")
    c.add_argument("--no-tui", action="store_true", help="no curses: one status line every 5 s on stderr")
    c.add_argument("--baud", type=int, default=115200, help="half UART baud rate")
    r = sub.add_parser("replay", help="the TUI fed from a run file")
    r.add_argument("run")
    r.add_argument("--speed", type=float, default=1.0)
    r.add_argument("--no-tui", action="store_true", help="print the report instead")
    r.add_argument("--dump", metavar="WxH", help="print one rendered TUI frame of the whole run and exit")
    r.add_argument("--at", type=float, default=None, help="with --dump: seconds into the run (default the end)")
    p = sub.add_parser("report", help="text summary of a run file")
    p.add_argument("run")
    p.add_argument("--window", type=int, default=None, metavar="S",
                   help="per-window table width (default 60 s; 600 s for a soak run)")
    s = sub.add_parser("stress", help="load the link with a half's stress generator, trace it, report it")
    s.add_argument("--dongle", metavar="/dev/ttyACMx", help="dongle CDC port (required)")
    s.add_argument("--left", metavar="/dev/ttyUSBx", help="left half's UART0 (ESP-Prog): stressed")
    s.add_argument("--right", metavar="/dev/ttyUSBy", help="right half's UART0 (ESP-Prog): stressed")
    s.add_argument("--scenario", required=True, choices=stress.SCENARIOS,
                   help="burst: 10 back to back + 200 ms pause; max: back to back; held: every 100 ms; "
                        "rate: --rate Hz; soak: --rate Hz (default 20) for hours, 10-min windows")
    s.add_argument("--rate", type=int, default=None, metavar="HZ", help="rate / soak: frames per second")
    s.add_argument("--burst-len", type=int, default=10, metavar="N", help="burst: frames per burst")
    s.add_argument("--burst-pause", type=int, default=200, metavar="MS", help="burst: pause after a burst")
    s.add_argument("--count", type=int, default=0, help="stop after this many frames (0: duration only)")
    s.add_argument("--duration", type=parse_duration, default=None, metavar="30s",
                   help="stress duration (default 30 s, soak 1 h)")
    s.add_argument("-o", "--output", default=None, help="run file (default rf_stress_<scenario>_<date>.jsonl)")
    s.add_argument("--no-tui", action="store_true", help="no curses: one status line every 5 s on stderr")
    s.add_argument("--baud", type=int, default=115200, help="half UART baud rate")
    s.add_argument("--window", type=int, default=None, metavar="S",
                   help="report window (default 60 s; 600 s for a soak)")
    return ap


# ---- capture ---------------------------------------------------------------------

def cmd_capture(a) -> int:
    chosen = {r: getattr(a, r) for r in ROLES if getattr(a, r)}
    if not chosen:
        print("rf_scope capture: give at least one of --dongle / --left / --right", file=sys.stderr)
        raise SystemExit(2)
    return _session(a, chosen, a.duration, None)


def cmd_stress(a) -> int:
    chosen = {r: getattr(a, r) for r in ROLES if getattr(a, r)}
    if "dongle" not in chosen or not ({"left", "right"} & set(chosen)):
        print("rf_scope stress: give --dongle and at least one of --left / --right (the stressed halves)",
              file=sys.stderr)
        return 2
    try:                                   # refused BEFORE any port is touched
        params = stress.scenario_params(a.scenario, a.duration, rate=a.rate, burst_len=a.burst_len,
                                        burst_pause_ms=a.burst_pause, count=a.count)
    except ValueError as exc:
        print(f"rf_scope stress: {exc}", file=sys.stderr)
        return 2
    if a.window is None and a.scenario == "soak":
        a.window = stress.SOAK_WINDOW_S
    if a.output is None:
        a.output = f"rf_stress_{a.scenario}_" + datetime.datetime.now().strftime("%Y%m%d_%H%M%S") + ".jsonl"
    # The firmware ends the run by itself at its duration; the capture lasts a
    # little longer so the last frames and records arrive, then stops both.
    return _session(a, chosen, params["duration_s"] + 3, params)


def _session(a, chosen: dict, duration_s: int, stress_params: dict | None) -> int:
    if len(set(chosen.values())) != len(chosen):
        print("rf_scope: the same port was given twice", file=sys.stderr)
        return 2
    try:                                   # every check BEFORE any open: all or nothing
        for path in chosen.values():
            ports.check_port_free(path)
    except ports.PortBusy as exc:
        print(f"rf_scope: {exc}", file=sys.stderr)
        return 2
    out_path = a.output or "rf_run_" + datetime.datetime.now().strftime("%Y%m%d_%H%M%S") + ".jsonl"
    win_s = getattr(a, "window", None) or 60
    open_ports = {}
    try:
        for role, path in chosen.items():
            open_ports[role] = ports.open_serial(path, a.baud, half=role != "dongle")
    except Exception as exc:               # noqa: BLE001 — report and close whatever opened
        print(f"rf_scope: cannot open a port: {exc}", file=sys.stderr)
        for p in open_ports.values():
            p.close()
        return 2

    q: queue.Queue = queue.Queue()
    request_s = min(65535, duration_s + 30)          # the firmware's own auto-off is the safety net
    runners = {r: SourceRunner(r, chosen[r], open_ports[r], StartController(request_s), q.put,
                               stress=stress.StressController(stress_params)
                               if stress_params is not None and r != "dongle" else None)
               for r in chosen}
    stop = threading.Event()
    deadline = [None]

    def worker(run: SourceRunner) -> None:
        try:
            while True:
                run.pump()
                if stop.is_set() and run.ctl.state not in ("stopping", "off"):
                    run.request_stop()
                if run.done or (deadline[0] is not None and time.monotonic() > deadline[0]):
                    break
                time.sleep(0.004)
        except OSError as exc:
            q.put({"type": "ctl", "src": run.src, "port": run.port_name, "host_ns": time.monotonic_ns(),
                   "event": "closed", "message": str(exc)})
        finally:
            run.flush()

    threads = [threading.Thread(target=worker, args=(r,), daemon=True) for r in runners.values()]
    an = Analyzer(expect=tuple(EMITTER_OF[r] for r in chosen if r in EMITTER_OF))
    san = Analyzer(only_pkt_kind=stress.PKT_STRESS) if stress_params is not None else None
    t_start = time.monotonic()
    writer = jsonl.Writer(out_path)
    meta = {"type": "meta", "version": jsonl.VERSION, "tool": "rf_scope", "sources": chosen,
            "started": datetime.datetime.now().isoformat(timespec="seconds"),
            "duration_s": duration_s, "host_ns0": time.monotonic_ns()}
    if stress_params is not None:
        meta["scenario"] = {"name": stress_params["scenario"], "params": stress_params}
    writer.write(meta)
    an.feed(meta)
    if san is not None:
        san.feed(meta)
    for t in threads:
        t.start()

    def drain() -> None:
        while True:
            try:
                it = q.get_nowait()
            except queue.Empty:
                return
            writer.write(it)
            an.feed(it)
            if san is not None:
                san.feed(it)
            if it["type"] == "ctl" and it["event"] in ("giveup", "stress_giveup", "stress_refused"):
                print("rf_scope: " + it["message"], file=sys.stderr)

    def finished() -> bool:
        return stop.is_set() and all(not t.is_alive() for t in threads)

    def step() -> tuple[int, bool]:
        drain()
        if not stop.is_set() and time.monotonic() - t_start >= duration_s:
            stop.set()
            deadline[0] = time.monotonic() + 4.0
        return time.monotonic_ns(), finished()

    mode = "LIVE" if stress_params is None else f"STRESS {stress_params['scenario']}"
    info = {"mode": mode, "run_file": out_path, "duration_s": duration_s}
    try:
        if a.no_tui or not sys.stdout.isatty():
            last = 0.0
            while True:
                now_ns, fin = step()
                if fin:
                    break
                if time.monotonic() - last >= 5:
                    last = time.monotonic()
                    print(f"rf_scope: {int(last - t_start)} s, items {an.n_items}, trace "
                          + " ".join(f"{k}:{v}" for k, v in sorted(an.trace.items())), file=sys.stderr)
                time.sleep(0.05)
        else:
            import curses
            curses.wrapper(lambda scr: tui.run(scr, an, step, info))
    except KeyboardInterrupt:
        pass
    finally:                               # leave the boards as we found them: trace off
        if not stop.is_set():
            stop.set()
            deadline[0] = time.monotonic() + 4.0
        for t in threads:
            t.join(timeout=6)
        drain()
        writer.close()
        for p in open_ports.values():
            try:
                p.close()
            except OSError:
                pass
    an.finish()
    if san is not None:
        san.finish()
    an.stress_view = san
    print(f"rf_scope: wrote {out_path}", file=sys.stderr)
    print(report.render(an, out_path, win_s), end="")
    return 0


# ---- replay / report -------------------------------------------------------------

def cmd_report(a) -> int:
    an = analyze_file(a.run)
    win = a.window
    if win is None:
        soak = ((an.meta or {}).get("scenario") or {}).get("name") == "soak"
        win = stress.SOAK_WINDOW_S if soak else 60
    print(report.render(an, a.run, win), end="")
    return 0


def render_dump(path, w: int, h: int, at: float | None = None) -> str:
    items = list(jsonl.read(path))
    an = Analyzer(expect=(0, 1))
    t0 = next((i["host_ns"] for i in items if "host_ns" in i), 0)
    limit = None if at is None else t0 + int(at * 1e9)
    for it in items:
        if limit is not None and it.get("host_ns", 0) > limit:
            break
        an.feed(it)
    now = limit if limit is not None else an.last_ns
    t_end = max((i.get("host_ns", 0) for i in items), default=t0)
    info = {"mode": "REPLAY", "run_file": str(path).rsplit("/", 1)[-1], "duration_s": (t_end - t0) / 1e9}
    return tui.plain(tui.render(an, now, w, h, info))


def cmd_replay(a) -> int:
    if a.dump:
        m = re.fullmatch(r"(\d+)x(\d+)", a.dump)
        if not m:
            print("rf_scope: --dump WxH, e.g. 100x32", file=sys.stderr)
            return 2
        print(render_dump(a.run, int(m[1]), int(m[2]), a.at))
        return 0
    if a.no_tui or not sys.stdout.isatty():
        return cmd_report(argparse.Namespace(run=a.run, window=None))
    items = list(jsonl.read(a.run))
    an = Analyzer(expect=(0, 1))
    t0 = next((i["host_ns"] for i in items if "host_ns" in i), 0)
    t_last = max((i.get("host_ns", 0) for i in items), default=t0)
    pos = [0]
    start = time.monotonic()

    def step() -> tuple[int, bool]:
        vnow = t0 + int((time.monotonic() - start) * a.speed * 1e9)
        while pos[0] < len(items) and items[pos[0]].get("host_ns", t0) <= vnow:
            an.feed(items[pos[0]])
            pos[0] += 1
        done = pos[0] >= len(items)
        if done:
            an.finish()
        return min(vnow, t_last), done

    info = {"mode": f"REPLAY x{a.speed:g}", "run_file": str(a.run).rsplit("/", 1)[-1],
            "duration_s": (t_last - t0) / 1e9}
    import curses
    curses.wrapper(lambda scr: tui.run(scr, an, step, info))
    return 0


def main(argv=None) -> int:
    a = build_parser().parse_args(argv)
    return {"capture": cmd_capture, "stress": cmd_stress, "replay": cmd_replay, "report": cmd_report}[a.cmd](a)
