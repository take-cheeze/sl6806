#!/usr/bin/env python3
"""
Tests for sl6806-boot, the HLKJ bootloader mapper.

WHY THIS FILE EXISTS. This tool answers "what run address is this file offset,
and who loads this constant" for a program that runs from SRAM at a base
nothing in the file states twice. Every answer it gives goes straight into
notes 23 and into a driver's provenance comments, and a wrong bias produces
addresses that look perfectly plausible and are off by a fixed amount - the
exact failure mode that put the wrong load address in 7h for five searches
(see 7m, "one number").

So three things are worth pinning down:

  * **the bias**, both directions. Address to offset and offset to address
    have to round-trip, and the known landmark - the SD/MMC host base at file
    0x00822958 in run terms - has to resolve where notes 23 says it does.
  * **the CRCs**, which are the tool's only way of saying "this dump is
    intact". A checker that passes a corrupted segment is worse than none,
    so this flips a byte and requires the check to fail.
  * **the literal cross-reference**, which is what actually found the SD
    host. It decodes two Thumb encodings by hand; a bug in either silently
    misses call sites rather than reporting an error.

The end-to-end checks run only when dump.bin is present, the way
test_sensortab.py's do.

    python3 tests/tools/test_boot.py
"""

import importlib.machinery
import importlib.util
import os
import struct
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", ".."))
TOOL = os.path.join(ROOT, "tools", "sl6806-boot")
DUMP = os.path.join(ROOT, "dump.bin")

failures = 0
checks = 0


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        print("  FAIL %s" % what)
        failures += 1


def load_tool():
    """Import a file with no .py extension."""
    loader = importlib.machinery.SourceFileLoader("sl6806_boot", TOOL)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def synthetic(mod):
    """A minimal image with a valid header, so the tests do not need a dump."""
    seg = bytearray(0x200)
    # a literal pool entry holding a recognisable "peripheral base", and an
    # LDR (T1) that points at it. PC for a T1 load is (addr + 4) & ~3.
    struct.pack_into("<I", seg, 0x100, 0x40003000)
    # place the instruction at segment offset 0xF8: pc = 0xFC, +4*1 = 0x100
    struct.pack_into("<H", seg, 0x0F8, 0x4801)

    blob = bytearray(0x60 + len(seg))
    blob[0:4] = b"HLKJ"
    struct.pack_into("<I", blob, 0x04, 0x0081FC00)
    struct.pack_into("<I", blob, 0x08, 0x00820001)
    struct.pack_into("<I", blob, 0x10, len(seg))
    blob[0x60:] = seg
    struct.pack_into("<I", blob, 0x14, mod.crc16(bytes(seg)))
    struct.pack_into("<I", blob, 0x5C, mod.crc16(bytes(blob[0x00:0x5C])))
    return bytes(blob)


def test_header_and_bias(mod):
    print("header, bias and CRCs")
    boot = mod.Boot(synthetic(mod))

    seg_ok, hdr_ok = boot.verify()
    check(seg_ok, "the segment CRC verifies")
    check(hdr_ok, "the header CRC verifies")

    check(boot.load == 0x0081FC00, "load base comes from +0x04")
    check(boot.entry == 0x00820001, "entry comes from +0x08")

    # notes 4: run address = file offset + 0x81FBA0, because the segment
    # starts at file 0x60 and loads at 0x0081FC00.
    check(boot.bias == 0x0081FBA0, "the bias is load - 0x60")
    check(boot.o2a(0x60) == 0x0081FC00, "file 0x60 is the load address")
    check(boot.a2o(0x0081FC00) == 0x60, "and back again")
    for off in (0x60, 0x1234, 0xE000):
        check(boot.a2o(boot.o2a(off)) == off, "offset %#x round-trips" % off)

    # The synthetic segment is only 0x200 bytes, so the real entry point is
    # past its end - which is itself worth checking, because `contains` is
    # what stops a disassembly request running off the end of the buffer.
    check(boot.contains(0x0081FC00), "the load address is inside the segment")
    check(boot.contains(0x0081FC00 + 0x1FF), "and so is its last byte")
    check(not boot.contains(0x0081FC00 + 0x200), "one past the end is not")
    check(not boot.contains(0x00C10000), "an XIP address is not")
    check(not boot.contains(0x00800000), "nor is SRAM below the load base")


