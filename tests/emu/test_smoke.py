#!/usr/bin/env python3
"""
Smoke tests: build a payload, run it under Unicorn, check what it printed.

    make -C tests/emu

These cover the seam the host tests cannot reach. tests/host proves the
console ring and the drawing primitives are correct as algorithms; these prove
that a real ARM image built by the real Makefile actually starts - .bss
cleared, C++ constructors run, the heap works, timekeeping finds a counter,
setup() and loop() execute, and the bytes land in the ring in the order a
monitor would read them.

Every failure mode they catch presents on hardware as "the device printed
nothing", which is the symptom that tells you least.

WHAT A FAILURE HERE MEANS. The emulator models memory, not peripherals, so a
test failing means the *software* is broken - not that the chip disagrees.
The converse does not hold: passing here says nothing about whether any
peripheral register in the framework is correct, because there is no
peripheral behind them.
"""

import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sl6806_emu import SL6806, EmuError, load_image   # noqa: E402

# Sentinel addresses the sketch answers itself; see cores/sl6806/sl6806_stat.h
# and cores/sl6806/sl6806_console.h.
STAT_ADDR = 0x53544154
STAT_MAGIC = 0x54415453
STAT_VERSION = 2
STAT_FMT = "<10I"
STAT_LEN = struct.calcsize(STAT_FMT)
CONSOLE_POLL_ADDR = 0x434F4E53

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", ".."))
BUILD = os.path.join(ROOT, "build", "emu")


def makefile_f_cpu():
    """The clock the images under test are actually built with.

    Read rather than hardcoded: F_CPU is a measured value now, and a test that
    pins it to a stale number fails for the wrong reason the next time a unit
    is calibrated.
    """
    with open(os.path.join(ROOT, "Makefile")) as f:
        for line in f:
            m = re.match(r"F_CPU\s*\?=\s*(\d+)", line)
            if m:
                return int(m.group(1))
    raise SystemExit("could not read the F_CPU default from the Makefile")


F_CPU = makefile_f_cpu()

failures = 0
checks = 0


def check(cond, msg, *args):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL " + (msg % args if args else msg))


def build(sketch, **make_vars):
    """Build a sketch with the project's own Makefile, as a user would."""
    out = os.path.join(BUILD, sketch + "".join(
        "-%s" % v for v in make_vars.values()))
    cmd = ["make", "-C", ROOT, "SKETCH=examples/%s" % sketch,
           "BUILD_DIR=%s" % out]
    cmd += ["%s=%s" % (k, v) for k, v in make_vars.items()]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        raise SystemExit("build failed for %s" % sketch)
    return os.path.join(out, sketch + ".bin")


def run(sketch, loops=0, **make_vars):
    emu = SL6806(load_image(build(sketch, **make_vars)))
    emu.start()
    if loops:
        emu.run_loop(loops)
    return emu


# ---------------------------------------------------------------- the tests

def test_hello_starts():
    """The bring-up test: everything the Hello sketch claims proves the chain."""
    emu = run("Hello")
    text, dropped = emu.console()

    check("=== SL6806 Arduino framework ===" in text, "banner is missing")
    check("variant : p20_player" in text,
          "variant.h is not reaching the sketch")
    # This line only appears if malloc and the C++ runtime both work.
    check("heap+String ok, 6806 reporting in" in text,
          "heap or String is broken; got:\n%s", text)
    check("GPIO    : NOT configured" in text,
          "GPIO should report unconfigured, not claim to work")
    check(dropped == 0, "%d bytes were dropped from the console ring", dropped)

    # _start must return in hook mode, having registered the ROM callback.
    check(emu.userfn is not None, "no USB user function registered")
    check(emu.idle_cb() not in (None, 0), "no idle callback in the table")


def test_hello_loop_advances():
    """loop() runs repeatedly, globals persist, and delay() terminates."""
    emu = run("Hello", loops=4)
    text, _ = emu.console()

    ticks = [ln for ln in text.splitlines() if ln.startswith("tick ")]
    check(len(ticks) == 4, "expected 4 ticks, got %d:\n%s", len(ticks), text)
    if len(ticks) == 4:
        # A static counter that increments proves .bss survives across calls.
        nums = [int(ln.split()[1]) for ln in ticks]
        check(nums == [0, 1, 2, 3], "tick counter did not advance: %s", nums)

        millis = [int(ln.split("millis=")[1]) for ln in ticks]
        check(millis == sorted(millis) and millis[-1] > millis[0],
              "millis() did not move forward: %s", millis)


