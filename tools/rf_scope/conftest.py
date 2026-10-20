# Makes `import rftrace` work from tools/rf_scope/tests (pytest prepends this
# directory to sys.path because a conftest.py lives here).

import pytest


@pytest.fixture(autouse=True)
def _no_real_serial_ports(monkeypatch):
    """No test may ever discover a real device: port auto-detection only sees
    what a test explicitly provides (2026-10-20: a test without ports found the
    real dongle and ESP-Prog and started a capture on them)."""
    import glob as _glob
    from rftrace import cli
    real = _glob.glob

    def guarded(pattern, *args, **kwargs):
        if pattern.startswith("/dev/"):
            return []
        return real(pattern, *args, **kwargs)
    monkeypatch.setattr(_glob, "glob", guarded)
    monkeypatch.setattr(cli, "autodetect_ports",
                        lambda given, glob=None, half="left", _f=cli.autodetect_ports:
                        _f(given, glob=glob or (lambda p: []), half=half))
