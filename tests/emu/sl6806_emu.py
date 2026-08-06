#!/usr/bin/env python3
"""
sl6806_emu - run an SL6806 payload image under Unicorn.

WHY THIS EXISTS
Everything in this project that can be tested on the host already is: the
console ring, the drawing primitives, the panel command stream. What that
leaves untested is the part where they meet the chip - startup clearing .bss,
C++ constructors running, the heap coming up, timekeeping finding a counter,
setup() and loop() actually executing, and bytes reaching the console ring in
the order a monitor would see them. Those are exactly the failures that show
up as "the device is silent", which is the least informative symptom there is.

A payload build is unusually easy to emulate, because it is a flat blob with a
known entry and it runs on top of a ROM whose ABI is a handful of functions.
So this models just enough machine to run one:

    SRAM        0x00800000 +256K   the whole address space the sketch uses
    mask ROM    0x00000000 +512K   filled with `bx lr`, with the handful of
                                   entry points in sl6806_rom.h implemented
                                   in Python
    peripherals 0x40000000 +16M    reads return 0, writes are recorded
    core        0xE0000000 +1M     DWT/SysTick, with a cycle counter that
                                   actually advances so delay() terminates

WHAT IT IS NOT
Not a chip model. Nothing here knows what any peripheral register means, so a
sketch that drives hardware will run but will not be *checked* - and a sketch
that spins waiting on a status bit will hang until the instruction budget runs
out, which is reported rather than silently passed. The value is in the parts
that are pure software, which on this framework is most of them.

THE CYCLE COUNTER IS COARSE. Each read of DWT->CYCCNT advances it by
`cycles_per_read` (default 100000) rather than by the instructions actually
executed, because emulating cycle-accurate timing would make delay(1000) take
120 million steps. So millis() advances in jumps: use it to check that time
moves forward and that delays terminate, not to measure anything.
"""

import struct
import sys

try:
    from unicorn import *
    from unicorn.arm_const import *
except ImportError:                                          # pragma: no cover
    sys.exit("error: unicorn is not installed (pip install unicorn)")

# ---------------------------------------------------------------- memory map

ROM_BASE, ROM_SIZE = 0x00000000, 0x00080000
SRAM_BASE, SRAM_SIZE = 0x00800000, 0x00040000
PERIPH_BASE, PERIPH_SIZE = 0x40000000, 0x01000000
CORE_BASE, CORE_SIZE = 0xE0000000, 0x00100000

PAYLOAD_LOAD = 0x00820000

# The ROM's initial SP, read out of a real mask ROM dump. A payload runs on
# the ROM's stack, so starting it here is what the hardware does.
ROM_SP = 0x00806000

# Where a called function "returns to". Outside every mapped region, so the
# run stops there and nothing can execute past it by accident.
MAGIC_RETURN = 0x00FFFFF0

DWT_CTRL = 0xE0001000
DWT_CYCCNT = 0xE0001004

CONSOLE_MAGIC = 0x36384C53          # "SL68"
CLOCKSTAMP_MAGIC = 0x4B4C4353       # "SCLK" - the clock-calibration stamp


class EmuError(RuntimeError):
    pass


