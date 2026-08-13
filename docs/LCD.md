# Display: how it works, and what to check first on hardware

The display stack is in three layers, and all three now exist.

| Layer | State |
|---|---|
| Drawing — framebuffer, primitives, font, `Display`/`Screen` | **done**, 64 host tests |
| Panel — geometry, init sequence, windowing, sleep/wake | **done**, recovered from the firmware, 53 host tests |
| Bus — putting a byte on the wire | **done**, 195 host tests against a model, and **verified on the panel 2026-08-07** |

**It works.** `examples/GfxDemo` draws shapes and text on the panel, in the
right colours, and the whole stack from a `Screen.fillRect()` call to a lit
pixel is verified end to end.

That is worth dwelling on for a moment, because the bus driver was written
entirely by disassembling the stock bootloader, was never run against a panel
while it was being written, and had **two bits of the transfer register
inferred from where the vendor sets them rather than read**. Those two bits are
right. So is the 33-command init sequence, the window arithmetic, the QSPI
frame layout and the pixel byte order.

**What had been wrong the whole time was the backlight**, which is not part of
the LCD path and which nothing here used to turn on. Every "the driver produces
no picture" result in the history of this repository was taken with the lamp
off, and none of them meant what they appeared to. Call
`sl6806_backlight_begin(100)` (`cores/sl6806/sl6806_pwm.h`) — `GfxDemo` does.
It is on/off only; the PWM counter does not run, so duty has no effect, which
does not matter here. §14a has that story.

The rest of this document is still "how it works", and its checklist is still
worth keeping for the next board or the next bug — but it is no longer a list
of reasons the screen might be dark.

```cpp
#include <Arduino.h>

static sl6806_color_t band[160 * 40];

void setup() {
    Serial.begin(115200);
    Screen.begin(band, 160, 40);   // brings the controller up by itself
    Screen.fill(SL6806_BLUE);
    Screen.drawText(2, 2, "hello", SL6806_WHITE);
    Screen.display();
}
```

```sh
make SKETCH=examples/GfxDemo RUN_MODE=poll run
```

## The panel is a QSPI display

This is the thing that unlocked the rest, and it is worth stating plainly
because every earlier note in this repository was written without it.

The stock bootloader's command writer sends **four bytes for every DCS
command**:

```
0x02  0x00  <cmd>  0x00
```

That is the standard QSPI display command frame — a one-lane `0x02` write
opcode, then a 24-bit address whose middle byte is the DCS command. Pixels go
out afterwards on four lanes behind a `0x32` opcode. Once you see that, the
unexplained fields in the old analysis all resolve at once:

- the panel descriptor's `+0x0C = 0x32` and `+0x0D = 0x02` are the two QSPI
  write opcodes, not "50" and a device id;
- the LCDC command list's mystery `b` field, "a descriptor byte shifted left
  8", is the DCS command inside that 24-bit address;
- its `a` field is which opcode to use.

The old note could not settle the `b` field because it compared the
bootloader's descriptor against the *application's*, and the application's
has four extra bytes at the front (`x_offset`, `y_offset`) so every field
after `+0x04` is numbered four higher. Line them up and `+0x0D`/`+0x0E` in the
bootloader are CASET and RASET, exactly as the window records need.

## What the driver does

`cores/sl6806/sl6806_lcdc.c`, a transcription of the bootloader's own driver.
Bootloader addresses are on every function so any line can be re-checked.

**Bring-up**, from `0x00827AA8` and the application's copy at `0x00D3E2A4`:

1. gate the module off and on — CRU `0x40080064` and `0x40080074`, bit 15
2. pick the module clock — CRU `0x4008010C`, field `0xF10` ← `0x910`
3. `HAL_lcdc_module_init` — `CFG0 = 0x9999`, `CTRL` bit 0, `CFG1` bits 3 and 6
4. configure the eight pads and the two lane-map words
5. soft-reset the controller — `CTRL` bit 31, then bit 30

