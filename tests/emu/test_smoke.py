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
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sl6806_emu import SL6806, EmuError, load_image   # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", ".."))
BUILD = os.path.join(ROOT, "build", "emu")

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

    text, _ = emu.console()
    check("=== SL6806 Arduino framework ===" in text,
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
    ms = poll_cycles / (120000000 / 1000.0)
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
    check(hook_cycles > 100000000,
          "hook mode should still honour delay(1000) in full, got %d cycles",
          hook_cycles)


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