def test_corruption_is_caught(mod):
    print("a corrupted segment fails its CRC")
    blob = bytearray(synthetic(mod))
    blob[0x80] ^= 0x01
    boot = mod.Boot(bytes(blob))
    seg_ok, hdr_ok = boot.verify()
    check(not seg_ok, "one flipped bit fails the segment CRC")
    check(hdr_ok, "and leaves the header CRC alone")

    blob = bytearray(synthetic(mod))
    blob[0x10] ^= 0x01
    boot = mod.Boot(bytes(blob))
    check(not boot.verify()[1], "a flipped header byte fails the header CRC")


def test_not_a_dump(mod):
    print("something that is not a dump")
    try:
        mod.Boot(b"\x00" * 0x200)
    except ValueError:
        check(True, "missing HLKJ magic is refused")
    else:
        check(False, "missing HLKJ magic is refused")


def test_xref(mod):
    print("the literal cross-reference")
    boot = mod.Boot(synthetic(mod))
    hits = list(boot.xref(0x40003000))
    check(len(hits) == 1, "the planted literal is found exactly once")
    if hits:
        ia, la = hits[0]
        check(ia == boot.o2a(0x60 + 0x0F8), "at the instruction that loads it")
        check(la == boot.o2a(0x60 + 0x100), "pointing at the right pool entry")

    check(list(boot.xref(0xDEADBEEF)) == [], "an absent literal finds nothing")


def test_against_the_real_image(mod):
    if not os.path.exists(DUMP):
        print("the real image (skipped: no dump.bin)")
        return

    print("the real image")
    with open(DUMP, "rb") as f:
        boot = mod.Boot(f.read())

    seg_ok, hdr_ok = boot.verify()
    check(seg_ok and hdr_ok, "both CRCs verify on the shipped dump")
    check(boot.load == 0x0081FC00, "the real load base is 0x0081FC00")
    check(boot.seglen == 0xEF6C, "the real segment length is 0xEF6C")

    # notes 23: the SD/MMC host base appears exactly once in the bootloader,
    # loaded at 0x008227F0 out of a pool entry at 0x00822958. That single
    # reference is the whole reason the block was findable.
    hits = list(boot.xref(0x40003000))
    check(len(hits) == 1, "0x40003000 is referenced exactly once")
    if hits:
        check(hits[0][0] == 0x008227F0, "loaded at 0x008227F0")
        check(hits[0][1] == 0x00822958, "from the pool entry at 0x00822958")

    # The strings the application's own tools cannot see, because they live
    # below file 0x10000.
    found = {t for _, t in boot.strings()}
    for want in ("HAL_sd_disk_init", "sdio(i):stop", "HAL_SD_Read_new"):
        check(want in found, "the bootloader carries %r" % want)

    addr = next(a for a, t in boot.strings() if t == "HAL_sd_disk_init")
    check(boot.contains(addr), "and its address is inside the segment")


def test_cli(mod):
    if not os.path.exists(DUMP):
        print("the command line (skipped: no dump.bin)")
        return

    print("the command line")
    out = subprocess.run([sys.executable, TOOL, DUMP],
                         capture_output=True, text=True)
    check(out.returncode == 0, "a bare run succeeds")
    check("segment CRC  ok" in out.stdout, "and reports the CRC")

    out = subprocess.run([sys.executable, TOOL, DUMP, "--xref", "0x40003000"],
                         capture_output=True, text=True)
    check("0x008227f0" in out.stdout.lower(), "--xref finds the SD host")


def main():
    print("test_boot")
    mod = load_tool()

    test_header_and_bias(mod)
    test_corruption_is_caught(mod)
    test_not_a_dump(mod)
    test_xref(mod)
    test_against_the_real_image(mod)
    test_cli(mod)

    print("test_boot: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
