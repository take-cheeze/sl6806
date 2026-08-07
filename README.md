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
| `millis()` / `micros()` / `delay()` | **Works**, on a measured 64 MHz clock. Backed by SysTick, whose 24-bit counter wraps every 262 ms — see the rule below. |
| Heap, C++ runtime, `String`, `new`/`delete` | **Works.** ~38 KB heap in payload mode, ~190 KB in firmware mode. |
| Flash image format (HLKJ, CRC16, partitions) | **Works.** Both CRCs verify and round-trip. |
| Graphics: framebuffer, shapes, text, `Screen.print()` | **Works.** RGB565, fully clipped, 64 host-side tests. Renders into RAM, and now onto glass. |
| Panel: geometry, vendor init sequence, windowing, sleep/wake | **Works.** Recovered from the firmware, 53 host-side tests. |
| Getting those pixels onto the glass | **Works.** Verified on hardware 2026-08-07: `examples/GfxDemo` draws shapes and text on the panel, in the right colours. The bus driver was written entirely from the bootloader's disassembly and had never been seen to run; the two bits of the transfer register that were inferred rather than read turn out to be right. Driver and 195 host tests: [docs/LCD.md](docs/LCD.md). |
| `shiftOut` / `shiftIn` / `pulseIn` | **Written**, in terms of the digital calls — so as real as GPIO is. |
| `pinMode` / `digitalWrite` / `digitalRead` | **Written.** The pad controller was recovered from the mask ROM. What is missing now is this board's pinout: only the two reset lines have known pad ids. |
| Keys — the two volume buttons | **Works.** They are an ADC resistor ladder, not GPIO; driver in [`cores/sl6806/sl6806_adc.c`](cores/sl6806/sl6806_adc.c), board map in the variant, 97 host tests. Verified on hardware. |
| `analogRead` / `analogWrite` / `tone` / `attachInterrupt` | **Not yet**, though the ADC block itself now works — see `sl6806_adc.h`. `analogRead` needs a pin-to-channel map this board does not have. |
| Backlight | **Works, on/off.** Dimming does not — `sl6806_backlight_begin(100)` lights the panel from cold or warm. The PWM counter does not run, so duty has no effect yet. PWM at `0x40084000`, channel 3, module id 68, pad bank 1 pin 0 function 4, and the pair clock enable at `0x40084014` bit 8 that nothing in the vendor's firmware writes. 13 host tests. |
| Audio, SD, Bluetooth, FM | **Not yet.** Hardware confirmed present; no drivers. |
| Flashing to run standalone | **Unproven.** See [docs/FLASHING.md](docs/FLASHING.md). |

Two honest caveats worth reading before you trust output:

**The CPU clock is measured, not documented: 64 MHz.** `F_CPU` now defaults to
64,000,000 because that is what one P20 Player was measured at — 64,000,071 Hz
with a bracket of ±0.06% containing exactly one whole MHz. The clock and reset
unit at `0x40080000` holds dividers and gates, not a PLL multiplier, and
nothing in the dump establishes the crystal, so this comes from timing the
device against the host rather than from a register.

If you are bringing up a different unit, re-measure rather than assume — it is
a single scale factor, and one measurement fixes every `delay()` and
`millis()` at once:

```sh
make SKETCH=examples/Hello RUN_MODE=poll calibrate
```

The device does not time anything — it reports its own cycle counter when
asked, and the host, which knows real wall-clock time, regresses one against
the other. Rebuild with the `F_CPU` it prints and all timing becomes absolute.
It reports a bracket as well as a number, and refuses to answer rather than
average if the samples contradict each other; see
[`cores/sl6806/sl6806_stat.h`](cores/sl6806/sl6806_stat.h).

⚠ **The DWT cycle counter does not run on this part**, and timekeeping uses
the 24-bit SysTick fallback instead. Two consequences worth knowing:

- **The wrap period is 262 ms**, not the ~67 s a 32-bit counter would give.
  Wraps are only accumulated when the counter is read, so code that runs for a
  quarter of a second without calling `millis()`, `delay()` or `yield()`
  loses time. An ordinary `loop()` is fine; a long computation is not.