**A command**: assert CS, set bit 17 ("emit a frame"), send `0x02`, `0`,
`cmd`, `0` one byte at a time out of `+0x24`, then each parameter out of the
FIFO — releasing CS before the final byte, which is what the vendor's `last`
argument does.

**A pixel push**: the same frame with the `0x32` opcode and bit 17 *cleared*,
so the transaction stays open, then the pixel bytes 16 at a time.

The 16 is the FIFO depth; the bootloader's own short-write helper at
`0x00821AA8` refuses anything longer. The vendor streams pixels with DMA
instead, which is the obvious thing to do later — a full 240x296 frame is
about 8900 transfers this way.

`tests/host/test_lcdc.c` runs all of that against a model of the controller
and asserts the bytes that would leave the pin, including the whole 33-command
init sequence.

## The GPIO registers, which turned out to matter

The panel has a reset line, so the bus needs GPIO, and the notes in this tree
said the GPIO registers had not been found. They had not been found because
nothing in flash writes them: both the bootloader and the application reach
GPIO through thunks into the mask ROM.

The ROM dump has them. `cores/sl6806/sl6806_padctl.h` is the map, and the
missing piece was a six-entry table of bank bases at ROM `0x00065004`:

```
0x40081000  0x40081040  0x400F6080  0x400810C0  0x40081100  0x400F6000
```

with the vendor's packed pin id decoding as bank `[19:16]`, pin `[15:11]`,
function `[10:7]`, initial level `[6]`, drive `[5:4]`, pull `[3:0]`. Direction
is the function nibble: 0 is input, 1 is output.

So `digitalWrite()` works too, for any pin whose pad id is known — which on
the P20 is still only the two reset lines. Finding the rest is a pinout
problem now, not a register problem.

## On hardware: what to check, in order

```sh
make SKETCH=examples/LcdProbe RUN_MODE=poll run
```

`examples/LcdProbe` answers most of what follows in one run: it reports
whether the bus came up, dumps the controller, clock and pad registers,
counts how many polls each transfer takes, and lets you flip the guessed
values from the monitor without rebuilding. Everything below is a real
possibility, not a formality.

**0. Is the backlight on?** — *solved; turn it on and move to step 1.*

```c
#include "sl6806_pwm.h"
sl6806_backlight_begin(100);      /* examples/GfxDemo already does this */
```

It is PWM channel 3 of the block at `0x40084000`, module id 68, output on bank
1 pin 0 in alternate function 4. On/off only: the counter does not run, so duty
has no effect, which does not matter here. §14a of the RE notes has the
bring-up order and the list of things that were wrong on the way.

**This changes what every other step below means.** Until now a dark panel was
consistent with a perfect driver, so none of the "no picture" results on this
board distinguished anything. Re-run before believing any of them.

The old advice here — shine a light at the panel at an angle and flip between
white and black to read it by reflection — is still a decent trick if you ever
suspect the lamp again, and `examples/LcdProbe`'s `w` and `k` keys still do it.

**Still worth trying while you are here: `vcomo`.** VCOM is an LCD panel
supply, and with that rail off the controller would accept every command over
QSPI while the glass stayed blank — exactly the symptom this document exists
for, and unaffected by the backlight. `tools/sl6806-padscan dump.bin --outputs
--sites` lists the seven plain outputs the firmware drives; the console handler
at `0x00D0AEE0` names `0x0001A0D0` (bank 1 pin 20) `vcomo`, and `0x000508C0`
(bank 5 pin 1) is toggled in the display's suspend/resume pair.

`examples/LcdProbe` drives those candidates: `A` turns them all on, `Z` all
off, `l` arms the next one and names it, `y`/`u` drive the armed one. `l`
announces before `y` drives, deliberately — if the device freezes, the line
naming the pad has already reached you.

**1. Is `delay()` being clamped?**

If the log has

```
*** SL6806: delay() clamped ***
    A delay of 120 ms was shortened.
```

