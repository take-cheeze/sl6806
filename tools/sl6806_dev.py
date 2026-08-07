"""
sl6806_dev - talking to a resident sketch over the stock bootloader tool.

There is no custom host driver anywhere in this project. Every host tool that
talks to a running sketch does it by shelling out to the vendor's own
smtlink_dump with its ordinary `read_mem2` / `write_mem` commands; the sketch
recognises particular sentinel addresses and answers them itself from inside
the boot ROM's USB handler. See cores/sl6806/sl6806_console.h for why that is
the whole protocol.

This module is that plumbing, shared so the monitor and the calibrator cannot
drift apart in how they reach the device.
"""

import atexit
import errno
import os
import shutil
import subprocess
import sys
import tempfile


def find_tool(explicit=None):
    """Locate smtlink_dump: the submodule build first, then $PATH."""
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for c in [os.path.join(here, "..", "3rd", "smartlink_flash", "smtlink_dump"),
              shutil.which("smtlink_dump") or ""]:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.abspath(c)
    sys.exit("error: smtlink_dump not found; pass --tool <path>")


def _why(r):
    """The most useful line smtlink_dump left behind, for a failed call."""
    for stream in (r.stderr, r.stdout):
        lines = [ln.strip() for ln in (stream or "").splitlines() if ln.strip()]
        if lines:
            return lines[-1]
    return "exit status %d, no output" % r.returncode


LOCK_PATH = "/tmp/.sl6806-device.lock"


def take_lock():
    """Refuse to run if another tool already owns the device.

    Two processes talking to this device at once desynchronises its USB
    endpoint, and the device then drops off the bus entirely and needs a
    physical replug. That is a bad enough outcome - and easy enough to cause
    by leaving a monitor running in another window - to be worth a lock file
    rather than a warning in the docs.
    """
    try:
        fd = os.open(LOCK_PATH, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    except OSError as e:
        if e.errno != errno.EEXIST:
            raise
        try:
            with open(LOCK_PATH) as f:
                owner = f.read().strip()
            pid = int(owner.split()[0])
            os.kill(pid, 0)          # still alive?
        except Exception:
            os.unlink(LOCK_PATH)     # stale; take it
            return take_lock()
        sys.exit("error: another SL6806 tool already has the device (%s).\n"
                 "       Two at once wedges it off the bus. Stop that one, or\n"
                 "       remove %s if it is stale." % (owner, LOCK_PATH))

    os.write(fd, ("%d %s" % (os.getpid(), " ".join(sys.argv))).encode())
    os.close(fd)
    atexit.register(lambda: os.path.exists(LOCK_PATH) and os.unlink(LOCK_PATH))


class Device:
    """One SL6806 in bootloader mode, with a sketch resident.

    Every call records why it failed in `last_error`. That matters more here
    than it looks: a payload that misbehaves inside the boot ROM's USB handler
    makes the device refuse commands, and a tool that retries in silence turns
    the single most diagnostic fact - what libusb said - into "it just sits
    there". Callers are expected to surface it.
    """

    def __init__(self, tool, sudo=True, timeout=10.0):
        self.tool = tool
        self.sudo = sudo
        self.timeout = timeout
        self.last_error = None
        self.tmp = tempfile.mkdtemp(prefix="sl6806.")

    def _run(self, args):
        cmd = (["sudo"] if self.sudo else []) + [self.tool, "chip", "6806"] + args
        try:
            return subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=self.timeout)
        except subprocess.TimeoutExpired:
            # A wedged endpoint can leave libusb blocked indefinitely. Without
            # this the whole tool hangs with no output at all.
            self.last_error = ("smtlink_dump did not return within %.0f s - "
                               "the USB endpoint is probably stalled"
                               % self.timeout)
            return None

    def read(self, addr, size):
        """Read device memory, or None on failure (see last_error)."""
        if size <= 0:
            return b""
        out = os.path.join(self.tmp, "r.bin")
        try:
            os.unlink(out)
        except OSError:
            pass
        r = self._run(["read_mem2", hex(addr), hex(size), out])
        if r is None:
            return None
        if r.returncode != 0 or not os.path.isfile(out):
            self.last_error = _why(r)
            return None
        with open(out, "rb") as f:
            data = f.read()
        if len(data) != size:
            self.last_error = ("asked for %d bytes, got %d"
                               % (size, len(data)))
            return None
        self.last_error = None
        return data

    def write(self, addr, data):
        src = os.path.join(self.tmp, "w.bin")
        with open(src, "wb") as f:
            f.write(data)
        r = self._run(["write_mem", hex(addr), "0", hex(len(data)), src])
        if r is None:
            return False
        if r.returncode != 0:
            self.last_error = _why(r)
            return False
        self.last_error = None
        return True