- **`sl6806_time_init()` requires a counter to *change***, not merely to read
  nonzero — the original probe accepted a register stuck at a constant, which
  is exactly what the DWT does here. If neither counter advances, `millis()`
  is frozen and `delay()` returns immediately with a one-time explanation
  rather than spinning on a target it can never reach. In `RUN_MODE=poll` that
  spin runs inside the boot ROM's USB handler and takes the device off the bus
  until it is unplugged.

**GPIO's hole is the pinout now, not the registers.** The pad controller was
found in the mask ROM, not in flash — which is why an earlier round of
analysis kept missing it. Nothing in the firmware writes a GPIO register
directly; both the bootloader and the application call thunks into the ROM,
and the ROM holds a six-entry table of bank bases at `0x00065004`:

```
0x40081000  0x40081040  0x400F6080  0x400810C0  0x40081100  0x400F6000
```

The vendor's packed pin id decodes as bank `[19:16]`, pin `[15:11]`, function
`[10:7]` (0 = input, 1 = output, 2+ = alternate), initial level `[6]`, drive
`[5:4]`, pull `[3:0]`. The map and the driver are in
[`cores/sl6806/sl6806_padctl.h`](cores/sl6806/sl6806_padctl.h).

So a pin whose pad id is known works:

```cpp
digitalWrite(PIN_LCD_RESET, HIGH);   // bank 1 pin 7
```

On this board that is still only the two reset lines the stock firmware
drives. Every other name in the variant is a pad nobody has identified, and
using one prints:

```
*** SL6806: GPIO is not configured ***
```

That is deliberate. A GPIO API that quietly does nothing costs you an
afternoon debugging your wiring. The recipe for finding an id is in
[`cores/sl6806/hal_gpio.h`](cores/sl6806/hal_gpio.h), and the place to write
the answer is
[`variants/p20_player/variant.c`](variants/p20_player/variant.c).

### Serial is not a UART

This chip has no known debug UART, and in payload mode the boot ROM owns the
only USB link. So `Serial` is a pair of ring buffers in the sketch's RAM,
serviced by a vendor SCSI command the sketch answers from inside the ROM's USB
loop. `tools/sl6806-monitor` drives it with the stock `smtlink_dump`.

Output is one USB round trip per poll, and the device reports exactly how many
bytes were lost if polling fell behind — those appear as `[lost output]`
rather than a silently mangled stream. Typing in the monitor feeds
`Serial.read()`, so sketches can be interactive.

### The display: written end to end, and now seen

`Screen` is a complete graphics stack — framebuffer, primitives, text, and a
`Print` interface so `Screen.print(x)` works like `Serial.print(x)`. It is
verified natively (`make -C tests/host`), including clipping and the font.

The panel is **240x296 RGB565, drawn at controller offset (0, 12)**, behind a
standard MIPI DCS command set, with a **33-command vendor init sequence**
recovered from the firmware into
[`variants/p20_player/panel.c`](variants/p20_player/panel.c). Regenerate any
of it from a dump with `tools/sl6806-panelseq`.

The layer that used to be missing — the byte-level bus — now exists.
**The panel is a QSPI display.** The stock bootloader sends `02 00 <cmd> 00`
for every DCS command and streams pixels behind a `32` opcode, which is the
standard QSPI display frame, and once that is recognised the LCD controller at
`0x400D9000` is straightforwardly a QSPI master. The bootloader's own driver
for it is in flash and disassembles, so
[`cores/sl6806/sl6806_lcdc.c`](cores/sl6806/sl6806_lcdc.c) is a transcription
of it. The bus comes up by itself:

```cpp
Screen.begin(band, 160, 40);   // brings the controller up
Screen.fill(SL6806_BLUE);
Screen.display();
```

**It has been run against a screen and it draws.** Shapes, text and the right
colours, verified 2026-08-07 with `examples/GfxDemo`. The two bits of the
transfer register that were inferred from where the vendor sets them rather
than read are correct, and so is the pixel byte order.