during bring-up, the panel never came out of sleep and this is the whole
problem. `RUN_MODE=poll` caps `delay()` at 50 ms, and the cap is installed
before `setup()` runs. The panel init ends `SLPOUT / 120 ms / DISPON`, and
the reset pulse ends with a 120 ms settle; both waits are how long the
controller takes to become able to answer at all, so DISPON lands on a
controller that is still asleep. Everything else looks perfect: transfers
complete, no error, black screen.

`sl6806_lcd_run()` and `sl6806_dcs_begin()` now run their waits at full
length regardless of the cap, bounded at 500 ms each, and
`tests/host/test_panel.c` asserts a bring-up still totals 320 ms with a
50 ms cap set. If you are on an older build, that clamp message is the bug.

**2. Does the device survive bring-up at all?**

Step 1 gates two clock domains off and back on. Which modules those gates
cover is not established — the LCD is the only peripheral either image uses
them for. If the device goes quiet the moment `Screen.begin()` runs, that is
the first suspect.

This is recoverable. A payload never writes flash; unplug it and it comes back
in bootloader mode. Test with `MODE=payload` (the default) before you consider
flashing anything.

**3. Does anything reach the controller?**

If a transfer is started and the controller never reports it complete, the
driver gives up after a bounded wait and prints

```
*** SL6806: the LCD controller stopped answering ***
```

It bounds the wait rather than spinning because in `RUN_MODE=poll` this code
runs inside the boot ROM's USB handler, and spinning there takes the device
off the bus until it is unplugged. If you see that message, the module clock
or the gates are wrong for this board.

**3b. Do the pins actually move?**

`p` drives an LCD data pad as a plain GPIO to prove the pad and its input
register work, then hands it back to the controller and samples the bank
during live traffic. Read its verdict carefully: a pad that never changes is
*not* proof the controller is idle, because most pad controllers disable the
input buffer for a pad in an alternate function, and such a pad reads 0
whatever happens on it.

`P` settles which case it is. It finds a pull-up empirically on a plain input,
then applies the same pull to the pad in function 2 with no driver: if the
input register still reads 1, the buffer works in alternate mode and `p` can
be believed. If it reads 0, neither test can see anything and the next step is
a scope on the panel flex, not more software.

**On the reference unit this has been run, and the answer is that software is
blind.** `P` finds pull selector 8 holds a plain input high, and the same pull
on the same pad in function 2 reads 0.

`examples/PadScope` then settled it properly, sweeping all eight LCD pads
through all sixteen function nibbles with a pull-up applied. Every pad gives
the same answer: the input register reads 1 in function 0 and function 14, and
0 in every other function. So the buffer is off outside those two modes, there
is no function where the controller is connected *and* the pin is readable,
and nothing that does not involve a probe on the flex can see this bus.

What is known on that unit: transfers complete in a clock-dependent number of
polls (3 at modclk `0x910`, 1 at `0x310`, so the controller is doing real
timed work), no timeouts, every register reads back exactly as the vendor
programs it, the pads are configured exactly as the stock firmware configures
them, and the panel does not answer reads. The five power pads the firmware
drives as outputs — `vcomo` included — make no difference.

**3c. Brute-force the inferred bits.**

