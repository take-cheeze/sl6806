#!/usr/bin/env python3
"""
Tests for actions-cardreader-try-subcmd's pure helpers.

Nothing here touches /dev/sg* or SG_IO - build_cdb and describe_result are
plain functions precisely so the CDB shape and the "is this worth reporting"
logic can be checked without a device, matching what a real run against
0x21 is known to produce (see docs/ACTIONS_CARDREADER.md).

    python3 tests/tools/test_actions_try_subcmd.py
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


tool = load("actions_cardreader_try_subcmd",
           os.path.join(ROOT, "tools", "actions-cardreader-try-subcmd"))

failures = 0
checks = 0


def check(cond, msg, *args):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL " + (msg % args if args else msg))


def test_build_cdb_matches_the_captured_adfu_reboot_shape():
    # Captured from a real device: cb 21 00 00 00 00 00 02 00 00 00 00 00 00 00 00
    cdb = tool.build_cdb(0x21, 2)
    expected = bytes([0xcb, 0x21, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0])
    check(cdb == expected, "got %s, expected %s", cdb.hex(" "), expected.hex(" "))
    check(len(cdb) == 16, "CDB must be 16 bytes, got %d", len(cdb))


def test_build_cdb_places_subcmd_and_length_correctly():
    cdb = tool.build_cdb(0x01, 4)
    check(cdb[0] == 0xcb, "opcode must stay 0xcb")
    check(cdb[1] == 0x01, "subcmd goes in cdb[1]")
    check(cdb[7] == 4, "recv_len goes in cdb[7]")
    check(all(b == 0 for i, b in enumerate(cdb) if i not in (0, 1, 7)),
          "every other byte must be zero")


def test_build_cdb_rejects_out_of_range_values():
    try:
        tool.build_cdb(0x100, 2)
        check(False, "subcmd > 0xff should raise")
    except ValueError:
        check(True, "-")
    try:
        tool.build_cdb(0x21, -1)
        check(False, "negative recv_len should raise")
    except ValueError:
        check(True, "-")


def test_describe_result_flags_the_known_accepted_reply():
    text = tool.describe_result(0, bytes([0xff, 0x00]))
    check("GOOD" in text, "status 0 should read as GOOD")
    check("accepted reply" in text, "ff 00 should be flagged as the accepted shape")


def test_describe_result_flags_the_known_rejected_reply():
    """This is the real reply captured for subcmd 0x21 on this chip."""
    text = tool.describe_result(0, bytes([0xff, 0x3d]))
    check("GOOD" in text, "the SCSI status was GOOD even though the payload rejects")
    check("not confirmed accepted" in text,
          "ff 3d must not be reported as the accepted shape")
    check("accepted reply" not in text,
          "ff 3d must not trigger the ff-00 celebration message")


def test_describe_result_reports_check_condition():
    text = tool.describe_result(2, b"")
    check("CHECK CONDITION" in text, "nonzero status should be flagged, not silently GOOD")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            print("  %s" % name)
            fn()
    print("test_actions_try_subcmd: %d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
