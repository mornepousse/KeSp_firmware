"""Port auto-detection: `rf_scope capture` with no port arguments."""
from rftrace.cli import autodetect_ports

DONGLE = "/dev/serial/by-id/usb-KaSe_KaSe_Dongle_N_A-if00"
PROG_UART = "/dev/serial/by-id/usb-FTDI_Dual_RS232-HS-if01-port0"
PROG_JTAG = "/dev/serial/by-id/usb-FTDI_Dual_RS232-HS-if00-port0"


def fake_glob(paths):
    def g(pattern):
        import fnmatch
        return sorted(p for p in paths if fnmatch.fnmatch(p, pattern))
    return g


def test_nothing_given_finds_dongle_and_esp_prog_as_left():
    got = autodetect_ports({}, glob=fake_glob([DONGLE, PROG_UART, PROG_JTAG]))
    assert got == {"dongle": DONGLE, "left": PROG_UART}


def test_the_jtag_channel_is_never_taken_for_the_uart():
    got = autodetect_ports({}, glob=fake_glob([DONGLE, PROG_JTAG]))
    assert got == {"dongle": DONGLE}


def test_explicit_ports_win():
    got = autodetect_ports({"dongle": "/dev/ttyACM9", "right": "/dev/ttyUSB7"},
                           glob=fake_glob([DONGLE, PROG_UART]))
    assert got == {"dongle": "/dev/ttyACM9", "right": "/dev/ttyUSB7"}


def test_esp_prog_goes_to_the_half_named_with_half():
    got = autodetect_ports({}, glob=fake_glob([DONGLE, PROG_UART]), half="right")
    assert got == {"dongle": DONGLE, "right": PROG_UART}


def test_nothing_plugged_finds_nothing():
    assert autodetect_ports({}, glob=fake_glob([])) == {}
