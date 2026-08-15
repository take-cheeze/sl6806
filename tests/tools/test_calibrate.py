#!/usr/bin/env python3
"""
Tests for sl6806-calibrate, the host-side clock measurement.

WHY THIS FILE EXISTS. This tool produces a single number that everything else
is then scaled by: get it wrong and every delay() and millis() in the project
is wrong with it, silently and consistently. There is no second opinion to
check it against on the bench, so the estimator has to be checkable here.

Two kinds of test:

  * the estimator, against synthetic samples from a device whose clock is
    known by construction - including the failure it exists to catch, a
    hardware counter that wrapped without being observed;
  * the whole tool, driven end to end against a fake smtlink_dump that
    simulates a device running at a known rate. That covers the CDB it sends,
    the struct it decodes and the arithmetic together, which is where a
    wrong-but-plausible answer would come from.

    python3 tests/tools/test_calibrate.py
"""

import importlib.machinery
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", ".."))


def load(name, path):
    """Import a tool that has no .py extension."""
    spec = importlib.util.spec_from_loader(
        name, importlib.machinery.SourceFileLoader(name, path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


CALIBRATE = os.path.join(ROOT, "tools", "sl6806-calibrate")
cal = load("sl6806_calibrate", CALIBRATE)

failures = 0
checks = 0


def check(cond, msg, *args):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL " + (msg % args if args else msg))


def stat_bytes(cycles, polls=1, loops=0, f_cpu=120000000, run_mode=2,
               tick_mask=0xFFFFFFFF, magic=None, version=None, raw=0x1234):
    return struct.pack("<10I", cal.STAT_MAGIC if magic is None else magic,
                       cal.STAT_VERSION if version is None else version,
                       cycles & 0xFFFFFFFF, cycles >> 32,
                       polls, loops, f_cpu, run_mode, tick_mask, raw)


# ------------------------------------------------------------------ decoding

def test_parse_stat_round_trips():
    st = cal.parse_stat(stat_bytes(0x1234_5678_9ABC, polls=7, loops=3))
    check(st is not None, "a well-formed status reply failed to parse")
    if st:
        check(st["cycles"] == 0x1234_5678_9ABC,
              "cycles decoded as 0x%X - the 64-bit split is wrong",
              st["cycles"])
        check((st["polls"], st["loops"]) == (7, 3),
              "counters decoded as %s", (st["polls"], st["loops"]))


def test_parse_stat_rejects_what_is_not_a_status():
    check(cal.parse_stat(None) is None, "None should not parse")
    check(cal.parse_stat(b"") is None, "empty input should not parse")
    check(cal.parse_stat(stat_bytes(1)[:20]) is None,
          "a truncated reply should not parse")
    # The console answers the same vendor command at a different address; its
    # packet must never be mistaken for a status.
    check(cal.parse_stat(b"\x04\x00\x00\x00" + b"tick" * 15) is None,
          "a console packet parsed as a status reply")
    check(cal.parse_stat(stat_bytes(1, magic=0x36384C53)) is None,
          "the console magic was accepted as a status magic")


# ----------------------------------------------------------------- estimator

def perfect_samples(hz, n=40, spacing=0.2, latency=0.03, start=1000.0):
    """Samples from an ideal device: the counter is read `latency` seconds
    after the host's timestamp, every time."""
    out = []
    for i in range(n):
        t0 = start + i * spacing
        t1 = t0 + latency
        out.append((t0, t1, int(round(hz * (t0 + latency / 2 - start)))))
    return out


def jittered_samples(hz, n=60, spacing=0.2, start=1000.0):
    """The realistic case: the read lands at an unpredictable point inside
    the interval the host timed. Deterministic, so a failure reproduces."""
    out = []
    rnd = 12345
    for i in range(n):
        rnd = (1103515245 * rnd + 12345) & 0x7FFFFFFF
        frac = rnd / 0x7FFFFFFF                  # where in the window it landed
        rnd = (1103515245 * rnd + 12345) & 0x7FFFFFFF
        width = 0.01 + 0.06 * (rnd / 0x7FFFFFFF)  # how long the round trip took
        t0 = start + i * spacing
        t1 = t0 + width
        out.append((t0, t1, int(round(hz * (t0 + frac * width - start)))))
    return out


def test_recovers_a_known_clock():
    hz = 192000000
    fit = cal.fit_clock(perfect_samples(hz))
    check(fit is not None, "fit_clock returned nothing")
    if fit:
        err = abs(fit["hz"] - hz) / hz
        check(err < 1e-6, "recovered %.0f Hz from a noiseless %d Hz device "
                          "(%.4f%% off)", fit["hz"], hz, err * 100)
        check(fit["consistent"], "noiseless samples reported as inconsistent")


def test_the_bracket_contains_the_truth():
    """The bracket is the tool's honesty: it must never exclude the real
    clock, whatever the round-trip latency did."""
    hz = 192000000
    fit = cal.fit_clock(jittered_samples(hz))
    check(fit["lo"] <= hz <= fit["hi"],
          "the bracket %.0f..%.0f excludes the true %d Hz",
          fit["lo"], fit["hi"], hz)
    check(fit["consistent"], "honest jittered samples reported as inconsistent")
    err = abs(fit["hz"] - hz) / hz
    check(err < 0.01, "point estimate %.0f Hz is %.3f%% off %d Hz",
          fit["hz"], err * 100, hz)


def test_the_bracket_tightens_with_duration():
    """Run longer, not denser: the bound comes from the span, so this is the
    advice the tool gives and it had better be true."""
    hz = 192000000
    short = cal.fit_clock(jittered_samples(hz, n=30, spacing=0.2))   # ~6 s
    long = cal.fit_clock(jittered_samples(hz, n=30, spacing=2.0))    # ~60 s
    check((long["hi"] - long["lo"]) < (short["hi"] - short["lo"]) / 5,
          "a 10x longer span barely narrowed the bracket: %.0f Hz vs %.0f Hz",
          long["hi"] - long["lo"], short["hi"] - short["lo"])


def test_an_unobserved_wrap_is_caught_not_averaged():
    """The failure this whole design has to survive.

    sl6806_cycles() only notices a wrap when it is read. Poll more slowly than
    the counter wraps and elapsed cycles go missing - which would come out as
    a plausible, wrong, and much too low clock. It has to be refused instead.
    """
    hz = 192000000
    samples = perfect_samples(hz, n=20, spacing=1.0)
    lost = [(t0, t1, c if i < 10 else c - (1 << 32))
            for i, (t0, t1, c) in enumerate(samples)]

    fit = cal.fit_clock(lost)
    check(not fit["consistent"],
          "a lost counter wrap was averaged into a %.0f Hz answer instead of "
          "being reported as impossible", fit["hz"])

    # And the honest version of the same run must still pass, or the check
    # above is just rejecting everything.
    check(cal.fit_clock(samples)["consistent"],
          "the un-corrupted samples were also called inconsistent")


def test_too_few_samples():
    check(cal.fit_clock([]) is None, "an empty sample set should not fit")
    check(cal.fit_clock([(1.0, 1.1, 0)]) is None, "one sample should not fit")


# -------------------------------------------------------- round frequencies

def test_round_frequencies():
    check(cal.round_frequencies(191_900_000, 192_100_000) == [192000000],
          "a bracket around 192 MHz should offer exactly that")
    check(cal.round_frequencies(192_100_000, 192_900_000) == [],
          "a bracket between two whole MHz should offer nothing")
    check(cal.round_frequencies(191_500_000, 193_500_000) ==
          [192000000, 193000000], "both enclosed whole MHz should be offered")
    check(cal.round_frequencies(1e6, float("inf")) == [],
          "an unbounded bracket should offer nothing")
    check(cal.round_frequencies(50e6, 400e6) == [],
          "a 350 MHz-wide bracket is useless and should offer nothing")


# ------------------------------------------------------------- the whole tool
#
# HOW FAST A FAKE DEVICE STARTS IS PART OF WHAT THESE TESTS MEASURE
#
# The tool spawns its `smtlink_dump` once per poll, and refuses to report from
# fewer than 8 samples. The tests give it a fixed wall-clock budget, so the
# number of samples it manages is entirely a question of how fast one fake
# device runs, and the margin is thinner than it looks.
#
# Both fakes were Python. A CPython start plus its imports is ~70 ms on an idle
# developer machine, which fits ~38 polls into 3 s. The floor is 8, so the
# margin was about 5x, and a loaded shared runner eats 5x without trying. On
# 2026-08-15 `test_a_frozen_counter_distinguishes_junk_from_ungated` failed on
# GitHub Actions with three checks reporting empty output, because the run
# collected fewer than 8 samples and the tool exited through "only %d samples -
# not enough to measure anything", which goes to *stderr*: stdout is then empty
# and the checks that search it have nothing to find. The same commit passed on
# a re-run, which is what a timing race looks like.
#
# The frozen fake below is therefore POSIX shell, ~5.7 ms per spawn, so the
# same 3 s holds ~191 samples. What is left of the budget is mostly the poll
# interval the test asks for, which no machine can take away. It emits the same
# status packet as the Python version did, byte for byte.
#
# FAKE_TOOL stayed Python on purpose - see the note above it. Converting a fake
# that has to simulate a *clock* traded a timing race for a worse problem.
#
# Packing bytes in shell is where a silent mistake would hide, so
# test_the_shell_fakes_pack_the_bytes_they_claim_to decodes what the fake
# actually writes with the tool's own parser. `$(( ))` arithmetic is 64-bit on
# every shell this repository targets.

FAKE_PREAMBLE = r'''#!/bin/sh
# Argument shape, from sl6806_dev.Device._run:
#   $1 chip   $2 6806   $3 read_mem2   $4 addr   $5 size   $6 outfile
[ "$1" = chip ] && [ "$2" = 6806 ] || { echo "fake: bad invocation" >&2; exit 1; }
[ "$3" = read_mem2 ] || { echo "fake: unexpected command $3" >&2; exit 1; }
STATE="$(dirname "$0")/state"

# One little-endian u32 as four octal escapes, for a printf format string.
esc() {
    printf '\\%%03o\\%%03o\\%%03o\\%%03o' \
        $(( $1 & 255 )) $(( ($1 >> 8) & 255 )) \
        $(( ($1 >> 16) & 255 )) $(( ($1 >> 24) & 255 ))
}
'''

#
# THIS ONE STAYS PYTHON, AND THE ATTEMPT TO CONVERT IT IS WHY THE NOTE EXISTS.
#
# It simulates a device running at a known clock, so unlike the frozen fake it
# needs a timebase, and the whole point of the test is that the tool recovers
# that clock to within 2%. A shell version was tried and measured 2.58% fast
# on CI: `date` is another fork in the critical path and reads CLOCK_REALTIME
# rather than a monotonic clock, and the tool's fit sees the difference.
# time.monotonic() sampled once per poll is worth ~70 ms of startup here.
#
# Its own sample-count margin is the one this file otherwise worries about,
# and it is handled by the duration below rather than by making the fake
# cheaper - see test_end_to_end_against_a_simulated_device.
FAKE_TOOL = r'''#!/usr/bin/env python3
"""A fake smtlink_dump: one SL6806 running at exactly HZ, in bootloader mode."""
import os, struct, sys, time

HZ = %(hz)d
STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "state")

args = sys.argv[1:]
if args[:2] != ["chip", "6806"]:
    sys.exit("fake: unexpected invocation %%r" %% args)
cmd, addr, size, out = args[2], int(args[3], 16), int(args[4], 16), args[5]
if cmd != "read_mem2":
    sys.exit("fake: unexpected command " + cmd)

now = time.monotonic()
if os.path.exists(STATE):
    t0, polls = open(STATE).read().split()
    t0, polls = float(t0), int(polls) + 1
else:
    t0, polls = now, 1
open(STATE, "w").write("%%r %%d" %% (t0, polls))

if addr != %(addr)d:
    sys.exit("fake: nothing at 0x%%08X" %% addr)

cycles = int(round(HZ * (now - t0)))
open(out, "wb").write(struct.pack(
    "<10I", %(magic)d, %(ver)d, cycles & 0xFFFFFFFF, cycles >> 32,
    polls, 0, 120000000, 2, %(mask)d, %(raw)d)[:size])
'''


def test_end_to_end_against_a_simulated_device():
    """Everything at once: CDB, decode, fit, report - against a known clock."""
    hz = 192000000
    with tempfile.TemporaryDirectory(prefix="sl6806cal.") as d:
        fake = os.path.join(d, "smtlink_dump")
        with open(fake, "w") as f:
            f.write(FAKE_TOOL % {"hz": hz, "ver": cal.STAT_VERSION,
                                 "magic": cal.STAT_MAGIC, "addr": cal.STAT_ADDR,
                                 "mask": 0xFFFFFFFF, "raw": 0x1000})
        os.chmod(fake, 0o755)

        # 8 s rather than the 4 s this used to ask for. The floor is 8 samples
        # and a Python fake costs ~70 ms a poll, so 4 s was about a 7x margin -
        # the same margin the frozen test had when it lost the race on CI. The
        # extra seconds also tighten the fit this test is actually about.
        r = subprocess.run(
            [sys.executable, CALIBRATE, "--tool", fake, "--no-sudo", "--json",
             "--duration", "8", "--interval", "0.05"],
            capture_output=True, text=True)

    if r.returncode != 0 or not r.stdout.strip():
        check(False, "the tool failed (rc=%d):\n%s\n%s",
              r.returncode, r.stdout[-800:], r.stderr[-800:])
        return

    got = json.loads(r.stdout)
    check(got["samples"] >= 8, "only %d samples in 8 s", got["samples"])
    check(got["consistent"], "a well-behaved simulated device came out "
                             "inconsistent")
    err = abs(got["hz"] - hz) / hz
    check(err < 0.02, "measured %.0f Hz from a %d Hz device (%.2f%% off)",
          got["hz"], hz, err * 100)
    check(got["hz_lo"] <= hz <= got["hz_hi"],
          "the reported bracket %.0f..%.0f excludes the true clock",
          got["hz_lo"], got["hz_hi"])
    check(got["built_f_cpu"] == 120000000,
          "the tool lost the device's built-in F_CPU: %s", got["built_f_cpu"])
    check(got["run_mode"] == "poll", "run_mode decoded as %r", got["run_mode"])
    check(got["polls"] >= got["samples"],
          "the device counted %d polls for %d samples", got["polls"],
          got["samples"])


FAKE_REFUSES = r'''#!/usr/bin/env python3
import sys
sys.stderr.write("LIBUSB_ERROR_PIPE\n")
sys.exit(1)
'''


def test_a_refusing_device_is_explained_not_retried_in_silence():
    """The failure that cost three minutes of a bench session.

    When the device refuses a command, the reason libusb gave is the single
    most diagnostic fact available. A tool that swallows it and retries turns
    that into "it just sits there".
    """
    with tempfile.TemporaryDirectory(prefix="sl6806cal.") as d:
        fake = os.path.join(d, "smtlink_dump")
        with open(fake, "w") as f:
            f.write(FAKE_REFUSES)
        os.chmod(fake, 0o755)

        r = subprocess.run(
            [sys.executable, CALIBRATE, "--tool", fake, "--no-sudo",
             "--duration", "30", "--interval", "0.01"],
            capture_output=True, text=True)

    check(r.returncode != 0, "a device that refuses everything should fail")
    check("LIBUSB_ERROR_PIPE" in r.stderr,
          "the tool hid what the device actually said:\n%s", r.stderr[-500:])


def test_a_hanging_device_does_not_hang_the_tool():
    """libusb can block forever on a stalled endpoint; the tool must not."""
    with tempfile.TemporaryDirectory(prefix="sl6806cal.") as d:
        fake = os.path.join(d, "smtlink_dump")
        with open(fake, "w") as f:
            f.write("#!/usr/bin/env python3\nimport time\ntime.sleep(300)\n")
        os.chmod(fake, 0o755)

        dev = load("sl6806_dev", os.path.join(ROOT, "tools", "sl6806_dev.py"))
        d_ = dev.Device(fake, sudo=False, timeout=1.0)
        started = os.times().elapsed
        got = d_.read(0x53544154, 36)
        took = os.times().elapsed - started

    check(got is None, "a hung tool should read as a failure, not data")
    check(took < 10, "the read took %.1f s - the timeout did not fire", took)
    check("stalled" in (d_.last_error or ""),
          "a timeout should say so, got %r", d_.last_error)


def test_the_poll_rate_stays_inside_a_counter_wrap():
    """The 24-bit fallback wraps in 262 ms at the measured 64 MHz.

    The default 200 ms interval plus a round trip exceeds that, which loses a
    wrap and produces a plausible, wrong, much-too-low clock - reproduced in
    simulation as 3.4 MHz. The tool has to pick its own rate rather than warn
    about the one it chose.
    """
    hz, mask24 = 64000000, 0x00FFFFFF
    wrap = (mask24 + 1) / hz

    gap = cal.safe_interval(mask24, hz, round_trip=0.012)
    check(gap + 0.012 < wrap / 3.0,
          "chose a %.3f s spacing against a %.3f s wrap", gap + 0.012, wrap)

    # The round trip counts toward the spacing, so it has to be subtracted.
    check(cal.safe_interval(mask24, hz, round_trip=0.04)
          < cal.safe_interval(mask24, hz, round_trip=0.0),
          "a slower round trip must shorten the sleep, not be ignored")
    check(cal.safe_interval(mask24, hz, round_trip=10.0) == 0.0,
          "a round trip longer than the wrap should sleep zero, not negative")

    # A 32-bit DWT wraps in ~67 s at this clock and needs no shortening.
    check(cal.safe_interval(0xFFFFFFFF, hz) == cal.DEFAULT_INTERVAL,
          "a 32-bit counter should keep the default interval")
    check(cal.safe_interval(0, 0) == cal.DEFAULT_INTERVAL,
          "a device with no counter should not divide by zero")


FAKE_FROZEN = FAKE_PREAMBLE + r'''
# A device whose cycle counter never advances - what a real SL6806 does.
polls=1
[ -f "$STATE" ] && polls=$(( $(cat "$STATE") + 1 ))
echo "$polls" > "$STATE"

printf "$(esc %(magic)d)$(esc %(ver)d)$(esc 0)$(esc 0)$(esc $polls)$(esc 0)\
$(esc 120000000)$(esc 0)$(esc %(mask)d)$(esc %(raw)d)" > "$6"
'''


def test_the_shell_fakes_pack_the_bytes_they_claim_to():
    """The fixtures are shell now, so their packing is testable and untested.

    Every other test here reads a fake device through the whole tool, which
    means a mis-packed byte surfaces as a confusing empty stdout rather than
    as "the fake is wrong". This decodes what a fake actually writes, with
    the tool's own parser, so `esc` cannot drift silently.
    """
    with tempfile.TemporaryDirectory(prefix="sl6806cal.") as d:
        fake = os.path.join(d, "smtlink_dump")
        with open(fake, "w") as f:
            f.write(FAKE_FROZEN % {"ver": cal.STAT_VERSION, "mask": 0xFFFFFFFF,
                                   "magic": cal.STAT_MAGIC, "raw": 0xDEADBEEF})
        os.chmod(fake, 0o755)
        out = os.path.join(d, "r.bin")
        r = subprocess.run([fake, "chip", "6806", "read_mem2",
                            hex(cal.STAT_ADDR), hex(cal.STAT_LEN), out],
                           capture_output=True, text=True)
        data = None
        if os.path.isfile(out):
            with open(out, "rb") as f:
                data = f.read()

    check(r.returncode == 0, "the fake exited %d: %s", r.returncode, r.stderr)
    check(data is not None, "the fake wrote no output file at all")
    if r.returncode != 0 or data is None:
        return

    check(len(data) == cal.STAT_LEN,
          "the fake wrote %d bytes, the tool asks for %d",
          len(data), cal.STAT_LEN)

    st = cal.parse_stat(data)
    check(st is not None, "the tool's own parser rejected the fake's packet")
    if st is None:
        return
    # The high bits are the ones a byte-order or masking slip would lose.
    check(st["tick_raw"] == 0xDEADBEEF,
          "tick_raw round-tripped as 0x%08X", st["tick_raw"])
    check(st["tick_mask"] == 0xFFFFFFFF,
          "tick_mask round-tripped as 0x%08X", st["tick_mask"])
    check(st["version"] == cal.STAT_VERSION, "version %r", st["version"])
    check(st["f_cpu"] == 120000000, "f_cpu %r", st["f_cpu"])
    check(st["cycles"] == 0, "a frozen counter should read 0, got %r",
          st["cycles"])
    check(st["polls"] == 1, "first poll should be 1, got %r", st["polls"])


def frozen_run(mask, raw):
    with tempfile.TemporaryDirectory(prefix="sl6806cal.") as d:
        fake = os.path.join(d, "smtlink_dump")
        with open(fake, "w") as f:
            f.write(FAKE_FROZEN % {"ver": cal.STAT_VERSION, "mask": mask,
                                   "magic": cal.STAT_MAGIC, "raw": raw})
        os.chmod(fake, 0o755)
        return subprocess.run(
            [sys.executable, CALIBRATE, "--tool", fake, "--no-sudo",
             "--duration", "3", "--interval", "0.01"],
            capture_output=True, text=True)


def test_a_frozen_counter_is_diagnosed_not_reported_as_zero_hz():
    """Measured on real hardware: 281 good polls, every one the same count.

    Fitting that gives a slope of 0, and the tool used to print "measured
    clock: 0 Hz" and recommend rebuilding with F_CPU=0. It has to name the
    fault instead - especially since this same frozen counter is what makes
    delay() spin forever and wedge the device in poll mode.
    """
    r = frozen_run(mask=0, raw=0)
    out = r.stdout
    check(r.returncode != 0, "a device with no clock should not exit 0")
    check("0 Hz" not in out and "F_CPU=0" not in out,
          "the tool still reports a 0 Hz clock as if it were a measurement:\n%s",
          out[-600:])
    check("not running" in out or "no time source" in out,
          "the frozen counter was not named:\n%s", out[-600:])
    check("wedge" in out,
          "the connection to the poll-mode wedge is worth stating:\n%s",
          out[-600:])


def test_a_frozen_counter_distinguishes_junk_from_ungated():
    """raw != 0 and raw == 0 are different faults and get different advice."""
    junk = frozen_run(mask=0xFFFFFFFF, raw=0xDEADBEEF).stdout
    zero = frozen_run(mask=0, raw=0).stdout
    check("0xDEADBEEF" in junk,
          "the constant the register reads should be shown:\n%s", junk[-600:])
    check("not implemented" in junk,
          "a constant nonzero should be called out as an unimplemented "
          "register:\n%s", junk[-600:])
    check("never clocked" in zero,
          "a constant zero should be called out as a clocking problem:\n%s",
          zero[-600:])
    # A device claiming a 32-bit counter that does not move is also telling us
    # its own probe is the old, broken one.
    check("Rebuild" in junk or "rebuild" in junk,
          "a device whose probe passed wrongly should be told to rebuild:\n%s",
          junk[-600:])


MONITOR = os.path.join(ROOT, "tools", "sl6806-monitor")
mon = load("sl6806_monitor", MONITOR)


def test_a_silent_device_and_an_empty_one_are_told_apart():
    """The two reasons there is no console mean opposite things.

    No answer at all is a link problem. An answer that is not the console
    magic is a healthy device with nothing uploaded - which is the state every
    replug leaves it in, since SRAM does not survive one. Confusing the two
    sends you hunting a cable fault when you just have not uploaded.
    """
    dead = mon.describe_absence(None, 0x00826880, "LIBUSB_ERROR_TIMEOUT")
    check("not answering" in dead and "LIBUSB_ERROR_TIMEOUT" in dead,
          "a dead link should be named as one, and quote the reason: %r", dead)

    empty = mon.describe_absence(struct.pack("<8I", *([0] * 8)),
                                 0x00826880, None)
    check("no sketch resident" in empty,
          "a device with nothing uploaded should say so: %r", empty)
    check("upload" in empty,
          "the empty case should say what to do about it: %r", empty)
    check(dead != empty, "the two cases produced the same message")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            print("  %s" % name)
            fn()
    print("test_calibrate: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