def test_blink_reports_gpio_once():
    """digitalWrite must report, not silently no-op - and only once."""
    emu = run("Blink", loops=3)
    text, _ = emu.console()

    check("GPIO is not configured" in text,
          "digitalWrite() silently did nothing:\n%s", text)
    check(text.count("*** SL6806: GPIO is not configured ***") == 1,
          "the unconfigured warning repeated; it is meant to report once")


def test_gfx_runs_on_target():
    """The drawing stack executes as ARM code, not just on the host."""
    emu = run("GfxDemo", loops=2)
    text, _ = emu.console()
    check(len(text) > 0, "GfxDemo printed nothing at all")
    # With no LCD bus registered the panel must decline rather than pretend.
    check("no LCD bus" in text or "not available" in text.lower()
          or "panel" in text.lower(),
          "expected the display stack to say why it cannot draw:\n%s", text)


def test_romprobe_finds_nothing_in_clean_memory():
    """RomProbe on memory with no vendor routines must say so.

    This is the emulator checking the probe rather than the other way round:
    SRAM here is zeroed, so 'not resident' is the known-correct answer, and a
    probe that claimed otherwise would be reporting noise as code.
    """
    emu = run("RomProbe")
    text, _ = emu.console()
    check("SL6806 ROM routine probe" in text, "RomProbe did not run")
    check("0 of 6 entry points look like live code." in text,
          "probe should find nothing in zeroed memory:\n%s", text)


def test_takeover_mode_never_returns():
    """RUN_MODE=takeover spins in loop() instead of returning to the ROM."""
    emu = SL6806(load_image(build("Hello", RUN_MODE="takeover")))
    try:
        emu.start(max_insns=2_000_000)
    except EmuError as e:
        check("instruction budget" in str(e),
              "takeover build faulted instead of spinning: %s", e)
    else:
        check(False, "_start returned in takeover mode; it must not")

    # Either the banner is still in the ring, or loop() printed enough to push
    # it out. Both mean setup() completed before the spin; which one happens
    # depends on how many iterations fit in the instruction budget, which
    # moves with F_CPU.
    text, dropped = emu.console()
    check(text and ("=== SL6806 Arduino framework ===" in text or dropped),
          "takeover build produced no output before spinning")
    check(emu.userfn is None,
          "takeover mode must not register a ROM callback")


def test_poll_mode_cannot_block_the_usb_handler():
    """The regression that wedged a real device.

    RUN_MODE=poll runs loop() inside the ROM's USB command handler. Hello's
    loop() ends in delay(1000); left uncapped that blocks past the host's SCSI
    timeout, the endpoint desynchronises, and the device stops answering
    anything at all until it is unplugged. It is not slow polling - it is a
    hung device, and it happened.
    """
    import subprocess

    def one_loop(run_mode):
        path = build("Hello", RUN_MODE=run_mode)
        elf = path[:-4] + ".elf"
        out = subprocess.run(["arm-none-eabi-nm", elf],
                             capture_output=True, text=True).stdout
        fn = next((int(l.split()[0], 16) for l in out.splitlines()
                   if len(l.split()) == 3 and l.split()[2] == "sl6806_run_loop"),
                  None)
        emu = SL6806(load_image(path))
        emu.start()
        before = emu.cycles
        emu.call(fn)
        return emu, emu.cycles - before

    _, hook_cycles = one_loop("hook")
    emu, poll_cycles = one_loop("poll")

    # The point of the cap: an ordinary blocking sketch must not sit in the
    # handler for a second.
    ms = poll_cycles / (F_CPU / 1000.0)
    check(ms < 200, "poll mode blocked for %.0f ms in one loop() - the host's "
                    "SCSI timeout is about a second, so this wedges the "
                    "device", ms)
    check(poll_cycles < hook_cycles / 4,
          "poll mode did not shorten the delay: %d cycles vs %d uncapped",
          poll_cycles, hook_cycles)

    # And it must say so rather than silently running at the wrong speed.
    text, _ = emu.console()
    check("clamped" in text,
          "the delay was shortened without telling the sketch:\n%s", text)

    # hook mode must be left alone - the cap is poll-mode only.
    check(hook_cycles > F_CPU * 0.8,
          "hook mode should still honour delay(1000) in full: %d cycles for "
          "one second at %d Hz", hook_cycles, F_CPU)


