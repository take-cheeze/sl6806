#!/usr/bin/env python3
"""
Tests for sl6806-clockcal's fit, and for the monitor logic it shares.

WHY THIS FILE EXISTS. The clock calibrator produces a single number that then
gets baked into every subsequent build as F_CPU. If the fit is wrong, nothing
fails - all the timing is just quietly off, which is the exact situation the
tool exists to end. So the arithmetic is a pure function and is checked here
against synthetic devices whose clock we already know.

The monitor half is here for a different reason: sl6806_dev.py was factored
out of sl6806-monitor so the calibrator could reuse it, and the monitor is the
tool a bring-up session depends on. These exercise the input path against a
fake device so that refactor cannot silently break it.

    python3 tests/tools/test_clockcal.py
"""

import importlib.machinery
import importlib.util
import os
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import sl6806_dev as dev                                        # noqa: E402


def load(name, path):
    """Import a tool that has no .py extension."""
    spec = importlib.util.spec_from_loader(
        name, importlib.machinery.SourceFileLoader(name, path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


clockcal = load("sl6806_clockcal", os.path.join(ROOT, "tools", "sl6806-clockcal"))
monitor = load("sl6806_monitor", os.path.join(ROOT, "tools", "sl6806-monitor"))

failures = 0
checks = 0


def check(cond, msg, *args):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL " + (msg % args if args else msg))


# ------------------------------------------------------------------ the fit

def samples_at(hz, count=100, interval=0.25, jitter=None, t0=1234.5):
    """A device running at exactly `hz`, sampled every `interval` seconds."""
    out = []
    for i in range(count):
        t = t0 + i * interval
        # The host's idea of when the stamp happened is off by the jitter; the
        # device's counter is not.
        err = jitter[i % len(jitter)] if jitter else 0.0
        out.append((t + err, int(round((t - t0) * hz))))
    return out


def test_exact_clock_is_recovered():
    for hz in (24_000_000, 60_000_000, 120_000_000, 192_000_000):
        slope, rms = clockcal.fit(samples_at(hz))
        check(abs(slope - hz) < 1.0,
              "clean %d Hz recovered as %.1f", hz, slope)
        check(rms < 1e-6, "clean data should have no residual, got %g", rms)


def test_jitter_widens_the_residual_without_biasing_the_answer():
    """Uneven polling must cost precision, not correctness.

    This is the realistic case: smtlink_dump is a subprocess, so when a stamp
    actually happened is known only to within a round trip. Symmetric noise
    like that has to average out.
    """
    hz = 96_000_000
    jitter = [0.010, -0.008, 0.004, -0.012, 0.006, -0.000]
    slope, rms = clockcal.fit(samples_at(hz, jitter=jitter))

    check(abs(slope - hz) / hz < 0.001,
          "jitter biased the fit: %.1f vs %d", slope, hz)
    check(rms > 0.001, "jitter should show up in the residual, got %g", rms)


def test_a_single_outlier_does_not_dominate():
    """One slow transaction - a retry, a scheduler hiccup - is not a result."""
    hz = 120_000_000
    s = samples_at(hz, count=120)
    s[60] = (s[60][0] + 0.5, s[60][1])       # half a second late, mid-run

    slope, _ = clockcal.fit(s)
    check(abs(slope - hz) / hz < 0.005,
          "one outlier moved the answer by %.2f%%", 100 * abs(slope - hz) / hz)


def test_a_stopped_counter_is_not_reported_as_a_clock():
    """A device whose counter never advances must not read as 0 Hz-ish noise.

    fit() returning something positive here would print a confident wrong
    number; the caller checks for slope <= 0.
    """
    s = [(1000.0 + i * 0.25, 42) for i in range(50)]
    slope, _ = clockcal.fit(s)
    check(slope == 0 or slope is None,
          "a frozen counter fitted to %s", slope)


def test_fit_declines_when_every_sample_is_at_the_same_instant():
    s = [(5.0, i * 1000) for i in range(20)]
    slope, rms = clockcal.fit(s)
    check(slope is None and rms is None,
          "a zero time span must decline, got %s", slope)


# ------------------------------------------------------- the device plumbing

class FakeDevice:
    """A device that answers reads and writes out of a dict of memory."""

    def __init__(self):
        self.mem = {}
        self.writes = []

    def poke(self, addr, data):
        for i, b in enumerate(data):
            self.mem[addr + i] = b

    def read(self, addr, size):
        try:
            return bytes(self.mem[addr + i] for i in range(size))
        except KeyError:
            return None

    def write(self, addr, data):
        self.writes.append((addr, bytes(data)))
        self.poke(addr, data)
        return True


def make_console(base, size=2048, rx_head=0, rx_tail=0, version=dev.VERSION):
    return struct.pack(dev.HDR_FMT, dev.MAGIC, size, version,
                       0, 0, 0, rx_head, rx_tail)


def test_clockstamp_is_decoded():
    d = FakeDevice()
    d.poke(0x8000, struct.pack(dev.CLK_FMT, dev.CLK_MAGIC, 120000000,
                               7, 0x89ABCDEF, 0x00000003))
    got = dev.Device.read_clockstamp(d, 0x8000)
    check(got is not None, "a valid stamp was rejected")
    if got:
        seq, cycles, f_cpu = got
        check(seq == 7, "seq %d", seq)
        check(cycles == (3 << 32) | 0x89ABCDEF,
              "the 64-bit counter was reassembled wrong: 0x%X", cycles)
        check(f_cpu == 120000000, "f_cpu %d", f_cpu)


def test_clockstamp_without_the_magic_is_refused():
    """An older sketch, or memory that happens to be readable but is not ours.

    Getting this wrong would regress against garbage and print a clock.
    """
    d = FakeDevice()
    d.poke(0x8000, struct.pack(dev.CLK_FMT, 0xDEADBEEF, 1, 1, 1, 0))
    check(dev.Device.read_clockstamp(d, 0x8000) is None,
          "a stamp with the wrong magic was accepted")

    check(dev.Device.read_clockstamp(FakeDevice(), 0x8000) is None,
          "an unreadable address should give None, not raise")


def test_symbols_are_read_from_the_sym_file():
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".sym", delete=False) as f:
        f.write("00826000 B _sl6806_console\n"
                "008268a4 B _sl6806_clockstamp\n")
        path = f.name
    try:
        syms = dev.read_symbols(path)
        check(syms.get("_sl6806_console") == 0x826000,
              "console address: %r", syms.get("_sl6806_console"))
        # The two names share a prefix; a sloppy match would return the wrong
        # one and the calibrator would regress the console ring's magic word.
        check(syms.get("_sl6806_clockstamp") == 0x8268A4,
              "clockstamp address: %r", syms.get("_sl6806_clockstamp"))
        check(dev.symbol(path, "_sl6806_console") == 0x826000,
              "symbol() disagrees with read_symbols()")
    finally:
        os.unlink(path)


def test_console_header_parsing():
    check(dev.parse_console_header(make_console(0)) == (2048, dev.VERSION),
          "a valid header did not parse")
    check(dev.parse_console_header(b"\x00" * dev.HDR_LEN) is None,
          "a zeroed header was accepted as valid")
    check(dev.parse_console_header(None) is None, "None should parse as None")
    check(dev.parse_console_header(b"\x53") is None,
          "a short read was accepted")


def test_monitor_input_lands_in_the_rx_ring():
    """The half of the console that has never been exercised on hardware."""
    d = FakeDevice()
    base = 0x826000
    d.poke(base, make_console(base))

    ok = monitor.push_input(d, base, dev.HDR_LEN, "hi\n")
    check(ok, "push_input refused a valid ring")

    rx = base + dev.HDR_LEN + 2048
    check((rx, b"hi\n") in d.writes, "payload did not land at the rx ring: %s",
          [(hex(a), p) for a, p in d.writes])
    # rx_head is the 7th u32 of the header and must be published last.
    check(d.writes[-1] == (base + 24, struct.pack("<I", 3)),
          "rx_head was not published last: %s", d.writes[-1])


def test_monitor_input_wraps_the_rx_ring():
    d = FakeDevice()
    base = 0x826000
    # 254 bytes already consumed, so a 4-byte write straddles the end.
    d.poke(base, make_console(base, rx_head=254, rx_tail=254))

    check(monitor.push_input(d, base, dev.HDR_LEN, "abcd"), "refused")
    rx = base + dev.HDR_LEN + 2048
    payload = [w for w in d.writes if w[0] != base + 24]
    check(payload == [(rx + 254, b"ab"), (rx, b"cd")],
          "the wrap split wrong: %s", [(hex(a), p) for a, p in payload])


def test_monitor_refuses_to_overrun_unconsumed_input():
    d = FakeDevice()
    base = 0x826000
    d.poke(base, make_console(base, rx_head=300, rx_tail=100))   # 200 queued
    check(not monitor.push_input(d, base, dev.HDR_LEN, "x" * 100),
          "overran input the sketch has not read yet")
    check(monitor.push_input(d, base, dev.HDR_LEN, "x" * 50),
          "refused a write that fits")


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    print("sl6806-clockcal / monitor tests")
    for t in tests:
        t()
    print("test_clockcal: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
