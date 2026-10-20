"""rf_scope command line: capture / replay / report."""

from __future__ import annotations

import argparse
import datetime
import queue
import re
import sys
import threading
import time

from . import jsonl, ports, report, tui
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


def analyze_file(path, expect: tuple = ()) -> Analyzer:
    an = Analyzer(expect=expect)
    for it in jsonl.read(path):
        an.feed(it)
    an.finish()
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
    p.add_argument("--window", type=int, default=60, metavar="S")
    return ap


# ---- capture ---------------------------------------------------------------------

def cmd_capture(a) -> int:
    chosen = {r: getattr(a, r) for r in ROLES if getattr(a, r)}
    if not chosen:
        print("rf_scope capture: give at least one of --dongle / --left / --right", file=sys.stderr)
        raise SystemExit(2)
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
    request_s = min(65535, a.duration + 30)          # the firmware's own auto-off is the safety net
    runners = {r: SourceRunner(r, chosen[r], open_ports[r], StartController(request_s), q.put) for r in chosen}
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
    t_start = time.monotonic()
    writer = jsonl.Writer(out_path)
    writer.write({"type": "meta", "version": jsonl.VERSION, "tool": "rf_scope", "sources": chosen,
                  "started": datetime.datetime.now().isoformat(timespec="seconds"),
                  "duration_s": a.duration, "host_ns0": time.monotonic_ns()})
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
            if it["type"] == "ctl" and it["event"] == "giveup":
                print("rf_scope: " + it["message"], file=sys.stderr)

    def finished() -> bool:
        return stop.is_set() and all(not t.is_alive() for t in threads)

    def step() -> tuple[int, bool]:
        drain()
        if not stop.is_set() and time.monotonic() - t_start >= a.duration:
            stop.set()
            deadline[0] = time.monotonic() + 4.0
        return time.monotonic_ns(), finished()

    info = {"mode": "LIVE", "run_file": out_path, "duration_s": a.duration}
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
    print(f"rf_scope: wrote {out_path}", file=sys.stderr)
    print(report.render(an, out_path), end="")
    return 0


# ---- replay / report -------------------------------------------------------------

def cmd_report(a) -> int:
    an = analyze_file(a.run)
    print(report.render(an, a.run, a.window), end="")
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
        return cmd_report(argparse.Namespace(run=a.run, window=60))
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
    return {"capture": cmd_capture, "replay": cmd_replay, "report": cmd_report}[a.cmd](a)