def test_status_query_answers():
    """The clock-calibration query, decoded exactly as the host tool does."""
    emu = run("Hello")
    raw = emu.vendor_read(STAT_ADDR, STAT_LEN)

    check(raw is not None and len(raw) == STAT_LEN,
          "status query returned %r", None if raw is None else len(raw))
    if not raw or len(raw) != STAT_LEN:
        return

    (magic, ver, lo, hi, polls, loops,
     f_cpu, mode, mask, _raw) = struct.unpack(STAT_FMT, raw)
    check(magic == STAT_MAGIC, "status magic is 0x%08X, expected 0x%08X",
          magic, STAT_MAGIC)
    check(ver == STAT_VERSION, "status version is %d", ver)
    check(f_cpu == F_CPU, "device reports F_CPU=%d, not the build's %d", f_cpu, F_CPU)
    check(mode == 0, "run_mode should be 0 (hook) for a default build, got %d",
          mode)
    check(mask == 0xFFFFFFFF,
          "expected the 32-bit DWT counter here, got mask 0x%08X", mask)
    check(polls == 1, "first query should be poll 1, got %d", polls)
    check(((hi << 32) | lo) > 0, "the cycle counter read back as zero")


def test_status_cycles_advance_and_do_not_drive_loop():
    """The query must be side-effect free apart from sampling the counter.

    tools/sl6806-calibrate times these round trips, so every one has to cost
    the same. If a status read also ran loop(), the measurement would pick up
    whatever the sketch happened to do that time round as clock jitter.
    """
    emu = run("Hello", RUN_MODE="poll")

    def status():
        return struct.unpack(STAT_FMT, emu.vendor_read(STAT_ADDR, STAT_LEN))

    _, _, lo1, hi1, polls1, loops1, _, mode, _, _ = status()
    _, _, lo2, hi2, polls2, loops2, _, _, _, _ = status()

    check(mode == 2, "run_mode should be 2 (poll), got %d", mode)
    check(((hi2 << 32) | lo2) > ((hi1 << 32) | lo1),
          "the cycle counter did not advance between two queries")
    check(polls2 == polls1 + 1, "poll counter went %d -> %d", polls1, polls2)
    check(loops1 == 0 and loops2 == 0,
          "a status query ran loop() (%d -> %d); it must not, even in poll "
          "mode", loops1, loops2)

    # A console poll, by contrast, is exactly what does drive loop() here.
    emu.vendor_read(CONSOLE_POLL_ADDR, 64)
    _, _, _, _, _, loops3, _, _, _, _ = status()
    check(loops3 == 1,
          "a console poll should have driven loop() once in poll mode, "
          "loops=%d", loops3)


def test_status_counts_the_idle_callback():
    """`loops` is how the host finds out whether the ROM's cb2 really fires.

    On hardware, poll the status twice a second apart: if loops has moved
    while nothing else drove the sketch, the idle hook is real. Here the
    emulator plays the ROM, so driving the callback must move the counter.
    """
    emu = run("Hello")
    before = struct.unpack(STAT_FMT, emu.vendor_read(STAT_ADDR, STAT_LEN))[5]
    emu.run_loop(3)
    after = struct.unpack(STAT_FMT, emu.vendor_read(STAT_ADDR, STAT_LEN))[5]
    check(after - before == 3,
          "three idle callbacks counted as %d loop() calls", after - before)