What had been wrong for the entire history of this driver was **the backlight**,
which is not part of the LCD path and which nothing here turned on. Every
earlier "the controller runs clean and the panel stays dark" result was
measuring an unlit lamp. It is one call now:

```cpp
sl6806_backlight_begin(100);   // cores/sl6806/sl6806_pwm.h
```

It is on/off — the PWM counter does not run, so duty has no effect yet.
[docs/LCD.md](docs/LCD.md) is now a description rather than a list of reasons
the screen might be dark.

### Tests

Three suites, none of which needs a device:

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
is the one callback known to fire.

Two costs. `loop()` only advances while something is polling, so the sketch
stops when you disconnect the monitor. And **`loop()` must not block** — it
runs inside the ROM's USB command handler, and not returning before the host's
~1 s SCSI timeout desynchronises the endpoint and wedges the device until you
unplug it. A plain `delay(1000)` is enough. Poll mode therefore caps how long
`delay()` may block and reports once when it clamps, so an ordinary blocking
sketch stays alive and honest instead of hanging the link — but its timing is
wrong, so pace `loop()` with `millis()` if the timing matters.

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
make SKETCH=examples/Blink calibrate        # measure the real CPU clock
make SKETCH=examples/Blink F_CPU=48000000   # if your unit measures differently
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
| `sl6806-padscan` | Decodes every pad id the stock firmware configures into a bank/pin/function map; `--outputs` lists the pads worth driving when a rail is dark. |
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

1. **Run the display driver on a real P20 and report what happens.** This is
   worth more than any further reading of the dump. The whole stack is
   written and the panel is a QSPI display; what nobody knows is whether the
   two inferred bits of the transfer register and the guessed pixel byte
   order are right. [docs/LCD.md](docs/LCD.md) has the checklist. It cannot
   brick anything in payload mode.
2. **This board's pinout.** The pad controller is done
   ([`sl6806_padctl.h`](cores/sl6806/sl6806_padctl.h)); what is missing is
   which pad each button and LED is on. The ids are immediates at the stock
   firmware's 54 GPIO call sites, so this is a reading exercise plus a meter.
3. **A DMA driver.** The display pushes pixels 16 bytes at a time because
   that is the FIFO depth. The vendor uses the DMA controller at
   `0x40070000`; its command-list format is decoded at the bottom of
   [`sl6806_lcdc.h`](cores/sl6806/sl6806_lcdc.h).
4. ~~**The real CPU clock**~~ — `make calibrate` measures it against the host
   clock, so this no longer blocks anything; finding the PLL registers would
   still give it exactly. They are *not* at the clock unit's base:
   `0x40080000` has dividers but no multiplier.

~~**The LCDC command-list opcodes**~~ — decoded; see §12b of the notes. The
mask ROM dump settled two more in the negative: it holds no LCD driver, and
the vendor SRAM routines are not resident in bootloader mode, so a payload
cannot call them. See
[docs/sl6806_re_notes.md](docs/sl6806_re_notes.md) §7f.

## Layout

```
cores/sl6806/     the core: Arduino.h, Print/Stream/String, timing, GPIO HAL,
                  USB serial, startup for both modes, boot ROM ABI,
                  the pad controller (sl6806_padctl.*) and the LCD
                  controller (sl6806_lcdc.*), clock map (sl6806_cru.h)
cores/sl6806/gfx/ framebuffer, font, panel + LCD bus, Display
variants/         board definitions (pin maps go here)
ld/               linker scripts, one per build mode
tools/            host-side Python tools
examples/         Hello, Blink, GfxDemo, LcdProbe, MmioProbe, RomProbe,
                  CallbackProbe
tests/host/       native tests for console, graphics, the panel and the LCDC
docs/             DUMPING.md, FLASHING.md, LCD.md, sl6806_re_notes.md
3rd/              smartlink_flash submodule
```

## Credit

The USB protocol, the boot ROM entry points and the payload mechanism are all
from [ilyakurdyukov/smartlink_flash](https://github.com/ilyakurdyukov/smartlink_flash).
Without it none of this would be reachable.