Three things in the driver were deduced rather than read: bit 17 ("emit a
frame"), bit 18 ("hold chip select"), and whether CTRL bit 4 matters. Polarity
is exactly what reading a disassembly gets backwards, and getting either of
the first two backwards means every command goes out without a valid QSPI
header — a panel that ignores everything, which is the symptom.

Since the panel is the only instrument left, try all eight:

    m    next combination: re-init, paint white, look at the glass

The combinations are switchable at run time through `quirk_invert_frame`,
`quirk_invert_cs` and `quirk_ctrl_bit4` in `sl6806_lcdc_config_t`; all default
to the vendor's behaviour as transcribed.

**All eight have been run on the reference unit. None lights the panel** — but
two of them taught us something:

- **CTRL bit 4 is command-list mode.** Set it and no polled transfer ever
  completes. That both explains why the vendor sets it only in the
  command-list builder and proves the controller responds to configuration,
  so it is a live peripheral and not a dead address.
- **Inverting bit 17 changes a 16-byte data transfer from 6 status polls to
  21** — the signature of a command frame being emitted per transfer. That
  supports the "emit the frame" reading, and says the default is the one that
  does *not* repeat a header on every pixel chunk, which is what you want.

So the driver's transcription survives the brute force. The fault is
elsewhere.

**And the lamp test has now been done: flashing full white against full black
at 2 Hz, under a bright light at a shallow angle, the glass does not change.**
So this is not a hidden-but-working display. Nothing is reaching the panel.

One more thing tried and closed: the vendor's ISR clears status bits 27 and 22
after every transfer and this driver never does, which looked like a real
divergence. It is not - writing those bits back does not clear them, so they
are level status rather than latched flags, and `+0x14` reads `0x08020000`
either way. `e` in the probe still toggles it, and it changes nothing.

### The backlight: every pad on the chip, eliminated

Run on hardware with `examples/PadSweep`, one pad at a time, host-paced, each
announced before it was driven so a wedge would name itself:

| set | pads | result |
|---|---|---|
| named by `sl6806-padscan` | 17 | nothing |
| bank 0 | 32 | nothing |
| bank 1 (unnamed) | 8 | nothing |
| bank 2 | 31 | nothing |
| bank 3 | 25 | nothing |
| bank 4 | 16 | nothing |
| bank 5 | 30 | nothing |

**Every pad on the part has been driven high individually and none lights the
panel**, while the stock firmware lights it brightly. So the backlight enable
is not a GPIO that a static high turns on. What that leaves:

- an **active-low** enable, or one with an external pull-up. Only half the
  search was done - `SWEEP_LEVEL=0` drives low instead, and is the one cheap
  thing still untried.
- a backlight driver that wants a **pulse train** rather than a level. Some
  one-wire LED drivers latch a brightness from counted pulses and ignore a
  static input, which would fit: the vendor drives this from PWM.
- the **PMU**. `/dev/pmu` is another SRAM-resident driver, and `enable coreldo`
  in FIRM suggests rail control that no pad can reach.

Two incidental findings from the sweep, both worth keeping: most pads read
function 0 with nothing configuring them and are almost certainly unbonded;
and the handful that sit on a real function without any flash call site
setting them - bank 0 pins 0-3 (function 2, probably the XIP flash bus), bank
1 pin 9 (function 3), bank 2 pins 0-1 (function 2) - are configured by
something that is not in the flash image.

### What is left, and it is not more software

Everything observable from software is consistent with a working controller,
and software cannot observe the one thing that matters. The next step needs a
scope or a logic analyser on the panel flex:

    T    in examples/LcdProbe: hammer the bus continuously until a key

Probe bank 1 pins 1-6 and 8. **If there is no clock**, the controller is not
reaching these pads and the pin mux is wrong - which would mean function 2 on
bank 1 is not what routes *this* controller there, and the next place to look
is the mask ROM's own mux tables rather than the firmware's call sites.
**If there is a clock**, the bus works and the problem is at the panel end.

### One caveat about the source this was written from

Worth stating, because it bounds how much the transcription can be trusted:
the only byte-level driver that could be read is the **bootloader's**. The
application's is SRAM-resident and in no dump. And there is reason to doubt
the bootloader's LCD support was ever exercised on this board - its panel
descriptor is 320x385, which is not this panel, and it configures bank 4 pads
where the application uses bank 1. It may be generic SDK code that the vendor
never ran here. The register semantics it implies could be right in general
and wrong in some detail this board depends on.

**4. Is the screen dark but the sketch healthy?**

Then the transfers complete and the panel is not listening. In rough order of
likelihood:

- **the reset pad**. `0x000138CB` is bank 1 pin 7. Drive it by hand with
  `digitalWrite(PIN_LCD_RESET, ...)` and watch it with a meter.
- **the pad functions**. The seven data pads are set to function 2, read off
  the application's own setup at `0x00D3E9DC`. If your board is wired
  differently, that is where to change it.
- **the lane map**, `0xB0C432DB` / `0xBBBBBBBF`. Same source. A wrong map
  sends the right bytes down the wrong wires.
- **the module clock**, `0x910`. The bootloader uses `0x310` for the same
  peripheral and runs the panel slower; if the panel is marginal, try that.
- **bit 17 and bit 18**. These are the two `[I]` bits in the whole driver —
  inferred to be "emit a frame" and "hold CS" from where the vendor sets and
  clears them. If commands are getting through but pixels are not, or the
  panel only ever shows the first 16 bytes, these are why.

**5. Is there a picture but the colours are wrong?**

Flip `swap_pixel_bytes` in `variants/p20_player/lcdc.c` (the probe's `s` key
does it live). It should not need flipping: the vendor's single-pixel write at
`0x0082837C` byte-swaps the colour before pushing it — `r0 = colour >> 8 |
colour << 8` — which settles the question in favour of most-significant-byte
first. But it is one bit and reds looking blue is the symptom, so it is cheap
to rule out.

The *application* now says the same thing twice over, which is worth knowing
because it is the half that used to be missing (§12.5b of the RE notes called
the byte order unsettled "because the vendor's framebuffer comes from LVGL").
LVGL is built with `LV_COLOR_16_SWAP = 0`, so the framebuffer at `0x0087B800`
is little-endian RGB565 — and the window writer at `0x00D3EBD4` does an
explicit `rev16` on every pixel it pushes through the FIFO. Framebuffer
low-byte-first, wire high-byte-first, swap in the driver. See
docs/sl6806_re_notes.md §13.

**6. Is the picture shifted?**

That is the panel's `(0, 12)` offset, and it is applied in
`sl6806_dcs_flush()` and checked by both host test suites. If it is off by
something other than 12 rows, the panel is not the one in the reference dump.

## Regenerating the panel tables

Nothing about the panel is transcribed by hand:

```sh
tools/sl6806-panelseq dump.bin          # readable listing
tools/sl6806-panelseq dump.bin --c      # the exact tables in panel.c
```

The tool finds the panel descriptor by signature rather than by address
(`0x2C 0x2E 0x2A 0x2B 0x36` at `+0x0F`), then disassembles the five routines
it points at. The init sequence is not a table in the firmware — it is ~150
open-coded calls with the byte to send as an immediate in each — so recovering
it means walking the code, which is what the tool does.

`tests/host/test_panel.c` links the real tables and checks them command by
command, so regenerating them cannot silently change what gets sent.

## Memory, before you pick a resolution

An RGB565 framebuffer costs `width * height * 2` bytes:

| Panel | Framebuffer | Fits in payload mode (~38 KB heap)? | Firmware mode (~190 KB)? |
|---|---|---|---|
| 128x64 | 16 KB | yes | yes |
| 160x128 | 40 KB | no | yes |
| **240x296 (this board)** | **139 KB** | **no** | yes |
| 320x240 | 150 KB | no | yes |

At 240x296 a full framebuffer rules itself out in payload mode entirely, so
band rendering is the realistic approach on this board rather than an
optimisation. Render a band and push it repeatedly — `Display::begin(buffer,
w, h)` takes a buffer smaller than the panel, and `displayAt()` places it.
`examples/GfxDemo` does exactly this with a 160x40 band.

`sl6806_dcs_flush()` handles the offset and the clipping, including the case
where a clipped rectangle makes the source rows non-contiguous, so a band that
runs off an edge still lands correctly.

## Writing a different bus

The seam is unchanged. Three functions:

```c
static const sl6806_lcd_bus_t my_bus = {
    .name       = "…",
    .command    = my_command,     /* one command byte + n parameter bytes */
    .pixels     = my_pixels,      /* a run of RGB565 pixels for a RAMWR   */
    .pixels_end = my_pixels_end,  /* optional: the stream ends here       */
    .reset      = my_reset,       /* drive the reset pin (optional)       */
};

sl6806_lcd_bus_register(&my_bus);
Screen.begin();
```

Register one before anything touches `Screen` and it wins; the board's own
bring-up only runs when nothing is registered. `pixels_end()` exists because a
rectangle whose rows are not contiguous arrives as several `pixels()` calls,
and a bus that has to decide "this is the last transfer" before starting it —
as the LCDC does — cannot tell which call is the last.

## Testing without hardware

```sh
make -C tests/host
```

- `test_gfx.c` covers the primitives and writes `gfx_demo.ppm` to look at.
- `test_panel.c` runs the real init sequence through a recording bus and
  checks the window arithmetic against the panel's (0, 12) offset.
- `test_lcdc.c` puts a model of the controller behind the MMIO accessors and
  checks the actual bytes: command frames, transaction boundaries, the pixel
  stream across FIFO-sized transfers, the clipped non-contiguous case, and
  that a controller which never answers makes the driver give up rather than
  hang.

## What is still not done

- **DMA.** The vendor streams pixels with the DMA controller at `0x40070000`
  and interrupt 74. This driver polls. The command-list format that path uses
  is decoded and written down at the bottom of `sl6806_lcdc.h`; what is not
  written is a DMA driver.

  **It is worth writing, and the reason is measured rather than assumed.**
  `examples/PushRate` on one P20 Player, 2026-08-13:

  | | |
  |---|---|
  | full 240x296 frame | 167348 us — 829 KiB/s, 5 fps |
  | transfers | 8900, no timeouts, **0 busy_waits** |
  | polls per completion | min 3, max 7 |
  | one LCDC register read | 312 ns (~20 cycles); the same loop on SRAM is 94 ns |

  `busy_waits == 0` is the load-bearing number. It says the controller was
  never still busy when the CPU arrived with the next transfer, and a minimum
  of 3 polls says it finished within a few reads of being asked. **The panel's
  clock is not the ceiling.** So raising the module clock cannot be the fix,
  and DMA is not just offloading time the bus would have spent anyway.

  Where the 167 ms actually goes, with `test_bus_traffic` counting the
  crossings exactly against the model:

  - **~83 ms of register accesses.** 231346 accesses for the frame, 26 per
    transfer, times the measured 312 ns. The model completes on its first
    poll where the device takes 3 to 7, so the hardware figure is a few reads
    per transfer higher than the 72 ms the count alone gives.
  - **~84 ms of software.** What is left. `bus_pixels()` stages every pixel as
    two separate `stage()` calls, so a 16-byte transfer is 16 calls through a
    bounds check and a global before any register is touched — about 75 cycles
    a pixel.

  Both halves are per-16-bytes CPU work, and both are exactly what a command
  list removes, which is why the DMA path is the fix rather than a tidy-up.

  Two cheaper things are worth knowing before starting it, because they
  need no new peripheral:

  - `irq_quiet()` is 8 of the 26 accesses, four read-modify-writes of the
    interrupt enables, once per transfer. It writes the same constants every
    time. Hoisting it is ~15% of the frame — but it is `[V]` vendor sequence
    and `test_lcdc` pins it, so it is a deliberate change, not a cleanup.
  - Staging pixels a word at a time instead of a byte at a time changes no
    bus traffic at all, only how the bytes reach `g_pend`. That is most of the
    software half and it cannot alter what the panel sees.
- **Backlight.** Identified but not implemented: PWM channel 3 at 48000 Hz,
  duty as a percentage, default 60%. The strings are at `0x00C6854E` and
  `0x00C68585`, and `0x00D10354` opens the device and sets the default. The
  PWM peripheral's own registers have not been located — the app reaches it
  through a `/dev/...` driver layer, so that is the next thread to pull.
  Until then the panel needs external light to be read.
- **Rotation.** The descriptor has a rotation index and the vendor turns it
  into a MADCTL write (`0x0082816C`), but this board's descriptor disables it,
  so the init sequence's `MADCTL 0x00` stands.
- **Reading from the panel.** The controller has a read FIFO at `+0x34` and
  the descriptor names RAMRD, so `0x2E` should work; nothing uses it.
