# Arduino-like framework for the SL6806

An Arduino-style core for the Zhuhai Smartlink **SL6806** — the Cortex-M4F SoC
in cheap "Jointbees"/YP3 MP3 players. Write `setup()` and `loop()`, build with
`make`, load over USB.

```cpp
#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    Serial.println("hello from an MP3 player");
}

void loop() {
    Serial.println(millis());
    delay(1000);
}
```

```sh
make SKETCH=examples/Hello upload
make SKETCH=examples/Hello monitor
```

## What actually works

This chip has no datasheet. Everything here was built from a firmware dump and
from [ilyakurdyukov/smartlink_flash](https://github.com/ilyakurdyukov/smartlink_flash),
so it is worth being precise about which parts are real:

| Area | Status |
|---|---|
| Build → link → load → run | **Works.** Verified end to end; entry lands at 0x820000 with the correct Thumb entry. |
| `setup()` / `loop()` | **Works.** Driven from the boot ROM's idle callback so USB stays alive. |
| `Serial` — bidirectional USB serial | **Works.** Print, printf, String, Stream, and `Serial.read()` from an interactive monitor. Not a UART: see below. |
| `millis()` / `micros()` / `delay()` | **Works**, but scaled by an unverified clock — see below. |
| Heap, C++ runtime, `String`, `new`/`delete` | **Works.** ~38 KB heap in payload mode, ~190 KB in firmware mode. |
| Flash image format (HLKJ, CRC16, partitions) | **Works.** Both CRCs verify and round-trip. |
| Graphics: framebuffer, shapes, text, `Screen.print()` | **Works.** RGB565, fully clipped, 64 host-side tests. Renders into RAM. |
| Panel: geometry, vendor init sequence, windowing, sleep/wake | **Works.** Recovered from the firmware, 53 host-side tests. |
| Getting those pixels onto the glass | **Not yet.** Needs a 3-function bus — see [docs/LCD.md](docs/LCD.md). |
| `shiftOut` / `shiftIn` / `pulseIn` | **Written**, in terms of the digital calls — so as real as GPIO is. |
| `pinMode` / `digitalWrite` / `digitalRead` | **Not yet.** The GPIO registers are unknown, but two pins have working vendor ids — see below. |
| `analogRead` / `analogWrite` / `tone` / `attachInterrupt` | **Not yet.** Registers unknown. Calls report instead of silently doing nothing. |
| Audio, SD, Bluetooth, FM | **Not yet.** Hardware confirmed present; no drivers. |
| Flashing to run standalone | **Unproven.** See [docs/FLASHING.md](docs/FLASHING.md). |

Two honest caveats worth reading before you trust output:

**The CPU clock is a guess.** `F_CPU` defaults to 120 MHz as a placeholder.
The clock and reset unit has since been found at `0x40080000`, but it holds
dividers and gates, not a PLL multiplier, and nothing in the dump establishes
the crystal frequency. So every `delay()` and `millis()` is still off by one
constant ratio. It is a single scale factor —
`make SKETCH=examples/ClockCalibrate run`, time it with a stopwatch, then
build with `F_CPU=<measured>` and all timing is correct.

**GPIO has one hole, with two ways to fill it.** The driver is complete;
what is missing is either the register addresses or a way to reach the
vendor's own GPIO routine.

The stock firmware never writes a GPIO register — it calls
`gpio_write(id, value)` at `0x00811C7C` with a packed pin id, from code that
lives in the mask ROM's driver set. Those ids *are* recovered, including the
panel reset pin, so a build that can reach that routine gets working
`digitalWrite()` with no register map at all:

```cpp
sl6806_gpio_vendor_register(&my_backend);   // one function: write(id, value)
digitalWrite(PIN_LCD_RESET, HIGH);          // works
```

The other route is the register table. The recipe is in
[`cores/sl6806/hal_gpio.h`](cores/sl6806/hal_gpio.h), the helper is
`tools/sl6806-find-mmio`, and the one place to write the answer is
[`variants/p20_player/variant.c`](variants/p20_player/variant.c). Until one of
the two exists, a digital call prints:

```
*** SL6806: GPIO is not configured ***
```

That is deliberate. A GPIO API that quietly does nothing costs you an
afternoon debugging your wiring.

### Serial is not a UART

This chip has no known debug UART, and in payload mode the boot ROM owns the
only USB link. So `Serial` is a pair of ring buffers in the sketch's RAM,
serviced by a vendor SCSI command the sketch answers from inside the ROM's USB
loop. `tools/sl6806-monitor` drives it with the stock `smtlink_dump`.

Output is one USB round trip per poll, and the device reports exactly how many
bytes were lost if polling fell behind — those appear as `[lost output]`
rather than a silently mangled stream. Typing in the monitor feeds
`Serial.read()`, so sketches can be interactive.

### The display draws, but nothing shows yet

`Screen` is a complete graphics stack — framebuffer, primitives, text, and a
`Print` interface so `Screen.print(x)` works like `Serial.print(x)`. It is
verified natively (`make -C tests/host`), including clipping and the font.

The panel is no longer a mystery either. It is **240x296 RGB565, drawn at
controller offset (0, 12)**, behind a standard MIPI DCS command set, and its
**33-command vendor init sequence** was recovered from the firmware and lives
in [`variants/p20_player/panel.c`](variants/p20_player/panel.c). Regenerate
any of it from a dump with `tools/sl6806-panelseq`.

What is missing is one layer below all that: the byte-level bus. The stock
firmware puts bytes on the wire with two routines in the mask ROM's driver
set, so they are not in the flash image. Supply three functions —

```c
sl6806_lcd_bus_register(&my_bus);   // command(), pixels(), reset()
Screen.begin();
```

— and the init sequence, windowing, framebuffer and text all start working
unchanged. [docs/LCD.md](docs/LCD.md) describes the three ways to write that
bus. The LCD controller itself has since been located at `0x400D9000`, with
its driver readable in the bootloader; see
[`cores/sl6806/sl6806_lcdc.h`](cores/sl6806/sl6806_lcdc.h).

## Testing

Two suites, neither of which needs a device:

```sh
make test              # all three
make -C tests/host     # pure logic, under ASan/UBSan
make -C tests/tools    # the Python tools' parsers
make -C tests/emu      # real ARM images, under an emulator
```

**Host tests** cover the parts that are algorithms: the console ring
(wrapping, overflow accounting, framing), every drawing primitive (clipping,
shapes, text), and the panel command stream and window arithmetic.
`tests/host/host_stub.h` is force-included so core sources compile for the
host without a single `#ifdef` for testing. The graphics test writes
`gfx_demo.ppm` for visual checks.

**Emulator smoke tests** build real payload images with this Makefile and run
them under [Unicorn](https://www.unicorn-engine.org/), then check what they
printed. That covers the seam the host tests cannot reach — `.bss` cleared,
C++ constructors run, the heap up, timekeeping finding a counter, `setup()`
and `loop()` executing, bytes reaching the console ring in the right order.
Every failure they catch shows up on hardware as "the device printed
nothing", which is the symptom that tells you least.

They need `pip install unicorn` and the ARM toolchain. What they emulate is
memory, not peripherals: passing says the software starts, and says nothing
about whether any peripheral register in the framework is right — there is no
peripheral behind them. See `tests/emu/sl6806_emu.py`.

**Tool tests** cover the parts of the host tools that interpret what the
hardware says back — mode detection from a SCSI inquiry, for instance. Those
are pure functions, and `--help` in CI cannot check them.

CI runs all three on every push and pull request, plus a build of all six
sketches in both modes with `-Werror`.

## Getting started

```sh
sudo apt install gcc-arm-none-eabi        # toolchain
git submodule update --init               # smtlink_dump
make -C 3rd/smartlink_flash               # build the USB tool
```

Put the device in **bootloader mode**: power off, hold a button (which one
varies per unit — trial and error), plug in USB while holding it. Then:

```sh
make SKETCH=examples/Hello upload
make SKETCH=examples/Hello monitor
```

If `Hello` prints, the whole chain works: the payload loaded, the ROM called
it, `.bss` was cleared, constructors ran, the heap works, and `loop()` is
being driven.

## Two build modes

**`MODE=payload`** (default) — a 64 KiB RAM image at `0x00820000`, loaded and
run by the boot ROM over USB. Flash is never written, so **this mode cannot
brick the device.** Develop here. `loop()` runs from the ROM's idle callback,
which keeps USB — and therefore `Serial` — alive.

**If your sketch prints `setup()`'s output and then never ticks**, that idle
callback is not periodic on your ROM revision — which has now been measured on
a real unit, so expect it rather than being surprised. Build with
`RUN_MODE=poll`: `loop()` is driven from the vendor SCSI handler instead, which
is the one callback known to fire. The cost is that `loop()` only advances
while something is polling, and a long `delay()` in `loop()` stalls the USB
transaction it runs inside.

`examples/CallbackProbe` reports which of the ROM's callback slots your unit
actually calls, if you want to know rather than assume.

`RUN_MODE=takeover` spins in `loop()` and never returns to the ROM. It is not
the answer to a sketch that will not tick: it costs you USB and the monitor,
so you get a running `loop()` you cannot observe.

**`MODE=firmware`** — an image linked to SRAM at `0x00804C00` with its own
vector table, intended to replace the vendor application. Read
[docs/FLASHING.md](docs/FLASHING.md) first: the FIRM header is not fully
decoded, so this is unproven, and it is the only thing here that can leave you
with a non-booting device.

## Make targets

```sh
make SKETCH=examples/Blink                  # build (payload)
make SKETCH=examples/Blink upload           # load and run
make SKETCH=examples/Blink monitor          # watch Serial
make SKETCH=examples/Blink run              # upload, then monitor
make SKETCH=examples/Blink F_CPU=48000000   # after calibrating
make SKETCH=examples/Blink RUN_MODE=takeover
make SKETCH=examples/Blink MODE=firmware
make clean
```

Sketches are `.ino` (compiled as C++ with `Arduino.h` pre-included), `.cpp` or
`.c` in a directory named after the sketch. Unlike the Arduino IDE, forward
declarations are not synthesised — define helpers before you call them.

## Tools

| Tool | Purpose |
|---|---|
| `sl6806-checkdump` | **Run this on every dump.** Detects the silent-failure dump, verifies both HLKJ CRCs and the partition table. `--rom` / `--sram` validate memory dumps, which fail as plausible noise rather than as a repeated block. |
| `sl6806-upload` | Loads a payload; refuses to proceed if the device is in card-reader mode. |
| `sl6806-monitor` | Serial monitor; finds the ring buffer by symbol, not by hardcoded address. |
| `sl6806-find-mmio` | Ranks candidate peripheral base addresses in a dump. |
| `sl6806-pack` | Builds a flashable image by patching a known-good dump. |
| `sl6806-panelseq` | Recovers the panel descriptor and its DCS command sequences from a dump; `--c` emits the variant tables. |
| `sl6806-ramcalls` | Lists the SRAM and mask-ROM routines the stock firmware calls, ranked, with the constants passed at each call site. |
| `sl6806-dumpram` | Reads device memory over USB — the mask ROM is the one worth reading. |

## Hardware notes

The SL6806 is an ARM **Cortex-M4F** (ARMv7E-M, Thumb-2, single-precision FPU),
SoC codename "spark2".

| Region | Address | Size |
|---|---|---|
| Mask ROM (boot ROM, USB download, **shared driver library**) | `0x00000000` | `0x7D000` |
| SRAM | `0x00800000` | 256 KiB |
| SPI flash, XIP | `0x00C00000` | 4 MiB |
| Core peripherals (SysTick/NVIC/SCB/DWT) | `0xE000E000` | architectural |
| Peripherals | `0x40000000` | — |

Peripheral blocks identified so far:

| Base | Block |
|---|---|
| `0x40000000` | pad / pin function mux |
| `0x40009000` | timers |
| `0x40070000` | DMA |
| `0x40080000` | clock & reset ([`sl6806_cru.h`](cores/sl6806/sl6806_cru.h)) |
| `0x400D9000` | LCD controller ([`sl6806_lcdc.h`](cores/sl6806/sl6806_lcdc.h)) |
| `0x400F7000` | SD/MMC + SPI flash host |

Flash layout: HLKJ bootloader at `0x0`, partition table at `0xF000`, then
`FIRM` (application, XIP at `0x00C10000`), `PICS` and `FONT`.

The stock application is an LVGL 8.x UI over a scene framework, plus FFmpeg
and a Bluetooth stack. Full analysis is in
[`docs/sl6806_re_notes.md`](docs/sl6806_re_notes.md); the addresses this
framework depends on are annotated with their provenance in
[`cores/sl6806/sl6806.h`](cores/sl6806/sl6806.h) and
[`cores/sl6806/sl6806_rom.h`](cores/sl6806/sl6806_rom.h) — `[V]` verified,
`[A]` architectural, `[I]` inferred, `[?]` unknown.

## Where to help

In rough order of how much they unlock:

1. **The LCDC command-list opcodes.** The controller is at `0x400D9000`, the
   register map is written down, and the descriptor handoff is understood.
   What is left is the meaning of `0xABAB0005` and `0xCDCDxx03`. Everything
   above that layer is already written and tested, so this is now the single
   thing between the framework and a picture.
2. **GPIO registers** — the only route to `digitalWrite`. Start with
   `tools/sl6806-find-mmio`. Note the mask ROM has been dumped and does *not*
   contain them.
3. **The real CPU clock** — makes all timing absolute. A stopwatch does it;
   finding the PLL registers does it exactly. It is *not* at the clock unit's
   base: `0x40080000` has dividers but no multiplier.

The mask ROM has since been dumped, which settled two questions in the
negative: it holds no LCD driver, and the vendor SRAM routines are not
resident in bootloader mode, so a payload cannot call them. See
[docs/sl6806_re_notes.md](docs/sl6806_re_notes.md) §7f.

## Layout

```
cores/sl6806/     the core: Arduino.h, Print/Stream/String, timing, GPIO HAL,
                  USB serial, startup for both modes, boot ROM ABI,
                  peripheral maps (sl6806_cru.h, sl6806_lcdc.h)
cores/sl6806/gfx/ framebuffer, font, panel + LCD bus, Display
variants/         board definitions (pin maps go here)
ld/               linker scripts, one per build mode
tools/            host-side Python tools
examples/         Hello, Blink, GfxDemo, ClockCalibrate, MmioProbe, RomProbe
tests/host/       native tests for console, graphics and the panel
docs/             DUMPING.md, FLASHING.md, LCD.md, sl6806_re_notes.md
3rd/              smartlink_flash submodule
```

## Credit

The USB protocol, the boot ROM entry points and the payload mechanism are all
from [ilyakurdyukov/smartlink_flash](https://github.com/ilyakurdyukov/smartlink_flash).
Without it none of this would be reachable.
