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

Bringing a real device up for the first time: **[docs/BRINGUP.md](docs/BRINGUP.md)**.

## What actually works

This chip has no datasheet — searched again 2026-08-13, and there is nothing
public in English or Chinese; the vendor is very probably Zhuhai's 绅聚科技
("Jointbees"), whose 云P3 chip matches the firmware's `yp3_` strings, and it
publishes no documentation ([notes §1a](docs/sl6806_re_notes.md)). Everything here
was built from a firmware dump and from
[ilyakurdyukov/smartlink_flash](https://github.com/ilyakurdyukov/smartlink_flash),
so it is worth being precise about which parts are real:

| Area | Status |
|---|---|
| Build → link → load → run | **Works.** Verified end to end; entry lands at 0x820000 with the correct Thumb entry. |
| `setup()` / `loop()` | **Works.** Driven from the boot ROM's idle callback so USB stays alive. |
| `Serial` — bidirectional USB serial | **Works.** Print, printf, String, Stream, and `Serial.read()` from an interactive monitor. Not a UART: see below. |
| `millis()` / `micros()` / `delay()` | **Works**, on a measured 64 MHz clock. Backed by SysTick, whose 24-bit counter wraps every 262 ms — see the rule below. |
| Heap, C++ runtime, `String`, `new`/`delete` | **Works.** ~650 KB heap in payload mode — SRAM is 1 MiB and only the *loaded* image has to fit in the 64 KB window. `examples/BigBuffer` pattern-tests the region and draws a full-screen framebuffer. |
| Flash image format (HLKJ, CRC16, partitions) | **Works.** Both CRCs verify and round-trip. |
| Graphics: framebuffer, shapes, text, `Screen.print()` | **Works.** RGB565, fully clipped, 64 host-side tests. Renders into RAM, and now onto glass. |
| Panel: geometry, vendor init sequence, windowing, sleep/wake | **Works.** Recovered from the firmware, 53 host-side tests. |
| Getting those pixels onto the glass | **Works.** Verified on hardware 2026-08-07: `examples/GfxDemo` draws shapes and text on the panel, in the right colours. The bus driver was written entirely from the bootloader's disassembly and had never been seen to run; the two bits of the transfer register that were inferred rather than read turn out to be right. Driver and 198 host tests: [docs/LCD.md](docs/LCD.md). |
| How fast those pixels arrive | **5 fps, measured.** A full 240x296 frame takes 167 ms — 829 KiB/s. It is not the panel's clock: the controller is always finished before the CPU returns, and the time splits about evenly between register-access latency (312 ns each, 26 per 16-byte transfer) and staging pixels a byte at a time. `examples/PushRate` measures it; [docs/LCD.md](docs/LCD.md) says what to do about it. |
| `shiftOut` / `shiftIn` / `pulseIn` | **Written**, in terms of the digital calls — so as real as GPIO is. |
| `pinMode` / `digitalWrite` / `digitalRead` | **Written.** The pad controller was recovered from the mask ROM. What is missing now is this board's pinout: only the two reset lines have known pad ids. |
| Keys — the two volume buttons | **Works.** They are an ADC resistor ladder, not GPIO; driver in [`cores/sl6806/sl6806_adc.c`](cores/sl6806/sl6806_adc.c), board map in the variant, 97 host tests. Verified on hardware. |
| `analogRead` / `analogWrite` / `tone` / `attachInterrupt` | **Not yet**, though the ADC block itself now works — see `sl6806_adc.h`. `analogRead` needs a pin-to-channel map this board does not have. |
| Backlight | **Works, on/off. Dimming still does not, and the counter is now measured as stopped rather than assumed.** The static find is real: `0x00D99C34` — the channel init §14a took the register map from — opens with *three* clock steps, and the two nobody had performed are `clk_setsrc(39, …)` and `romclk_enable(39)`, both landing on **`0x400800F4`**. §18's "the PWM does not ask for a clock of its own" was a scan of the SRAM blob and stage 1 while the call sites are in XIP. `sl6806_backlight_begin()` now performs them. **But the counter still does not run.** `examples/PwmClock` walked five combinations of the clock, its source-select bit and the pair register's bit 8, with a witness that observes an effect — retiring CTRL bit 8 needs a period boundary — and it reports **0/4 in every configuration**, including the two where CTRL bit 28 goes clear. So bit 28 is not a run flag; it tracks romclk 39 and means nearer "the block has a clock". The panel tracks **pair bit 8** and nothing else: set, it lights; clear, it is dark even where bit 28 says "running". That write is what makes this board have a backlight at all, nothing in the vendor's image performs it, and it was briefly removed here on the strength of bit 28 — a regression, now reverted and guarded by a named host test. Bit 28 is not even a deterministic function of the clock register — configuration D produced it both ways with an identical `0x400800F4`, so it is a settling flag with a race in it and is disqualified as evidence on its own. Two leads remain, both fields the vendor writes and this driver does not: **MODE `[3:1]`** — which `0x00811E9A` clears the enable before changing, the shape of a field that selects what the counter *does* — and **CTRL `[19:16]`**. `examples/PwmMode` sweeps both against the commit witness, 168 rows. PWM at `0x40084000`, channel 3, module 68 + romclk 39, pad bank 1 pin 0 function 4. 27 host tests. [§32](docs/sl6806_re_notes.md) |
| Audio out | **Measured 2026-08-15: the block counts a length register down and never touches memory.** The controller is `0x40009000`, and it accepts a descriptor, retires it in a time linear in length (~536 B/µs, over a 1440:1 range) and raises its completion flag — but a CPU loop reading SRAM *inside* the transfer window is completely unaffected, `+0%` across four interleaved runs, where 4796 bytes in 9 µs would be ~533 MB/s of bus traffic. That test depends on no register's meaning, which is what undid several earlier attempts. Exhausted with working, fail-able controls: all 128 module ids and all 56 romclk ids (cumulative), the four routes and three source modes, all 32 bits of `0x400E0000`, the bit clock divider and its source bit, the module-2 cycle with the vendor's delays, the audio PLL and the vendor's wider clock chain, START ordering, a 16-byte primer, and TX_CTRL's three unwritten bits. Caveat: a read-back census cannot see a write-only bit — `TX_TRIG` bit 0, the arm strobe the path depends on, reads as unimplemented — so a fetch enable may exist that no census can find. Next move is not another sweep: boot the stock firmware with `examples/FirmBoot`, dump the block while it really plays, and diff. Driver in [`cores/sl6806/sl6806_audio.c`](cores/sl6806/sl6806_audio.c), 93 host tests; [docs/AUDIO.md](docs/AUDIO.md). |
| SD / TF card | **Host found, driver written, and run on hardware 2026-08-13: configured and not running.** The SD/MMC host is `0x40003000` — not `0x400F7000`, which is the SPI flash host and, at `+0x100`, the register-file mailbox (§7m); that row of the peripheral map is now corrected. It is written down once per image, into a driver handle, which is why a literal scan never ranked it. **The whole SD stack is in the mask ROM**, the same way GPIO is: command layer, CMD0/CMD8/ACMD41 identification, the CSD parse, and a *polled* FIFO drain at `+0x200` — so this needs neither DMA nor interrupts. [`cores/sl6806/sl6806_sd.c`](cores/sl6806/sl6806_sd.c) transcribes it with every poll bounded, brings the block up the ROM's way (pads bank 1 pins 12–14 on function 2, module id 36 for the registers, ROM clock id 17 for the function), identifies the card and reads 512-byte blocks over a one-bit bus. 72 host tests against a model of the controller. **On hardware the decode is confirmed and the block does not run.** The cold dump — before this framework touched anything — reads `CTRL 0x40000000`, `CLKCR 0x20070008`, `DTIMER 0xFFFFFF40` and `CMD 0x0000A400`, which are exactly the four values the transcription predicts, the last of them being the CMD0 command word with its start bit cleared. (Whether the *mask ROM* left those values or an earlier payload in the same power session did is not settled — uploading a payload does not reset the chip; [docs/SD.md](docs/SD.md) has the one-power-cycle test that decides it.) That looked like the PWM's failure and the camera's a third time — and `examples/SdWall` **ruled it out**: `0x400E0000` implements bits [6:0] and nothing above them, all seven were tried, and none is the SD host's. (That also shrinks §14a's and §16's open item from 28 bits to three — 2, 3 and 4 — since every walk of that register so far has been walking 25 bits that do not exist.) The bootloader's four differences from the ROM's bring-up fail too. `examples/SdClock` then swept all 64 settings of the SD clock register `0x40080080` (found already enabled, on its default source, undivided) and twelve CLKCR configurations: **the clock is not it either.** `examples/SdCtrl` then swept every writable CTRL bit before and after the reset, and reported the thing that reframes the problem: **`start LATCHED` on every line.** The command register takes the start bit and has dropped it again by the time the poll gives up — so the block is not gated, not stalled on a clock, and not refusing the command; it accepts it, drops busy, and the status register reports nothing. (It also mapped CTRL: writable bits are 1, 3, 4, 24, 25, and **bit 0 is not writable at all**, which retired the hypothesis that sketch was built on.) Three hypotheses are now closed by measurement — `0x400E0000`, the clock, and CTRL — so `examples/SdScope` mapped instead of guessing: 72 registers snapshotted around one CMD0, and **exactly one moved, the one that was written**, with the busy bit gone within ~300 ns where shifting 48 bits would take tens of microseconds. `examples/SdLife` then produced the first positive result: **sixteen words pushed into the FIFO at `+0x200` moved the flags in `+0x48`** — empty cleared, full set at exactly 16, a level field appeared. The FIFO is 16 words deep and **the datapath is clocked**, so the core is not stopped and the command state machine alone is at fault. `examples/SdPads` then tried to see whether the card clock toggles on a pin, and **that test could not have worked**: the pad input buffer is off in every function except 0 and 14, which `examples/PadScope` measured during the LCD work and `docs/LCD.md` records. So **software is blind to the SD bus for the same reason it is blind to the LCD bus** — whether the clock leaves the chip needs a probe on the socket. What the run did establish is that pins 12/13/14 pull high as plain inputs, so the pads and their pull-ups are real. `examples/PadMap` covers what is left that software *can* see: a no-write census of every pad's function and level across all six banks — this board's pinout, never written down — and a pull-up/pull-down test on pads already in function 0, which finds the card-detect contact (a switch to ground) by diffing a run with a card against one without. Also outstanding: the power-cycle test in [docs/SD.md](docs/SD.md), because uploading a payload does not reset the chip and a "cold" dump taken second in a session is not cold. The bootloader half of all this is now reproducible with `tools/sl6806-boot` (36 tests). No writing to cards, and no four-bit mode. **The filesystem layer above it is done and tested**: [`sl6806_fat.c`](cores/sl6806/sl6806_fat.c) is a read-only FAT16/FAT32 reader that takes a block-reader callback and knows nothing about SD, so 43 host checks read real FAT16 and FAT32 volumes built in memory. `examples/SdFiles` walks the whole stack and says where it stops — which is at the block read, on this board. |
| Bluetooth | **Candidate register window found; its gate is decoded and RUN, 2026-08-13.** `0x400E2000`, next to the application's HCI command dispatcher — about ninety load sites over thirty registers in three clusters, including four free-running counters of 25 and 30 bits that read like a link controller's native clock. The block's functional clock is `0x400E0000` **bit 0** and its reset is `0x400E0008` bit 0, and the shared bring-up that opens the whole `0x400Exxxx` group is transcribed in [`cores/sl6806/sl6806_bt.c`](cores/sl6806/sl6806_bt.c) — which also settles that module 46 gates the *group*, not the camera, and that `0x4008011C`'s value is `0x31`. No bit's meaning is known. **On hardware the gate opens**: the PLL locks from a payload (`0x40080008` came back `0xD0010C04`, lock bit set), `0x400E0000` holds `0x21`, and four registers in the window wake up — but the counters stay at zero, so the link controller is not running. `examples/BtProbe`; see [docs/BLUETOOTH.md](docs/BLUETOOTH.md). |
| Touch panel | **Interrupt works; coordinates written, not yet confirmed.** `examples/TouchDemo` resets the controller and watches its interrupt pad — that much has run on hardware. For the coordinates it bit-bangs I2C on the two TWI1 pads rather than waiting for the TWI controller to be found, and reads the CST816 the way the vendor does; that path has not been run yet. |
| I2C on bare pads | **Works — confirmed on hardware.** `examples/CameraDemo` bit-bangs TWI 0 on two pads and reads the FM tuner's chip id: `0x5808`, the exact value the stock driver checks for, on a read-only pass. First device this framework has ever read over I2C, and it needs no TWI controller. |
| Camera — the sensor | **The module is fitted and works under the stock firmware; from a payload `0x68` has always been silent, and the missing rail is now reachable — written, not yet run.** Decoded register-for-register in [`docs/sl6806_re_notes.md`](docs/sl6806_re_notes.md) §7h, and the rail in §7m. The vendor's `power_on` is pads, delays and "clock channel 6" — which turns out to be an LDO at 2.8 V, not a clock, and which it switches through the indexed register file. That file was the blocker and is now found: not an MMIO block at all but a chip at I2C `0x30` behind a mailbox at `0x400F7000+0x100`, driven by [`cores/sl6806/sl6806_regfile.c`](cores/sl6806/sl6806_regfile.c). `examples/CameraDemo` performs the vendor's six writes, and then — if the chip id answers — replays the vendor's own 203-pair init table, lifted out of flash by `tools/sl6806-sensortab`. Whether the sensor answers at all is the open question, and it needs a P20 and five minutes. |
| Camera — the pixel path | **Mapped and enabled; the sensor still will not start.** The DVP front end at `0x400E1000` is decoded in [`cores/sl6806/sl6806_dvp.h`](cores/sl6806/sl6806_dvp.h) (§7n) — geometry, crop, scaler, and the DMA destination register found by mapping the live block. It can be woken: mask ROM module id 46 for its registers, then `0x400E0000` bit 6 for its logic — which is §15's "dead" register, alive once something gates it open. On hardware the block accepts and reads back a full configuration, and the sensor's own output pins still sit high-impedance against a pull-up. Rail, enables, bus, pads, PLL, both clock families and all 64 camera-clock settings have been applied. That is a complete negative, not a missing step; §7n says what would settle it. **And the `+0x30..0x3C` DMA cluster is not a latch.** [`cores/sl6806/sl6806_dvp.c`](cores/sl6806/sl6806_dvp.c) drives the cluster DvpProbe only mapped — address, then length, then a trigger, this driver's own convention since no vendor code ever writes these four registers. On hardware the descriptor registers hold what they are given, and the destination buffer never changes — but that negative is worthless here, because the sensor never starts and a capture engine with no pixel source has nothing to write. `+0x30` answers instead, needing no source: the census wrote each bit and read it back in the same breath, and a read one millisecond later shows bits clearing *themselves*, in a pattern where bit 0 clears unless bit 3 holds it and bit 1 clears if bit 0 is set. 749 readings across 107 rounds and a power cycle, zero variance — sequential logic behind the register, and the first fact about this cluster a register census could not have produced. `examples/DvpDma` reproduces it in isolation; 45 host tests hold the driver to its contract. It still does not show a byte moving. |
| FM tuner | **Tunes, on hardware.** RDA5807 family on TWI 0 at `0x10`, chip id `0x5808` (§7i). `examples/FmDemo` enables it and sweeps the band over bit-banged I2C: 41 channels tuned, each setting tune-complete and reading its own channel number back. First peripheral this framework has successfully *written* to. No audio path yet — the proof is the tuner's own status registers, and RSSI stays at the noise floor until headphones are plugged in, since the lead is the aerial. |
| Flashing to run standalone | **Unproven.** See [docs/FLASHING.md](docs/FLASHING.md). |
| Starting the vendor's firmware from a payload | **Works — 2026-08-14.** `examples/FirmBoot` copies the FIRM segment out of the flash image already on the device into SRAM and enters it — no flash write, and a power cycle undoes it, which is the whole difference from `MODE=firmware`. The load address is not in the header: it is the entry less the 256-entry vector table (§7m), and [`sl6806_firm.c`](cores/sl6806/sl6806_firm.c) derives it and then checks it against the image's own vector table before jumping, because getting it wrong is a hard fault with USB already gone. 15 host tests, all refusals bar one. On hardware it starts the stock application, which takes USB with it and stops sketch upload until a power cycle — which is exactly what success looks like. That also confirms §7m's load address on hardware: the derivation is entry less the 256-entry vector table, and the vector-table cross-check passed before the jump. It exists to ask what bootloader mode withholds, since the mask ROM leaves both the SD host and the serial port switched off there and one of the two still will not run a command after a payload wakes it (§28). **What it has not yet been asked is the SD question** — run it with a card in the slot and see whether the host enumerates `301a:2801` and mounts it. |

Two honest caveats worth reading before you trust output:

**The CPU clock is measured, not documented: 64 MHz — and now partly
explained.** [V] 2026-08-14, §25: the bootloader's first init phase programs a
PLL to exactly **384,000,000 Hz**, which is six times the measured figure. The
PLL is at CRU `+0x10`/`+0x14` and its rate reads back at `+0x00` as
`(m × 48 / d)` MHz — so the sentence below about the CRU holding "no PLL
multiplier" was wrong, though which divider feeds the core is still not
established.

**The measurement itself:** `F_CPU` now defaults to
64,000,000 because that is what one P20 Player was measured at — 64,000,071 Hz
with a bracket of ±0.06% containing exactly one whole MHz. ~~The clock and
reset unit at `0x40080000` holds dividers and gates, not a PLL multiplier, and
nothing in the dump establishes the crystal~~ — **retracted (§25)**: it holds
both. The PLL is at `+0x10`/`+0x14`, its rate reads back at `+0x00` as
`(m × 48 / d)` MHz. **[M] 2026-08-14 it reads `0xD0010802` on a live device —
m = 8, d = 2, so 192 MHz, with its lock bit set — and 192 / 3 is the measured
64 MHz exactly** (§30). So the core divider is 3 and the clock is no longer
purely a host-timing measurement; the bootloader separately asks for 384 MHz,
which would make that divider 6.

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

The panel is an **NV3030B — 240x296 RGB565, drawn at controller offset
(0, 12)** — behind a standard MIPI DCS command set, with a **33-command
vendor init sequence** recovered from the firmware into
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

CI runs all three on every push and pull request, plus a build of ten
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

**Not every "USB card reader" is an SL6806.** If `lsusb` shows vendor ID
`20d6` rather than `301a`, that is a different chip vendor (Actions
Semiconductor) with an unrelated protocol - see
[docs/ACTIONS_CARDREADER.md](docs/ACTIONS_CARDREADER.md).

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
| `actions-cardreader-probe` | **Not for the SL6806.** Identifies an Actions-brand (VID `20d6`) USB card reader and walks it into ADFU mode — see [docs/ACTIONS_CARDREADER.md](docs/ACTIONS_CARDREADER.md). |

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
| `0x40000000` | pad / pin function mux; `+0x70`/`+0x74` is also a ten-domain power handshake ([`sl6806_bt.h`](cores/sl6806/sl6806_bt.h)) |
| `0x40009000` | **audio controller** ([`sl6806_audio.h`](cores/sl6806/sl6806_audio.h), [docs/AUDIO.md](docs/AUDIO.md)). §7c filed this as "timers" from the shape of `+0x108`/`+0x208`; those are its two DMA directions, and the timers are at `0x40099000` |
| `0x40070000` | DMA |
| `0x40080000` | clock & reset ([`sl6806_cru.h`](cores/sl6806/sl6806_cru.h)) |
| `0x400A0000` | **[I] a hardware mutex** (§27). `+0x10 + id*4` is a lock array — spin while non-zero, write zero to release; the code guarding the TWI mailbox uses it |
| `0x400D9000` | LCD controller ([`sl6806_lcdc.h`](cores/sl6806/sl6806_lcdc.h)) |
| `0x400E0000` | peripheral **functional** clocks, one bit each: camera 6, Bluetooth 0, the `0x400E2300` cluster 1, and 5 taken by the group's own bring-up. Resets are the matching bits of `+0x08`. Gated behind module clock 46, which is why §15 read it as dead (§7n) |
| `0x400E1000` | camera front end / DVP ([`sl6806_dvp.h`](cores/sl6806/sl6806_dvp.h)) |
| `0x400E2000` | **candidate** Bluetooth window, three clusters, unconfirmed on hardware ([`sl6806_bt.h`](cores/sl6806/sl6806_bt.h), [docs/BLUETOOTH.md](docs/BLUETOOTH.md)) |
| `0x40091000` | **[I] a UART — the bootloader's printf sink**, driven, and [M] transmitting ([docs/UART.md](docs/UART.md), §26). Module clock id 73, ROM clock id 22, pad bank 1 pin 2 function 6, 1,500,000 against 48 MHz; FIRM brings it up identically. The whole write path is ROM `0x1D0`: wait for status bit 4, store nine bits to `+0x00`. `examples/UartProbe` sends a banner once a second and mirrors the whole console to it; `examples/UartPin` finds the physical pin with a multimeter. **What is unproven is the last hop** — the rate is the vendor's 1.5 Mbaud and the framing is a guess |
| `0x40099000` | timers (`HAL_timer_*`) |
| `0x40003000` | **SD/MMC host** ([`sl6806_sd.h`](cores/sl6806/sl6806_sd.h), [docs/SD.md](docs/SD.md)). §7c filed the SD host at `0x400F7000` on the strength of a mailbox that turned out to be §7m's; the real base appears once per image, cached in a driver handle (§23) |
| `0x400F7000` | SPI flash host; `+0x100` is a **TWI master** — the vendor's own name, from the `rtwi op in isr` string on its mutex (§27) — and the indexed register file is the chip on it (§7m). **Not** the SD host |

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

1. **The audio block, which is found, woken, and will not run.** Six candidate
   causes are closed by measurement or call graph — the output route, the bit
   clock, all 32 bits of `0x400E0000`, the general DMA controller, the
   vendor's system clock init, and a hidden register behind the coefficient
   window. What is left is that the block accepts a descriptor and retires
   4796 bytes in 10 µs (480 MB/s, which this bus does not do) with every
   register holding exactly what the vendor's own code puts there.
   [docs/AUDIO.md](docs/AUDIO.md) has the whole elimination table and the four
   sketches. **The most useful thing anyone can do here is have a fresh idea**
   — this one has been narrowed about as far as reading can narrow it.
2. **Walk `0x400E0000`'s three unattributed bits with the PWM counter as the
   witness.** [M] 2026-08-13, `examples/SdWall`: **the register is seven bits
   wide** — writes to bits 7..31 are dropped — so it is bits 2, 3 and 4, not
   28 of them, and every previous walk of it spent most of its time on bits
   that do not exist. `examples/AudioWall` already walked them against an audio
   witness and found nothing, but the audio witness may simply be insensitive
   — the block has other problems. The PWM's stalled counter is a cleaner
   test, and the walk only became possible on 2026-08-13, when `BtProbe` got
   that register to hold bits from a payload. Four bits are attributed
   (camera 6, Bluetooth 0, the `0x400E2300` cluster 1, group 5), so the walk
   can check itself.
3. **The SD host is confirmed at `0x40003000` and will not complete a
   command — and the usual answer is now ruled out.** `examples/SdWall`
   walked `0x400E0000` with CMD0 as the witness and found that the register
   is only seven bits wide; all seven were tried and none is the SD host's.
   The clock is ruled out too (all 64 settings of `0x40080080`, found already
   enabled and undivided), and so is CTRL (every writable bit, before and
   after the reset). What those runs established is sharper than any of them
   set out to test: **the command register is live** — the start bit latches
   and clears on every configuration — so the block accepts commands and
   executes nothing. `examples/SdScope` then mapped the block around one CMD0:
   one register moved, the one that was written, and the busy bit was gone
   within ~300 ns. `examples/SdLife` then showed the FIFO works: 16 words
   into `+0x200` and the flags in `+0x48` move, so the datapath is clocked
   and only the command state machine is at fault. `examples/SdPads` then tried to watch the card
   clock on a pin and **could not have worked** — the pad input buffer is off
   outside functions 0 and 14, which `PadScope` measured during the LCD work.
   Software is blind to this bus, as it is to the LCD's; the clock question
   now needs a probe on the socket. **`examples/PadMap` is the run that
   matters, and it wants two — once with a card in the slot and once
   without.** It censuses every pad's function and level without writing
   anything (this board's pinout, never recorded) and pull-tests the pads
   already in function 0, so the diff between the two runs gives the SD
   pinout and the card-detect contact.
4. **Run the display driver on a real P20 and report what happens.** This is
   worth more than any further reading of the dump. The whole stack is
   written and the panel is a QSPI display; what nobody knows is whether the
   two inferred bits of the transfer register and the guessed pixel byte
   order are right. [docs/LCD.md](docs/LCD.md) has the checklist. It cannot
   brick anything in payload mode.
5. **This board's pinout.** [M] 2026-08-13, `examples/PadMap`: **80 pads are
   bonded out and the boot ROM assigns six of them** — bank 0 pins 0–3 and
   bank 2 pins 0–1 on function 2, bank 1 pin 9 on function 3, everything else
   parked on function 15. A function nibble of 0 means the pin does not
   exist; six banks show the same shape. That is the first pad map this
   project has had, and what is still missing is which *peripheral* each
   assigned pad belongs to. The pad controller is done
   ([`sl6806_padctl.h`](cores/sl6806/sl6806_padctl.h)); what is missing is
   which pad each button and LED is on. The ids are immediates at the stock
   firmware's 54 GPIO call sites, so this is a reading exercise plus a meter.
   Four pads came out that way already — the touch panel's reset and
   interrupt, and the camera's reset and power-down — so the method works;
   see §7h of the notes. The buttons are the ones still open.
6. ~~**Find the PWM's bit in `0x400E0000`**~~ — merged into item 1, which is
   the same walk. `0x400E0000` was recorded as dead from a payload for two
   sessions; it is gated, `sl6806_module_enable(46)` opens it (§7n), and the
   whole vendor gate sequence including the PLL has now been run (§17,
   measured).
7. ~~**Where the indexed register file lives**~~ — found (§7m): a chip at I2C
   `0x30` behind a mailbox at `0x400F7000+0x100`, not a base address, which is
   why five searches for a base could not have worked. Read and written on
   hardware; its five LDO rails decode to real supply voltages.
   `examples/RegFileProbe` dumps it without writing anything. **The camera it
   was meant to unlock is a clean negative** — rail, enables, bus, pads, both
   clock families and the whole front end all check out and the sensor never
   starts (§7n). What is left there needs SWD or `MODE=firmware`, not more
   probing.
8. **An FM driver**, one of the cheapest working devices on the board: an
   RDA5807 at `0x10` on a bus that already reads correctly (§7i). Standard
   part, published register map, and the id read is done.
9. **The TWI controller's base address** — demoted, because it is now a speed
   problem rather than an access one. `examples/TouchDemo` bit-bangs bus 1
   and `examples/CameraDemo` bit-bangs bus 0, both on pads alone, and the
   second has read a real device. What the base would buy is throughput,
   which matters for a camera and not much else.
8. **A DMA driver.** The display pushes pixels 16 bytes at a time because
   that is the FIFO depth. The vendor uses the DMA controller at
   `0x40070000`; its command-list format is decoded at the bottom of
   [`sl6806_lcdc.h`](cores/sl6806/sl6806_lcdc.h).
9. **Run `examples/BtProbe` on a real P20 and report what happens.** A
   candidate Bluetooth register block, `0x400E2000`, is found — see
   [docs/BLUETOOTH.md](docs/BLUETOOTH.md) — but read-only and completely
   unconfirmed on hardware. Cheap to run, and the outcome (flat zero, flat
   ones, or something that moves on its own) decides whether this is worth
   any further reverse engineering at all. Note it shares the `0x400Exxxx`
   group with the camera front end, so it may well need a bit in
   `0x400E0000` before it reads as anything but zeros (§7n).
10. ~~**The real CPU clock**~~ — `make calibrate` measures it against the host
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
                  controller (sl6806_lcdc.*), clock map (sl6806_cru.h),
                  the indexed register file and its LDO rails
                  (sl6806_regfile.*), the camera front end (sl6806_dvp.h)
cores/sl6806/gfx/ framebuffer, font, panel + LCD bus, Display
variants/         board definitions (pin maps go here)
ld/               linker scripts, one per build mode
tools/            host-side Python tools
examples/         Hello, Blink, GfxDemo, TouchDemo, CameraDemo, FmDemo,
                  RegFileProbe, DvpProbe, DvpDma, LcdProbe, PadScope, PadSweep,
                  BacklightHunt, MmioProbe, RomProbe, CallbackProbe, BtProbe
tests/host/       native tests for console, graphics, the panel and the LCDC
docs/             BRINGUP.md, DUMPING.md, FLASHING.md, LCD.md, BLUETOOTH.md,
                  sl6806_re_notes.md, ACTIONS_CARDREADER.md
3rd/              smartlink_flash submodule, actions_flash submodule
                  (for a different, non-SL6806 device - see
                  docs/ACTIONS_CARDREADER.md)
```

## Related projects

[tytydraco/mp3c](https://github.com/tytydraco/mp3c) converts audio, images,
video and text into the device-specific formats these players expect —
matching orientation and rotation direction to the target chip (it names SL
devices explicitly, alongside ATJ) and cropping to fill the screen. It is a
content-prep tool, not a flashing or firmware tool: useful once a device is
up and you want real media on it, orthogonal to everything in this repo.

Its device spec sheet lists several stock SL6806 units, and the encode
recipe they share: AVI container, H.264 baseline, `yuvj420p`, transpose
counter-clockwise, crop+upscale to fill the panel, ~1-2 Mbps / GOP 8-12,
`pcm_s16le` audio up to 48 kHz — via a patched FFmpeg (`ffmpeg-yp3-patch`).
That is what the stock firmware's decoder was built against, and worth
knowing if video decode is ever taken on here.

## Credit

The USB protocol, the boot ROM entry points and the payload mechanism are all
from [ilyakurdyukov/smartlink_flash](https://github.com/ilyakurdyukov/smartlink_flash).
Without it none of this would be reachable.
