#!/usr/bin/env python3
"""
Tests for sl6806-sensortab, the sensor init-table extractor.

WHY THIS FILE EXISTS. The tables it emits are compiled into a sketch and then
written, byte for byte, into a chip with no datasheet. Nothing downstream can
tell a correct table from a subtly wrong one - the sensor either works or is
silent, and a silent sensor has a dozen other explanations. So the walker has
to be checkable here.

Three things are worth pinning down, and each has a way of going wrong that
would not be obvious:

  * the terminator. FF FF ends the table, but FF is a perfectly ordinary
    register number and 0xFF a perfectly ordinary value - only the pair ends
    it. A walker that stops on either byte alone truncates silently.
  * the paging. Register 0xF0 sets the high byte of a 16-bit register number
    and is not itself a write to the sensor's register 0xF0's target. Getting
    that wrong shifts every register number after the first page write.
  * the real image. The crop window in the vendor's own table B decodes to
    648x488 inside 1280x720, which 7h derived independently. Reproducing that
    from the bytes is the end-to-end check, and it runs only when dump.bin is
    present.

    python3 tests/tools/test_sensortab.py
"""

import importlib.machinery
import importlib.util
import os
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", ".."))
TOOL = os.path.join(ROOT, "tools", "sl6806-sensortab")
DUMP = os.path.join(ROOT, "dump.bin")


def load(name, path):
    """Import a tool that has no .py extension."""
    spec = importlib.util.spec_from_loader(
        name, importlib.machinery.SourceFileLoader(name, path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


st = load("sensortab", TOOL)

checks = 0
failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        print("  FAIL %s" % what)
        failures += 1


def image(pairs, at=st.TABLE_A):
    """A fake flash image with one table at the tool's own table-A address."""
    off = at - st.FLASH_BASE
    body = b"".join(bytes(p) for p in pairs) + b"\xFF\xFF"
    return b"\x00" * off + body + b"\x00" * 64


def test_walk():
    print("walking")

    pairs = [(0xF0, 0x30), (0x01, 0xFF), (0x30, 0x10)]
    got, end = st.read_table(image(pairs), st.TABLE_A)
    check(got == pairs, "the pairs come back in order")
    check(end == st.TABLE_A + 2 * len(pairs) + 2, "the end is past the terminator")

    # 0xFF as a value and as a register, neither of which ends a table.
    pairs = [(0x01, 0xFF), (0xFF, 0x01), (0x02, 0x03)]
    got, _ = st.read_table(image(pairs), st.TABLE_A)
    check(got == pairs, "only the FF FF *pair* terminates")

    got, end = st.read_table(image([]), st.TABLE_A)
    check(got == [] and end == st.TABLE_A + 2, "an empty table is not an error")


def test_paging():
    print("paging")

    pairs = [(0x03, 0x11),               # page 0 at the start
             (0xF0, 0x32), (0x00, 0x01), (0x01, 0x40),
             (0xF0, 0x00), (0x70, 0x6B)]
    got = list(st.paged(pairs))
    check([(r, v) for r, v, _p in got] ==
          [(0x0003, 0x11), (0x3200, 0x01), (0x3201, 0x40), (0x0070, 0x6B)],
          "0xF0 sets the high byte and is not itself emitted")
    check(all(p == 0 for _r, _v, p in got[:1]), "the page starts at 0")
    check(len(got) == len(pairs) - 2, "the two page writes are consumed")


def test_against_the_real_image():
    if not os.path.exists(DUMP):
        print("real image: skipped, no dump.bin")
        return

    print("the real image")
    fw = open(DUMP, "rb").read()

    a, a_end = st.read_table(fw, st.TABLE_A)
    b, _ = st.read_table(fw, st.TABLE_B)
    check(len(a) == 192, "table A is 192 pairs, as 7h says")
    check(len(b) == 203, "table B is 203 pairs")
    check(a_end == st.TABLE_B, "table A ends exactly where table B begins")

    # 7h's crop decode, reproduced from the bytes rather than transcribed.
    crop = {}
    for reg16, val, _page in st.paged(b):
        if reg16 in st.CROP:
            crop[reg16] = val
        elif reg16 - 1 in st.CROP:
            crop[reg16 - 1] = (crop.get(reg16 - 1, 0) << 8) | val
    check(crop.get(0x3200) == 320 and crop.get(0x3204) == 967,
          "table B crops x to 320..967")
    check(crop.get(0x3202) == 120 and crop.get(0x3206) == 607,
          "and y to 120..607")
    check(crop.get(0x3204) - crop.get(0x3200) + 1 == 648, "648 wide")
    check(crop.get(0x3206) - crop.get(0x3202) + 1 == 488, "488 tall")

    # Table A has no crop block; that is what makes it the full-array one.
    a_regs = set(reg16 for reg16, _v, _p in st.paged(a))
    check(not (a_regs & set(st.CROP)), "table A writes no crop registers")


def test_generated_header_is_current():
    """The checked-in header must match what the tool produces now.

    A generated file that has drifted from its generator is worse than no
    generated file: it looks authoritative and is not. This is cheap to check
    and the failure mode - someone hand-edits the header - is a real one.
    """
    header = os.path.join(ROOT, "examples", "CameraDemo", "sc101_init.h")
    if not (os.path.exists(DUMP) and os.path.exists(header)):
        print("generated header: skipped")
        return

    print("the generated header")
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "sc101_init.regen.h")
    try:
        subprocess.run([sys.executable, TOOL, DUMP, "--header", out],
                       check=True, stdout=subprocess.DEVNULL)
        check(open(out).read() == open(header).read(),
              "examples/CameraDemo/sc101_init.h is what the tool emits today")
    finally:
        if os.path.exists(out):
            os.remove(out)


def main():
    print("test_sensortab")
    test_walk()
    test_paging()
    test_against_the_real_image()
    test_generated_header_is_current()
    print("test_sensortab: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
