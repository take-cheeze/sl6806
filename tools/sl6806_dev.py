"""
sl6806_dev - talking to a running SL6806 sketch.

Not a command-line tool; the shared half of sl6806-monitor and
sl6806-clockcal. Both need the same three things: find smtlink_dump, read and
write device memory through it, and look up where the build put the sketch's
structures.

There is no protocol of our own down here. Every operation is one invocation
of the stock smtlink_dump, which is what makes the whole scheme safe: nothing
in this file can put the device in a state the vendor tool could not.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC = 0x36384C53          # "SL68"
VERSION = 2
POLL_ADDR = 0x434F4E53      # "CONS" - the console-poll sentinel

# struct sl6806_console_t header: magic,size,version,head,tail,dropped,
#                                 rx_head,rx_tail
HDR_FMT = "<IIIIIIII"
HDR_LEN = struct.calcsize(HDR_FMT)

# struct sl6806_console_pkt_t: len,flags,pending,data[60]
PKT_FMT = "<BBH60s"
PKT_LEN = struct.calcsize(PKT_FMT)

F_OVERFLOW = 0x01
RX_SIZE = 256

# struct sl6806_clockstamp_t: magic,f_cpu,seq,cyc_lo,cyc_hi
CLK_MAGIC = 0x4B4C4353      # "SCLK"
CLK_FMT = "<IIIII"
CLK_LEN = struct.calcsize(CLK_FMT)


def find_tool(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for c in [os.path.join(here, "..", "3rd", "smartlink_flash", "smtlink_dump"),
              shutil.which("smtlink_dump") or ""]:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.abspath(c)
    sys.exit("error: smtlink_dump not found; pass --tool <path>")


def read_symbols(path):
    """Parse the .sym file the build emits: {name: address}."""
    syms = {}
    try:
        with open(path) as f:
            for line in f:
                m = re.match(r"([0-9a-fA-F]+)\s+\S+\s+(\S+)\s*$", line)
                if m:
                    syms[m.group(2)] = int(m.group(1), 16)
    except OSError as e:
        sys.exit("error: %s" % e)
    return syms


def symbol(path, name):
    """One address, or exit explaining what to rebuild."""
    syms = read_symbols(path)
    if name not in syms:
        sys.exit("error: no %s symbol in %s (was the sketch built by this "
                 "framework, and is it up to date? try 'make clean')"
                 % (name, path))
    return syms[name]


class Device:
    def __init__(self, tool, sudo):
        self.tool = tool
        self.sudo = sudo
        self.tmp = tempfile.mkdtemp(prefix="sl6806.")

    def _run(self, args):
        cmd = (["sudo"] if self.sudo else []) + [self.tool, "chip", "6806"] + args
        return subprocess.run(cmd, capture_output=True, text=True)

    def read(self, addr, size):
        """Read device memory, or None on failure."""
        if size <= 0:
            return b""
        out = os.path.join(self.tmp, "r.bin")
        try:
            os.unlink(out)
        except OSError:
            pass
        r = self._run(["read_mem2", hex(addr), hex(size), out])
        if r.returncode != 0 or not os.path.isfile(out):
            return None
        with open(out, "rb") as f:
            data = f.read()
        return data if len(data) == size else None

    def write(self, addr, data):
        src = os.path.join(self.tmp, "w.bin")
        with open(src, "wb") as f:
            f.write(data)
        r = self._run(["write_mem", hex(addr), "0", hex(len(data)), src])
        return r.returncode == 0

    def poll_console(self):
        """One round trip: fetch and consume queued output.

        Returns (data, overflowed, pending) or None if the device did not
        answer."""
        raw = self.read(POLL_ADDR, PKT_LEN)
        if raw is None or len(raw) != PKT_LEN:
            return None
        n, flags, pending, data = struct.unpack(PKT_FMT, raw)
        if n > 60:
            return None
        return data[:n], bool(flags & F_OVERFLOW), pending

    def read_clockstamp(self, addr):
        """The snapshot the last poll left behind: (seq, cycles, f_cpu).

        None if the device did not answer or has not initialised timekeeping
        yet. No locking is involved and none is needed - the device only
        writes this from inside a USB command, so it cannot be mid-update
        while we are reading it in a different one.
        """
        raw = self.read(addr, CLK_LEN)
        if raw is None or len(raw) != CLK_LEN:
            return None
        magic, f_cpu, seq, lo, hi = struct.unpack(CLK_FMT, raw)
        if magic != CLK_MAGIC:
            return None
        return seq, (hi << 32) | lo, f_cpu


def parse_console_header(raw):
    """(size, version) from a console header, or None if it is not one."""
    if raw is None or len(raw) < HDR_LEN:
        return None
    magic, size, version = struct.unpack_from("<III", raw)
    if magic != MAGIC:
        return None
    return size, version