def test_a_dead_cycle_counter_does_not_hang():
    """The fault a real SL6806 has, and the wedge it caused.

    On the device the DWT probe passed - CYCCNT read nonzero after being
    written 0 - while the counter never actually advanced. sl6806_cycles()
    then returned 0 forever, so `while (cycles < target)` in delay() could
    never terminate. In RUN_MODE=poll that loop runs inside the boot ROM's USB
    handler, so the device left the bus and stayed off it until unplugged.
    Measured as 281 status polls all reporting the same cycle count.

    cycles_per_read=0 models exactly that: a counter register that reads back
    a constant. The test is that _start finishes and loop() returns at all.
    """
    emu = SL6806(load_image(build("Hello", RUN_MODE="poll")), cycles_per_read=0)
    emu.start()

    text, _ = emu.console()
    check(text.count("*** SL6806: no working cycle counter ***") == 1,
          "a dead counter must be reported exactly once at startup, not "
          "silently tolerated and not repeated:\n%s", text)

    # The real assertion: each of these returns instead of spinning to the
    # instruction budget. Hello's loop() ends in delay(1000).
    for _ in range(10):
        check(emu.vendor_read(CONSOLE_POLL_ADDR, 64) is not None,
              "a console poll failed while the counter was dead")

    st = struct.unpack(STAT_FMT, emu.vendor_read(STAT_ADDR, STAT_LEN))
    check(st[8] == 0,
          "tick_mask should be 0 when no counter advances, got 0x%08X", st[8])
    check(((st[3] << 32) | st[2]) == 0,
          "a dead counter should report 0 cycles, not a running total")
    check(st[5] == 10, "all ten console polls should have run loop(), got %d",
          st[5])

    # Ten delay() calls must not each re-announce. The polls above drained the
    # original warning out of the ring, so any copy still in there is a repeat.
    text, _ = emu.console()
    check("*** SL6806: no working cycle counter ***" not in text,
          "delay() re-announced the dead counter; it is meant to report "
          "once:\n%s", text)


def test_changing_the_flags_rebuilds():
    """Switching RUN_MODE must not silently re-upload the previous binary.

    RUN_MODE, MODE, F_CPU and EXTRA_FLAGS are all -D on the command line, and
    none is a file, so make cannot see them change. Left alone it rebuilds
    nothing and hands you the last mode's image byte for byte - which does not
    fail, it just makes every measurement taken with it a lie about a
    different binary. That happened on the bench, hence this test.
    """
    out = os.path.join(BUILD, "flags")
    subprocess.run(["rm", "-rf", out], check=False)

    def build_here(**make_vars):
        cmd = ["make", "-C", ROOT, "SKETCH=examples/Hello",
               "BUILD_DIR=%s" % out]
        cmd += ["%s=%s" % (k, v) for k, v in make_vars.items()]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit("build failed:\n%s\n%s" % (r.stdout[-1500:],
                                                        r.stderr[-1500:]))
        with open(os.path.join(out, "Hello.bin"), "rb") as f:
            return f.read()

    poll = build_here(RUN_MODE="poll")
    check(build_here(RUN_MODE="poll") == poll,
          "rebuilding with identical flags produced a different binary - the "
          "flag stamp is churning and every build is now a full rebuild")
    check(build_here(RUN_MODE="hook") != poll,
          "switching RUN_MODE rebuilt nothing: hook and poll came out "
          "identical, so an upload would run the wrong image")
    check(build_here(RUN_MODE="poll",
                     EXTRA_FLAGS="-DSL6806_POLL_BLOCK_LIMIT_MS=1") != poll,
          "changing EXTRA_FLAGS rebuilt nothing")


def test_no_stray_peripheral_writes():
    """A sketch with no drivers must not poke unknown MMIO.

    The framework's whole posture is that unknown registers stay untouched.
    A write into 0x40000000 from Hello would mean something started guessing.
    """
    emu = run("Hello", loops=2)
    check(not emu.mmio_writes,
          "Hello wrote to peripheral space: %s",
          ["0x%08X from pc 0x%08X" % (a, pc)
           for pc, a, _s, _v in emu.mmio_writes[:5]])


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    print("SL6806 emulator smoke tests")
    for t in tests:
        print("  %s" % t.__name__)
        try:
            t()
        except EmuError as e:
            global failures
            failures += 1
            print("    FAIL emulator error: %s" % e)

    print("test_smoke: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
