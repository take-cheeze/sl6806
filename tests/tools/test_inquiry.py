#!/usr/bin/env python3
"""
Tests for sl6806-upload's USB mode detection.

WHY THIS FILE EXISTS. The mode check used to be a substring match against
smtlink_dump's stdout, looking for "SMTLINK DEVICE". That string never appears
in that output: it is a hexdump whose ASCII column renders 0x20 as ".", and
the T10 vendor field ends at byte 15, which is a line boundary. So the two
halves land on different lines.

The visible symptom was a spurious "could not confirm bootloader mode" warning
on a device that was in bootloader mode. The invisible one was worse: the
card-reader branch could never fire either, so the check that exists to stop
you uploading into card-reader mode was dead.

Both are the kind of bug the CI `tools` job cannot catch by running --help,
which is why the parse is now a pure function with tests around it.

    python3 tests/tools/test_inquiry.py
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


upload = load("sl6806_upload", os.path.join(ROOT, "tools", "sl6806-upload"))

failures = 0
checks = 0


def check(cond, msg, *args):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL " + (msg % args if args else msg))


# Verbatim from a real device in bootloader mode.
BOOTLOADER = """kernel driver is active, trying to detach
result (36):
00 80 02 02 20 00 00 10 53 4d 54 4c 49 4e 4b 20  |........SMTLINK.|
44 45 56 49 43 45 20 20 20 20 20 20 20 20 20 20  |DEVICE..........|
32 2e 30 30                                      |2.00|
"""

# The same shape with the card-reader identity, whose typos are the vendor's.
CARDREADER = """result (36):
00 80 02 02 20 00 00 10 53 53 54 4c 49 4e 4b 20  |........SSTLINK.|
44 44 56 49 43 45 20 20 20 20 20 20 20 20 20 20  |DDVICE..........|
32 2e 30 32                                      |2.02|
"""


def test_bootloader_is_recognised():
    ident = upload.parse_inquiry(BOOTLOADER)
    check(ident is not None, "a real bootloader response failed to parse")
    if ident:
        check(ident == ("SMTLINK", "DEVICE", "2.00"),
              "got %r, expected ('SMTLINK', 'DEVICE', '2.00')", ident)
        check((ident[0], ident[1]) == upload.BOOTLOADER_ID,
              "parsed identity does not match BOOTLOADER_ID")


def test_cardreader_is_recognised():
    ident = upload.parse_inquiry(CARDREADER)
    check(ident is not None, "a card-reader response failed to parse")
    if ident:
        check(ident == ("SSTLINK", "DDVICE", "2.02"),
              "got %r, expected ('SSTLINK', 'DDVICE', '2.02')", ident)
        check((ident[0], ident[1]) == upload.CARDREADER_ID,
              "parsed identity does not match CARDREADER_ID - the branch that "
              "refuses to upload in card-reader mode would not fire")


def test_the_two_modes_are_distinguished():
    """The whole point: these must not both look like the same device."""
    check(upload.parse_inquiry(BOOTLOADER) != upload.parse_inquiry(CARDREADER),
          "bootloader and card-reader modes parse identically")


def test_noise_lines_are_ignored():
    """Headers and driver chatter must not corrupt the byte stream."""
    noisy = "some unrelated warning\n" + BOOTLOADER + "\ntrailing junk 12345\n"
    check(upload.parse_inquiry(noisy) == ("SMTLINK", "DEVICE", "2.00"),
          "surrounding output changed the parse")


def test_short_or_absent_response():
    check(upload.parse_inquiry("") is None, "empty input should not parse")
    check(upload.parse_inquiry("LIBUSB_ERROR_PIPE") is None,
          "an error message should not parse as an inquiry")
    check(upload.parse_inquiry("result (4):\n00 80 02 02  |....|\n") is None,
          "a truncated response should not parse")


def test_ascii_column_is_not_what_is_parsed():
    """Guard the exact regression: matching the rendered text must not be how
    this works. The literal string is absent from a correct response."""
    check("SMTLINK DEVICE" not in BOOTLOADER,
          "the test fixture is wrong - it should NOT contain the literal "
          "string, that is the entire point")
    check(upload.parse_inquiry(BOOTLOADER)[:2] == ("SMTLINK", "DEVICE"),
          "identity must come from the hex bytes, not the ASCII column")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            print("  %s" % name)
            fn()
    print("test_inquiry: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
