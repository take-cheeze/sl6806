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
| Getting those pixels onto the LCD | **Not yet.** Needs one panel driver — see [docs/LCD.md](docs/LCD.md). |
| `pinMode` / `digitalWrite` / `digitalRead` | **Not yet.** The GPIO registers are unknown. Calls report instead of silently doing nothing. |
| `analogRead` / `analogWrite` | **Not yet.** Same reason. |
| Audio, SD, Bluetooth, FM | **Not yet.** Hardware confirmed present; no drivers. |
| Flashing to run standalone | **Unproven.** See [docs/FLASHING.md](docs/FLASHING.md). |

Two honest caveats worth reading before you trust output:

**The CPU clock is a guess.** `F_CPU` defaults to 120 MHz as a placeholder.
Nothing in the dump establishes the real frequency, so every `delay()` and
`millis()` is off by one constant ratio. It is a single scale factor —
`make SKETCH=examples/ClockCalibrate run`, time it with a stopwatch, then
build with `F_CPU=<measured>` and all timing is correct.

**GPIO needs one table filled in.** The driver is complete; only the register
addresses are missing, and no amount of framework design can invent them. The
recipe for finding them is in
[`cores/sl6806/hal_gpio.h`](cores/sl6806/hal_gpio.h), the helper is
`tools/sl6806-find-mmio`, and the one place to write the answer is
[`variants/p20_player/variant.c`](variants/p20_player/variant.c). Until then a
digital call prints:

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

What is missing is the panel driver: one `sl6806_panel_t` struct giving the
controller, resolution, bus and init sequence. Those facts live in the stock
firmware's `lv_lcd_init` at `0x00D3E34C`, so they need a real dump.
[docs/LCD.md](docs/LCD.md) walks through extracting them. Until then drawing
works and lands in RAM — you can build and test a UI now, and it will appear
the moment the driver exists.

## Testing

The hardware-independent parts of the core are tested natively, under
AddressSanitizer and UBSan:

```sh
make -C tests/host
```

This covers the console ring (wrapping, overflow accounting, framing) and all
drawing primitives (clipping, shapes, text). `tests/host/host_stub.h` is
force-included so core sources compile for the host without a single `#ifdef`
for testing. The graphics test also writes `gfx_demo.ppm` for visual checks.

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

If your sketch never advances, that callback is not periodic on your ROM
revision; build with `RUN_MODE=takeover` to spin in `loop()` instead. That
costs you USB and the monitor.

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
| `sl6806-checkdump` | **Run this on every dump.** Detects the silent-failure dump, verifies both HLKJ CRCs and the partition table. |
| `sl6806-upload` | Loads a payload; refuses to proceed if the device is in card-reader mode. |
| `sl6806-monitor` | Serial monitor; finds the ring buffer by symbol, not by hardcoded address. |
| `sl6806-find-mmio` | Ranks candidate peripheral base addresses in a dump. |
| `sl6806-pack` | Builds a flashable image by patching a known-good dump. |

## Hardware notes

The SL6806 is an ARM **Cortex-M4F** (ARMv7E-M, Thumb-2, single-precision FPU),
SoC codename "spark2".

| Region | Address | Size |
|---|---|---|
| Mask ROM (boot ROM, USB download) | `0x00000000` | `0x7D000` |
| SRAM | `0x00800000` | 256 KiB |
| SPI flash, XIP | `0x00C00000` | 4 MiB |
| Core peripherals (SysTick/NVIC/SCB/DWT) | `0xE000E000` | architectural |
| Everything else (GPIO, clocks, ADC…) | unknown | — |

Flash layout: HLKJ bootloader at `0x0`, partition table at `0xF000`, then
`FIRM` (application, XIP at `0x00C10000`), `PICS` and `FONT`.

The stock application is an LVGL 8.x UI over a scene framework, plus FFmpeg
and a Bluetooth stack. Full analysis is in
[`sl6806_re_notes.md`](sl6806_re_notes.md); the addresses this framework
depends on are annotated with their provenance in
[`cores/sl6806/sl6806.h`](cores/sl6806/sl6806.h) and
[`cores/sl6806/sl6806_rom.h`](cores/sl6806/sl6806_rom.h) — `[V]` verified,
`[A]` architectural, `[I]` inferred, `[?]` unknown.

## Where to help

In rough order of how much they unlock:

1. **GPIO registers** — turns `digitalWrite` real, and with it most of what
   "Arduino" implies. Start with `tools/sl6806-find-mmio`.
2. **The real CPU clock** — makes all timing absolute. A stopwatch does it;
   finding the PLL registers does it exactly.
3. **The FIRM header's mark field and body CRC** — makes standalone firmware
   possible, and yields the SD-update format as a safer install channel.
4. **The panel driver** — everything above it is written and tested, so
   `lv_lcd_init` at `0x00D3E34C` is the highest-value single function left in
   the dump. See [docs/LCD.md](docs/LCD.md).

## Layout

```
cores/sl6806/     the core: Arduino.h, Print/Stream/String, timing, GPIO HAL,
                  USB serial, startup for both modes, boot ROM ABI
cores/sl6806/gfx/ framebuffer, font, panel interface, Display
variants/         board definitions (pin maps go here)
ld/               linker scripts, one per build mode
tools/            host-side Python tools
examples/         Hello, Blink, GfxDemo, ClockCalibrate, MmioProbe
tests/host/       native tests for console + graphics
docs/             DUMPING.md, FLASHING.md, LCD.md
3rd/              smartlink_flash submodule
```

## Credit

The USB protocol, the boot ROM entry points and the payload mechanism are all
from [ilyakurdyukov/smartlink_flash](https://github.com/ilyakurdyukov/smartlink_flash).
Without it none of this would be reachable.
