#!/usr/bin/env python3
"""
Tests for actions-cardreader-probe's response parsers.

Neither `inquiry` nor `adfu_info` output has ever been captured from this
particular chip - see the tool's docstring for why. These tests only pin
down the two parsers against the shapes actions_dump.c is known to print
(print_mem's hexdump, and the "\\0CADFUD" adfu_info layout documented in
3rd/actions_flash/README.md), so a change to either parser is caught here
rather than by a confusing failure on real hardware.

    python3 tests/tools/test_actions_cardreader.py
"""

import importlib.machinery
import importlib.util
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", ".."))


def load(name, path):
    """Import a tool that has no .py extension."""
    spec = importlib.util.spec_from_loader(
        name, importlib.machinery.SourceFileLoader(name, path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


probe = load("actions_cardreader_probe",
             os.path.join(ROOT, "tools", "actions-cardreader-probe"))

failures = 0
checks = 0


def check(cond, msg, *args):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL " + (msg % args if args else msg))


def hexdump(data):
    """Render bytes the way actions_dump.c's print_mem does."""
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hexpart = " ".join("%02x" % b for b in chunk)
        hexpart += "   " * (16 - len(chunk))
        ascii_part = "".join(chr(b) if 0x20 < b < 0x7f else "." for b in chunk)
        lines.append("%s |%s|" % (hexpart, ascii_part))
    return "\n".join(lines) + "\n"


# A standard 36-byte SCSI INQUIRY reply: bytes 8-15 vendor, 16-31 product,
# 32-35 revision. Fabricated, since no real capture exists for this chip.
INQUIRY_DATA = (b"\x00\x80\x02\x02\x20\x00\x00\x10"
                b"ACTIONS "
                b"USB CARDREADER  "
                b"1.00")


def test_inquiry_parses_standard_layout():
    text = "result (36):\n" + hexdump(INQUIRY_DATA)
    ident = probe.parse_inquiry(text)
    check(ident is not None, "a well-formed 36-byte reply failed to parse")
    if ident:
        check(ident == ("ACTIONS", "USB CARDREADER", "1.00"),
              "got %r, expected ('ACTIONS', 'USB CARDREADER', '1.00')", ident)


def test_inquiry_rejects_short_reply():
    check(probe.parse_inquiry("result (4):\n00 80 02 02  |....|\n") is None,
          "a truncated reply should not parse")
    check(probe.parse_inquiry("") is None, "empty input should not parse")
    check(probe.parse_inquiry("LIBUSB_ERROR_PIPE") is None,
          "an error message should not parse as an inquiry")


def test_inquiry_ignores_surrounding_noise():
    noisy = "kernel driver is active\n" + hexdump(INQUIRY_DATA) + "\ntrailing\n"
    check(probe.parse_inquiry(noisy) == ("ACTIONS", "USB CARDREADER", "1.00"),
          "surrounding output changed the parse")


# adfu_info layout per 3rd/actions_flash/README.md: "\0CADFUD" then two id
# bytes at offset 7-8, then padding. ATJ2127's real id is 0x10d6.
ADFU_INFO_ATJ2127 = b"\0CADFUD" + bytes([0x10, 0xd6]) + b"A" + b"\0" * 8


def test_adfu_info_decodes_known_chip():
    text = hexdump(ADFU_INFO_ATJ2127)
    chip_id = probe.parse_adfu_info(text)
    check(chip_id == 0x10d6, "got 0x%r, expected 0x10d6" % (chip_id,))
    check(probe.KNOWN_CHIPS.get(chip_id) == "ATJ2127",
          "0x10d6 should resolve to ATJ2127 via KNOWN_CHIPS")


def test_adfu_info_reports_unknown_chip_id_without_crashing():
    unknown = b"\0CADFUD" + bytes([0x20, 0xd6]) + b"A" + b"\0" * 8
    chip_id = probe.parse_adfu_info(hexdump(unknown))
    check(chip_id == 0x20d6, "got 0x%r, expected 0x20d6" % (chip_id,))
    check(probe.KNOWN_CHIPS.get(chip_id) is None,
          "0x20d6 is not a chip id this tool claims to recognise")


def test_adfu_info_rejects_wrong_magic():
    check(probe.parse_adfu_info(hexdump(b"garbage!" + b"\0" * 8)) is None,
          "a reply without the \\0CADFUD magic should not parse")
    check(probe.parse_adfu_info("") is None, "empty input should not parse")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            print("  %s" % name)
            fn()
    print("test_actions_cardreader: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