class SL6806:
    def __init__(self, image, load=PAYLOAD_LOAD, cycles_per_read=100000,
                 trace_mmio=False):
        # MCLASS is required: without it the core has no PRIMASK, and the
        # first `mrs r1, PRIMASK` in sl6806_irq_save() is an invalid
        # instruction.
        self.uc = Uc(UC_ARCH_ARM,
                     UC_MODE_THUMB | UC_MODE_MCLASS | UC_MODE_LITTLE_ENDIAN)
        self.load = load
        self.cycles_per_read = cycles_per_read
        self.trace_mmio = trace_mmio

        self.cycles = 0
        self.dwt_ctrl = 0
        self.mmio_writes = []       # (pc, addr, size, value)
        self.mmio_reads = []        # (addr, size)
        self.printf_calls = 0
        self.userfn = None          # address of the registered sl6806_usb_userfn_t
        self.rom_calls = {}

        uc = self.uc
        uc.mem_map(ROM_BASE, ROM_SIZE)
        uc.mem_map(SRAM_BASE, SRAM_SIZE)

        # Peripherals are mapped as MMIO, not as RAM.
        #
        # The obvious alternative - map them as RAM and fix up the cycle
        # counter from a UC_HOOK_MEM_READ - does not work, and fails in a way
        # worth recording: mem_write() from inside a hook invalidates the
        # translation cache while the current block is executing, and the
        # instruction *after* the load gets skipped. It presents as
        # timekeeping that advances 625x too slowly, because the skipped
        # instruction happened to be the one loading tick_mask, so every
        # elapsed-cycle delta got masked with an address instead. An MMIO read
        # callback returns the value directly and touches no memory.
        uc.mmio_map(CORE_BASE, CORE_SIZE, self._core_read, None,
                    self._core_write, None)
        uc.mmio_map(PERIPH_BASE, PERIPH_SIZE, self._periph_read, None,
                    self._periph_write, None)

        # An unstubbed ROM call returns immediately rather than executing
        # whatever zeros happen to be there.
        uc.mem_write(ROM_BASE, b"\x70\x47" * (ROM_SIZE // 2))

        if len(image) > SRAM_BASE + SRAM_SIZE - load:
            raise EmuError("image does not fit in SRAM")
        uc.mem_write(load, image)

        self._install_stubs()
        uc.hook_add(UC_HOOK_CODE, self._on_code, begin=ROM_BASE,
                    end=ROM_BASE + ROM_SIZE - 1)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)

    # ------------------------------------------------------------ ROM stubs

    def _install_stubs(self):
        """Addresses from cores/sl6806/sl6806_rom.h."""
        self.stubs = {
            0x0640: self._rom_printf,
            0x0B82: self._rom_strcpy,
            0x0BF2: self._rom_strlen,
            0x0C8E: self._rom_memcpy,
            0x0D0E: self._rom_memset,
            0x0D82: self._rom_memmove,
            0x0DB8: self._rom_memcmp,
            0x5B70: self._rom_set_userfn,
        }

    def _on_code(self, uc, address, size, _user):
        fn = self.stubs.get(address)
        if fn is None:
            return
        self.rom_calls[address] = self.rom_calls.get(address, 0) + 1
        fn(uc)
        # Return by jumping to LR *with its Thumb bit intact*. Masking it off
        # drops the core into ARM state and the next instruction decodes as
        # garbage - which presents as "invalid instruction" one instruction
        # after a perfectly good ROM call.
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def _args(self, uc):
        return (uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1),
                uc.reg_read(UC_ARM_REG_R2))

    def _rom_memcpy(self, uc):
        d, s, n = self._args(uc)
        if n:
            uc.mem_write(d, bytes(uc.mem_read(s, n)))
        uc.reg_write(UC_ARM_REG_R0, d)

    def _rom_memmove(self, uc):
        self._rom_memcpy(uc)

    def _rom_memset(self, uc):
        d, c, n = self._args(uc)
        if n:
            uc.mem_write(d, bytes([c & 0xFF]) * n)
        uc.reg_write(UC_ARM_REG_R0, d)

    def _rom_memcmp(self, uc):
        a, b, n = self._args(uc)
        x, y = bytes(uc.mem_read(a, n)), bytes(uc.mem_read(b, n))
        uc.reg_write(UC_ARM_REG_R0, 0 if x == y else (1 if x > y else 0xFFFFFFFF))

    def _rom_strlen(self, uc):
        uc.reg_write(UC_ARM_REG_R0, len(self.read_cstr(uc.reg_read(UC_ARM_REG_R0))))

    def _rom_strcpy(self, uc):
        d, s, _ = self._args(uc)
        uc.mem_write(d, self.read_cstr(s).encode("latin-1") + b"\0")
        uc.reg_write(UC_ARM_REG_R0, d)

    def _rom_printf(self, uc):
        self.printf_calls += 1

    def _rom_set_userfn(self, uc):
        self.userfn = uc.reg_read(UC_ARM_REG_R0)

    # -------------------------------------------------------------- hooks

    def _core_read(self, _uc, offset, size, _user):
        """Cortex-M core peripherals. Only the cycle counter has behaviour."""
        addr = CORE_BASE + offset
        if addr == DWT_CYCCNT:
            self.cycles = (self.cycles + self.cycles_per_read) & 0xFFFFFFFF
            return self.cycles
        if addr == DWT_CTRL:
            return self.dwt_ctrl
        return 0

    def _core_write(self, _uc, offset, size, value, _user):
        addr = CORE_BASE + offset
        if addr == DWT_CTRL:
            self.dwt_ctrl = value
        elif addr == DWT_CYCCNT:
            # A write of 0 is how sl6806_time_init() probes for the DWT: it
            # zeroes the counter and checks that it ticks. Honour the write so
            # the probe is measuring something real.
            self.cycles = value & 0xFFFFFFFF

    def _periph_read(self, _uc, offset, size, _user):
        self.mmio_reads.append((PERIPH_BASE + offset, size))
        return 0

    def _periph_write(self, uc, offset, size, value, _user):
        address = PERIPH_BASE + offset
        self.mmio_writes.append((uc.reg_read(UC_ARM_REG_PC), address, size, value))
        if self.trace_mmio:
            print("  MMIO w%d 0x%08X = 0x%X" % (size * 8, address, value),
                  file=sys.stderr)

    def _on_unmapped(self, uc, _access, address, _size, _value, _user):
        raise EmuError("access to unmapped memory 0x%08X from pc 0x%08X"
                       % (address, uc.reg_read(UC_ARM_REG_PC)))

    # ------------------------------------------------------------- running

    def read_cstr(self, addr, limit=256):
        out = bytearray()
        for i in range(limit):
            b = self.uc.mem_read(addr + i, 1)[0]
            if b == 0:
                break
            out.append(b)
        return out.decode("latin-1")

    def call(self, addr, args=(), max_insns=20_000_000):
        """Call a Thumb function and return r0. Raises on fault or budget."""
        uc = self.uc
        for i, v in enumerate(args[:4]):
            uc.reg_write(UC_ARM_REG_R0 + i, v)
        uc.reg_write(UC_ARM_REG_SP, ROM_SP)
        uc.reg_write(UC_ARM_REG_LR, MAGIC_RETURN | 1)
        try:
            uc.emu_start(addr | 1, MAGIC_RETURN, count=max_insns)
        except UcError as e:
            raise EmuError("fault at pc 0x%08X: %s"
                           % (uc.reg_read(UC_ARM_REG_PC), e))
        pc = uc.reg_read(UC_ARM_REG_PC)
        if pc != MAGIC_RETURN:
            raise EmuError(
                "ran out of instruction budget at pc 0x%08X - the sketch is "
                "probably spinning on a peripheral this emulator does not "
                "model" % pc)
        return uc.reg_read(UC_ARM_REG_R0)

    def start(self, **kw):
        """Run _start: .bss clear, constructors, setup(), hook registration."""
        return self.call(self.load, **kw)

    def idle_cb(self):
        """The cb2 the payload registered with the ROM, or None."""
        if self.userfn is None:
            return None
        return struct.unpack("<I", self.uc.mem_read(self.userfn + 8, 4))[0]

    def run_loop(self, iterations=1, **kw):
        """Drive loop() the way the ROM's idle callback would."""
        cb = self.idle_cb()
        if not cb:
            raise EmuError("no idle callback registered - is this a "
                           "RUN_MODE=takeover build?")
        for _ in range(iterations):
            self.call(cb & ~1, **kw)

    # ------------------------------------------------------------- console

    def find_console(self):
        """Locate _sl6806_console by its magic, the way a monitor would."""
        want = struct.pack("<I", CONSOLE_MAGIC)
        blob = bytes(self.uc.mem_read(self.load, SRAM_BASE + SRAM_SIZE - self.load))
        off = blob.find(want)
        while off != -1:
            addr = self.load + off
            size, ver = struct.unpack_from("<II", blob, off + 4)
            if ver in (1, 2) and 0 < size <= 0x10000 and (size & (size - 1)) == 0:
                return addr
            off = blob.find(want, off + 4)
        return None

    def clockstamp(self, addr):
        """The clock-calibration side-channel: (seq, cycles, f_cpu) or None.

        Read at a symbol address, exactly as tools/sl6806-clockcal does with
        the .sym file - not searched for by magic. The magic is a literal in
        the code that writes it, so a scan finds the literal pool first; the
        host tool cannot make that mistake and neither should this.

        None means the magic is absent, which means sl6806_time_init() has not
        run yet.
        """
        _m, f_cpu, seq, lo, hi = struct.unpack("<5I", self.uc.mem_read(addr, 20))
        if _m != CLOCKSTAMP_MAGIC:
            return None
        return seq, (hi << 32) | lo, f_cpu

    def console(self):
        """Everything the sketch has printed, in order, as the monitor sees it."""
        addr = self.find_console()
        if addr is None:
            raise EmuError("console ring not found - did _start run?")
        magic, size, _ver, head, tail, dropped = struct.unpack(
            "<6I", self.uc.mem_read(addr, 24))
        if magic != CONSOLE_MAGIC:
            raise EmuError("console magic moved")
        tx = bytes(self.uc.mem_read(addr + 32, size))
        n = min(head - tail, size)
        out = bytearray()
        for i in range(n):
            out.append(tx[(tail + i) % size])
        return out.decode("latin-1", "replace"), dropped


def load_image(path):
    with open(path, "rb") as f:
        return f.read()
