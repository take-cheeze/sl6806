# The TF card slot: the host is `0x40003000`, and the driver was in the ROM

The capability table has said "SD — not yet, hardware confirmed present, no
driver" since this project started. This is the decode that ends the first
half of that sentence. It does not end the second: **nothing here has run on
hardware**, and the sections below are careful about which claims come from
the dump and which are predictions.

Driver: [`cores/sl6806/sl6806_sd.h`](../cores/sl6806/sl6806_sd.h) and
[`.c`](../cores/sl6806/sl6806_sd.c). Sketch: `examples/SdProbe`. Host tests:
`tests/host/test_sd.c`, 72 checks.

```sh
make SKETCH=examples/SdProbe RUN_MODE=poll run
```

## What was wrong, and why it stayed wrong

§7c of the notes files `0x400F7000` as the "storage host (SD/MMC + SPI
flash)", on this evidence:

> `+0x100`/`+0x104` command registers with a bit-31 start/busy, `+0x108`
> argument, `+0x10C`/`+0x110` response, `sdio(e):rx error` strings nearby

Every clause of that is a true statement about something. None of them is
about the SD host.

- The registers at `0x400F7000+0x100` are the mailbox to the chip that
  carries the power rails — §7m found that independently while hunting the
  camera's LDO, and did not go back and correct §7c. They really do have a
  command word with a bit-31 start, an argument and a response, which is why
  they read as an SD host to a search that was looking for exactly that
  shape.
- The `sdio(...)` strings are at file offsets `0xE6B5`–`0xEB60`, which is
  **below** `0x10000` — they are in the HLKJ bootloader, not in FIRM, and
  "nearby" was measuring distance in a file where two different programs sit
  end to end.

The real base is written down twice in four megabytes:

```
mask ROM  0x0003D398   0x40003000
bootloader 0x00822958  0x40003000
```

Both are literal pools feeding a store into a driver handle, and every access
afterwards goes through that handle. §7c's own blind-spot note predicted this
exactly — *"if a peripheral is missing, look for the table it is cached
in"* — and the literal scan that ranked `0x40009000` and `0x400E2000` by
frequency could never have ranked a base that appears once.

## The driver is in the mask ROM

The `sdio(i):`/`hal_sd(i):` strings in the bootloader belong to a disk layer:
mid-buffers, a flush timer, a mutex, retry counting. Underneath it, every
routine that touches a register is a call into the mask ROM, at addresses in
the `0x00004000` range. This is the same shape as GPIO (§7f): flash has the
callers, the ROM has the driver.

| ROM | What |
|---|---|
| `0x000040D2` | write ARG, wait out the busy bit, write CMD with bit 31 |
| `0x000040F4` | command index → command word (the table below) |
| `0x000040E6` | data timeout, block size, transfer length |
| `0x00003D88` | controller reset — set bits 0..2, wait for bit 2 to clear |
| `0x00003D9A` | the clock configuration, both phases |
| `0x00004310` | CMD0, CMD8, then CMD55+ACMD41 until ready — or CMD1 for MMC |
| `0x000044BC` | CMD2 (CID), CMD3 (RCA), CMD9 (CSD) |
| `0x00003E5E` | the CSD parse and the capacity arithmetic |
| `0x00004778` | set up a block read: DCTRL, DLEN, then CMD17 or CMD18 |
| `0x00004806` | drain 512 bytes from the FIFO, eight words at a time |
| `0x00004C90` | wait for a response, turn the status into an error number |
| `0x0000455A` | all of the above in order — `HAL_SD_Init` |
| `0x0003D3A0` | the bring-up: three pads, module 36, ROM clock 17 |

The driver in `cores/` transcribes these rather than calling them, for three
reasons, in decreasing order of how much they matter:

1. **The ROM's polls have no bound.** `while (!(STA & 4)) ;` is fine in a
   boot ROM that owns the machine. In payload mode it runs inside the boot
   ROM's USB handler, and a device that never comes back out is off the bus
   until it is physically unplugged, with nothing printed. Every poll in the
   transcription is finite and every timeout is reported. The ACMD41 loop
   gets a second bound as well as the ROM's count of 65535: a card that
   answered "still busy" every time would otherwise hold the CPU for seconds,
   and the SD specification gives a card one second to power up anyway.
2. **The handle is 0xCC bytes of partly-understood layout**, with a callback
   pointer at `+0x8C` that the ROM dispatches through four times during
   initialisation.
3. Transcribing costs about a page of C, and the result can be tested on the
   host against a model of the block, which a ROM call cannot be.

## The register map

Offsets from `0x40003000`. The shape is recognisably an ST-style SDIO
peripheral with the registers moved around — argument and command are
swapped relative to an STM32, the status is at `+0x34` as it is there, and
the FIFO is a single port at `+0x200` rather than a 32-word window.

| Offset | Name | What, and where it comes from |
|---|---|---|
| `+0x00` | CTRL | bits 0..2 set together at reset; bit 2 self-clears (ROM `0x3D88`). Bits 31:30 are configuration; bit 25 and bit 3 are touched around transfers (`0x4734`, `0x4668`). Individually `[?]` |
| `+0x04` | CMD | the command word; bit 31 starts it and reads back as busy |
| `+0x08` | ARG | the argument |
| `+0x0C` | BLKSIZE | 512 in every use in the dump |
| `+0x10` | DLEN | byte count for the data phase |
| `+0x14` | DTIMER | data timeout; `0xFFFFFF40` after clock setup |
| `+0x2C` | DCTRL | bit 3 enables the data phase, bit 14 marks multi-block |
| `+0x34` | STA | status, write-1-to-clear |
| `+0x38`..`+0x44` | RESP0..3 | `+0x44` holds bits [127:96] — the MSB |
| `+0x48` | STA2 | bit 2 receive FIFO empty, bit 3 transmit FIFO full, bits [16:11] echo the answered command index |
| `+0x4C` | CLKCR | clock control; fields below |
| `+0x64` | `[?]` | written `0xFFF` by the bootloader, `0xFFFFFF` by the ROM |
| `+0x80` | DMACTL | the DMA path's; unused by this driver |
| `+0x100` | `[?]` | bit 1 set during clock setup |
| `+0x200` | FIFO | the data port, one word per access |

**STA bits**, each one a bit the ROM tests or clears by name:

| Bit | Meaning |
|---|---|
| 2 | command answered — the bit everything waits on |
| 5 | eight or more words in the receive FIFO |
| 6 | response CRC failed → the ROM's error 1 |
| 8 | no response → error 3 |
| 8,9 | tested as a pair for "some error" → error 32 |

The ROM's clear-everything mask is `0xBFC6`, which is the union of every bit
it ever acknowledges.

**CLKCR** takes exactly two values in the whole dump, and both the mask ROM
and the bootloader use the same pair:

```
identification   0x20070008     divider field [22:16] = 7
run              0x1003000C     divider field [22:16] = 3
```

Bits [30:23] hold one of two bits, `0x20000000` while identifying and
`0x10000000` afterwards. `[?]` what they select — a clock source, most
likely, but the values are transcribed, not derived.

## The bring-up

From the mask ROM at `0x0003D3A0`, which is the one place in the dump that
does all of it in one function:

```
pad_configure(0x00016110)      bank 1 pin 12, function 2, drive 1, no pull
pad_configure(0x0001711B)      bank 1 pin 14, function 2, drive 1, pull 11
pad_configure(0x0001691B)      bank 1 pin 13, function 2, drive 1, pull 11
module_clock_enable(36)        the registers
delay
romclk_enable(17)              0x40080080 bit 0 - the functional clock
install IRQ 44                 not used here; this driver polls
HAL_SD_Init(...)
```

Three pads is a **one-bit bus** — clock, command, one data line. Pin 12
carries no pull and the other two carry pull selector 11, which is the
difference you would expect between a clock and the two lines that need a
pull-up, so pin 12 is the clock `[I]` and 13/14 are CMD and DAT0 in an order
nothing in the dump establishes. The same three ids appear again with
function 15 at ROM `0x0003D2F0`, which is the routine that parks them, so
function 15 is `[I]` the off state.

The bootloader adds one thing the ROM does not: it pulses module 36 **off**,
waits 5 ms, and turns it back on (`0x008227D4`), which is a reset for a block
that may already have been in use. The driver does that too, because a
payload arrives at a chip the boot ROM has been running on.

The two clock ids are worth stating plainly next to §15b's mechanism and
§7n's: **module id 36 gives the block its registers, ROM clock id 17 gives it
its function**, and they are from the two different clock families this chip
has. Neither is `0x400E0000`, which is the third mechanism and the one the
camera and the PWM both turned out to need.

## The command word table

ROM `0x40F4` maps an index to a 16-bit word. It is not `0x2540 | index`:

| Command | Word | | Command | Word |
|---|---|---|---|---|
| CMD0 | `0xA400` | | CMD13 | `0x254D` |
| CMD1 | `0x2441` | | CMD16 | `0x2550` |
| CMD2 | `0x25C2` | | CMD17 | `0x2351` |
| CMD3 | `0x2543` | | CMD18 | `0x2352` / `0x3352` |
| CMD6 | `0x2546` | | CMD24 | `0x2758` |
| CMD7 | `0x2547` | | CMD25 | `0x2759` / `0x3759` |
| CMD8 | `0x2548` | | ACMD41 | `0x2569` |
| CMD9 | `0x25C9` | | CMD55 | `0x2577` |
| CMD12 | `0x454C` | | | |

Reading the fields off the table rather than out of a document — so all `[I]`:

- `[5:0]` the index; bit 6 set for everything except CMD0, so `[I]` "expect a
  response";
- bit 7 for CMD2 and CMD9 alone — the two 136-bit responses;
- bit 9 for CMD17/18/24/25 — the four commands with a data phase;
- bit 10 clear only for CMD17 and CMD18, so `[I]` the data direction;
- bit 12 in the second form of CMD18 and CMD25 — multi-block;
- bit 13 for everything but CMD12, which has bit 14 instead;
- bit 15 for CMD0 alone — `[I]` no response expected.

Bit 8 is set for everything except CMD0 and CMD1, which reads like "check the
response CRC" until you notice that ACMD41 answers R3 exactly as CMD1 does
and has the bit set. That one stays `[?]`.

## The data path is polled, and that is the ROM's own choice

The bootloader's disk layer uses DMA and interrupts. The mask ROM has both,
and its polled path is complete:

```
DCTRL |= 8                      enable the data phase
DTIMER, BLKSIZE = 512, DLEN     ROM 0x4680
CMD17 with the address
wait for the response
loop until 512 bytes:
    if (STA & 0x300) fail
    wait for STA bit 5          eight words are ready
    read eight words from +0x200, each after STA2 bit 2 goes clear
    STA = bit 5                 the acknowledgement that asks for eight more
```

Eight words per acknowledgement is not an optimisation to be flattened into a
one-word loop: the status bit says eight words are there, and the write that
clears it is what asks for the next eight. `tests/host/test_sd.c` asserts the
count.

Addresses: everything except a high-capacity card is addressed in **bytes**,
so the driver shifts the block number left by nine for SDSC and passes it
through for SDHC — ROM `0x4790`, and the single easiest thing in the whole
driver to get backwards.

## [M] What the first hardware run said, 2026-08-13

`examples/SdProbe` on a P20 Player, no card in the slot. This is the **cold**
dump — before this framework's driver touched anything:

```
module 36 reads ENABLED
CTRL   0x40000000      CMD    0x0000A400
DTIMER 0xFFFFFF40      DCTRL  0x00000000
STA    0x00000000      STA2   0x00000106
CLKCR  0x20070008
result: 3 (no response)      -- CMD0 never completed
```

Every one of those is a value this document predicts: `CTRL` is what ROM
`0x3D9A` leaves, `CLKCR` is the identification value with divider 7, `DTIMER`
is ROM `0x3E22`'s constant, and **`CMD` holds `0xA400` — the CMD0 word, with
the start bit cleared**. The mask ROM brought the block up itself at boot,
sent CMD0, waited out its poll and gave up. The driver then repeated the
sequence and landed in the same state, which is why the after-bring-up dump
is identical.

- **The decode is confirmed.** Five registers, five predicted values. Same
  class of evidence as the register file's rails decoding to 2.8 V.
- **The block is configured and not running**, which is where the PWM and the
  camera front end are.

⚠ **One thing that dump does *not* establish**, and was claimed here for a
day: that the *mask ROM* left those values. Uploading a payload does not reset
the chip — the boot ROM keeps running and drops the new image into SRAM — so
an earlier payload in the same power session leaves an identical fingerprint,
because the payload runs the ROM's sequence. It is settled by one power cycle;
see "the power-cycle test" below.

## [M] `0x400E0000` is seven bits wide, 2026-08-13

`examples/SdWall`, CMD0 as the witness:

```
0x400E0000 = 0x00000020     after the group bring-up
bits 0,1,2,3,4,6 tried      no CMD0 completed
bits 7..31                  DO NOT HOLD
```

That register implements bits [6:0] and nothing above them. Four are
attributed (0 Bluetooth, 1 the `0x400E2300` cluster, 5 the group's own,
6 camera), and the walk tried all seven. **The SD host has no functional-clock
bit there** — this chip's usual answer eliminated rather than untested.

It also shrinks an open question elsewhere: every walk of this register so
far, `examples/AudioWall`'s included, has been walking 25 bits that do not
exist. The unattributed ones are **2, 3 and 4**.

The bootloader's four differences from the ROM's bring-up were tried in the
same run — `CTRL[31:30] = 0xC0000000`, the run-speed CLKCR, both, and clearing
bit 0 of `+0x18` — and none completed a command either.

## The clock, and what the ROM leaves unset

ROM clock id 17 is `0x40080080`, and the ROM's own accessors decode all of it:

| Field | Meaning | Where |
|---|---|---|
| bit 0 | enable | `0x223C` sets, `0x2614` clears |
| bit 4 | source select: set → source 8, clear → source 59 | `0x372E` |
| bit 8 | divide by 8 | `0x2D64` |
| bits [19:16] | divider *n* | `0x2A2A` |

Total divide is `(n + 1) × (bit8 ? 8 : 1)`.

**The SD bring-up sets bit 0 and nothing else.** No divider is ever programmed
for this clock, and the one call to the source selector passes 10 — a value
the dispatcher's id-17 case does not recognise, since it only tests 8 and 59 —
so it writes nothing. The clock runs on reset defaults, which is safe for code
running at power-on and not obviously safe for a payload arriving in
bootloader mode.

```sh
make SKETCH=examples/SdClock RUN_MODE=poll run
```

sweeps all 64 settings of that register, then CLKCR's own divider field, with
CMD0 as the witness.

## [M] The clock is not it either, 2026-08-13

`examples/SdClock`. The clock as found — nobody had recorded these:

```
0x40080080 = 0x00000001    enable on, source 59, no prescale, divider 0
                           -> total divide 1, the fastest setting there is
CRU +0x40 = 0x00000009     +0x48 = 0x00000051     +0x60 = 0x00000002
```

All 64 settings of that register were swept — both sources, prescale on and
off, dividers 0..15 — plus twelve CLKCR configurations across both of its
`[30:23]` values. **Nothing completed a command.**

## The lever that is left: CTRL's enable bits

The useful line in that same run:

```
CTRL <- 0x40000003 (no reset bit)  reads 0x40000002
```

**Bit 1 holds; bit 0 does not.** After a full bring-up the register reads
`0x40000000`, so bit 1 does not survive the bring-up either. And the bring-up
is ROM `0x3D88`, the only reset in the dump:

```
CTRL |= 7                  bits 0, 1 and 2 in one store
while (CTRL & 4) ;         wait for bit 2 to clear
```

If bit 2 resets the block it takes bits 0 and 1 with it — so the vendor sets
two bits and immediately wipes them, in both of its bring-ups, and this
driver's transcription reproduces the order faithfully. **Nothing has ever set
those bits after the reset**, and one of them is now known to hold if you do.

```sh
make SKETCH=examples/SdCtrl RUN_MODE=poll run
```

reports which CTRL bits are writable at all, then sixteen combinations of the
four bits the ROM ever touches (0, 1, 3, 25) applied after bring-up, then
every other writable bit alone. Each attempt reports three things, not one:
what CTRL read back, **whether the command's start bit was ever seen
latched**, and the status. "The start bit did not latch" and "it latched,
cleared, and the status stayed empty" are different faults, and only the
second one is about clocks.

The hypothesis can still be wrong three ways, all of which that sweep
answers: bit 0 may be a self-clearing kick that already did its job; `[1:0]`
may be a two-bit field where 3 is not legal; or the order may be right and
the block disabled for an unrelated reason.

*Method note.* That line was printed by a check written as `(ctrl & 3)` with
the message "they stick" — true of a register where only one of the two did,
and it hid the finding until the hex was read by eye. A probe that collapses
two bits into one boolean can only tell you what you already expected. Both
sketches now print the bits separately.

## [M] The command register is live, 2026-08-13

`examples/SdCtrl`, thirty-odd configurations of CTRL, every line identical:

```
start LATCHED   STA 0x00000000   CMD 0x0000A400
```

**The start bit is taken and has cleared by the time the poll gives up.** The
block accepts the command, drops busy, and reports nothing. It is not gated,
not stalled on a clock, not refusing.

The run also mapped CTRL: **writable bits are 1, 3, 4, 24, 25** (plus the
`[31:30]` configuration field), and **bit 0 is not writable at all** — which
retires the hypothesis the sketch was built on, since there is no bit 0 for a
reset to wipe. A bare reset leaves CTRL at `0x00000000`, so it clears bit 30
as well.

Three hypotheses are now closed by measurement:

| Ruled out | By | How completely |
|---|---|---|
| a functional-clock bit in `0x400E0000` | `SdWall` | the register is seven bits wide; all seven tried |
| the clock into the block | `SdClock` | 64 settings, already enabled and undivided |
| a CTRL enable the reset destroys | `SdCtrl` | every writable bit, before and after the reset |

## Next: map the block instead of guessing at it

```sh
make SKETCH=examples/SdScope RUN_MODE=poll run
```

No hypothesis. Snapshot all 72 registers, send one CMD0, snapshot again,
print what moved — the method that found the DVP's DMA destination register.
Plus 32 back-to-back samples of the status and command registers with no
formatting in between (a bit that self-clears in a few cycles is invisible to
a poll loop that prints as it goes), and a sweep of the five CLKCR bits **no
bring-up in the dump ever writes**: the vendor's read-modify-write clears
`0x7FFF0FFF`, so bits 12..15 and 31 have never been set by anything.

**Run it twice — once with a card in the slot, once without.** CMD0 needs no
card in principle, but a host with a card-detect input may decline to run its
clock into a slot it knows is empty, and every run so far has been
uncontrolled for this. The card-detect pin is unknown, so it cannot be
checked in software; two runs and a microSD check it in a minute.

## [M] It discards commands in under a microsecond, 2026-08-13

`examples/SdScope`: 72 registers snapshotted, one CMD0, snapshotted again.

```
bring-up moved:   +0x004  0x0000A400 -> 0x00008020
the command moved:+0x004  0x00008020 -> 0x0000A400
32 samples after: STA 0, and CMD's busy bit already clear in the first one
CLKCR 12,13,14,15,31: all DROPPED - those bits do not exist
```

**One register moved, and it was the one that was written.** `SdCtrl` read CMD
one access earlier and saw the start bit; both are true, so the busy bit lasts
**under one register access, ~300 ns**. Shifting 48 bits at divider 7 takes
tens of microseconds. The block is discarding the command, not working slowly.

## The question that has not been asked yet

Every register read so far lives on the *register interface* — clocked by
module 36, demonstrably working, writes stick. **Nothing measured says whether
the logic behind those registers is running.** A peripheral with a clocked bus
interface and a stopped core looks exactly like this.

```sh
make SKETCH=examples/SdLife RUN_MODE=poll run
```

Three tests, none of which mentions SD:

1. **Sample all 72 registers 200 times**, report any word that ever differs
   from itself. Counters, FIFO levels, state machines all show up.
2. **Push words into the FIFO at `+0x200`** and watch the flags in `+0x48`.
   No card, no command, no bus to the slot. Flags that move mean the core is
   alive and the command path is the fault; flags that do not mean the core is
   stopped. This is the sharpest test available.
3. **The six unused bits of CLKCR `[30:23]`** — the vendor uses only 29 and 28.

If the core is stopped with module 36 and ROM clock 17 both on, the remaining
candidate is a third clock, and the CRU's own divider registers `+0x40` and
`+0x48` (reading `0x09` and `0x51`) are where to look — nothing in this
project has ever written them.

It also paces its own output. Four runs lost their register dump to
`[lost output - device outran the poll rate]`: the console is a 2 KB ring that
overwrites rather than blocking, and in `RUN_MODE=poll` the host drains it only
between `loop()` calls. `SdLife` emits one section per call and waits for the
ring to empty, the way `examples/BtProbe` does.

## [M] The core is clocked — the FIFO works, 2026-08-13

`examples/SdLife`, and the first positive result this block has produced:

```
STA2 before 0x00000106     rx-empty 1  tx-full 0
pushed 16 words, STA2 now 0x00200109      *** THE FLAGS MOVED ***
nothing moved on its own across 200 passes over 72 registers
CLKCR [30:23]: all eight bits hold; none starts a command
```

| `+0x48` bit | before | after | reading |
|---|---|---|---|
| 0 / 1 | 0 / 1 | 1 / 0 | a complementary pair, a second view of fullness |
| 2 | 1 | 0 | FIFO empty — clears once there is data |
| 3 | 0 | 1 | FIFO full — sets at exactly 16 words |
| 21 | 0 | 1 | a level field in the high half |

**The FIFO is sixteen words deep, its flags respond, and the datapath has a
clock.** That retires the "the core is stopped" idea five sketches had been
circling. Reading the FIFO back returned zeros, so the read port is not the
write port — separate directions, or reads are only served during a data
phase. Nothing moving on its own is consistent, not contradictory: with no
transfer running there is nothing to count.

The position is now precise: **the register interface works, the datapath
works, and the command state machine discards commands in under 300 ns.**

## Next: is the card clock on a pin?

A command shifts out on the *card* clock; the FIFO runs on the core clock, and
only the second is proven. The card clock is observable from software, because
the pad controller's input register reads a pad's physical level **whatever
function it is muxed to** (§7f, bank base `+0x010`). A pin carrying a clock,
sampled a few thousand times, comes back as a mix of ones and zeros; a parked
pin comes back constant.

```sh
make SKETCH=examples/SdPads RUN_MODE=poll run       # then again with a card
```

Samples all of bank 1 four ways — before the pads are configured, controller
up and idle, while CMD0 is issued repeatedly, and with pins 12/13/14 taken
back as pulled inputs. It answers three things:

- **does pin 12 toggle?** If yes, the clock leaves the chip and the fault is
  on the far side of the pad. If no, the controller is not driving its clock,
  which no register dump could have told us.
- **do pins 13 and 14 sit high** under the ROM's own pull selector, as a CMD
  and a DAT0 line should?
- **which pins change when a card goes in?** Run it twice and diff. Any pin
  that differs is wired to the slot — this board's SD pinout — and a pin that
  changes on insertion and does nothing else is the card-detect contact.
  Neither is known.

## [M] Software is blind to this bus, 2026-08-13

`examples/SdPads` asked whether the card clock toggles on pin 12, by sampling
the pad input register while the pad was in function 2. It reported "stuck
low" — and that means nothing, because **the pad input buffer is switched off
in every function except 0 and 14**. `examples/PadScope` measured that across
eight pads and sixteen functions during the LCD work, and `docs/LCD.md` says
in as many words that no software test can watch such a bus. This one was
written anyway.

What the run does establish: with pins 12/13/14 taken back to function 0 under
the ROM's own pull selector, all three read **high**. The pads exist and their
pull-ups work.

**So whether the SD clock leaves the chip cannot be answered from a payload.**
It needs a probe on the socket. The remaining software-visible questions are
the pad census and card detect, which `examples/PadMap` covers:

```sh
make SKETCH=examples/PadMap RUN_MODE=poll run     # empty slot, then with a card
```

It never drives a pad and never touches one the boot ROM has assigned. For
each **parked** pad — function 15 — it configures a plain input with a pull-up,
reads, with a pull-down, reads, then parks it again and restores its pull. A
pin that follows its pull is floating; one that ignores it is driven from
outside.

The first run of it aimed at the wrong pads and taught the board's pad map
instead: every bank has a contiguous run of function 15 from pin 0 and reads
`0` above it, across all six banks, so **`0` means the pin does not exist and
`15` means a real pad the ROM has parked**. Eighty pads are bonded out; the
ROM assigns six. The notes have the table. A microSD socket's card-detect contact is a switch
to ground, so **a pin that follows the pull with an empty slot and reads low
with a card in it is card detect**, which the driver currently has to
substitute with a pair of command timeouts.

The pull-down comes from the table at ROM `0x0006501C`, which §7f found and
never decoded — selector 4 is the only clean pull-down of the twelve; see the
notes.

## Reproducing the disassembly

Everything in this document that cites a `0x0082xxxx` address comes out of the
HLKJ bootloader, which `tools/sl6806-xref` cannot see: it scans FIRM, and
every `sdio(...)` string in the image is below file `0x10000`.
`tools/sl6806-boot` reaches it —

```sh
tools/sl6806-boot dump.bin                      # the map, and both CRCs
tools/sl6806-boot dump.bin --strings sdio       # strings at run addresses
tools/sl6806-boot dump.bin --xref 0x40003000    # the finding, in one command
tools/sl6806-boot dump.bin 0x00822768 --dis 96  # HAL_sd_disk_init
```

## The power-cycle test

The ROM's SD bring-up tears itself down when it fails: `0x0003D5CC` calls
`0x0003D2F0` on a non-zero return, which parks the three pads on function 15,
disables module 36 and clears `0x40080080` bit 0. So a fresh boot into
bootloader mode with `SdProbe` as the **first** payload run distinguishes
three cases that currently look alike:

| module 36 reads | What it means |
|---|---|
| ENABLED, registers holding `0x20070008`/`0xA400` | the ROM's init ran and returned success — which with no card in the slot it should not have |
| disabled | the ROM tried and tore down, and what was seen on 2026-08-13 was a previous payload's |
| disabled, registers zero | the ROM never touches the SD host in bootloader mode; the block is simply cold |

All three are useful and they are not the same.

One measurement the first probe did not take, and now takes first: whether a
write to this block sticks. Every register already held what the driver was
about to write, so "unchanged" was compatible with a live block *and* with a
gated one dropping writes silently. Two nonces into `+0x08`, read back,
before and after bring-up.

Also open: **CTRL bits 0 and 1 read back zero** even though the ROM's
bring-up and the driver both set them alongside the reset bit. Self-clearing
kick, or not writable — the probe now says which.

## What the probe will tell you

`examples/SdProbe` is read-only and answers five questions in order:

1. do the registers change when the gates open (the `DvpProbe` pattern),
2. does CMD0 complete — it should, even with an empty slot, because it
   expects no response,
3. does a card answer CMD8 and ACMD41,
4. what do the CID and CSD say, and does the capacity come out as a real card
   size,
5. does sector 0 read, twice, identically, with `0x55AA` at the end of it.

Question 4 is the one that cannot be faked. A capacity of 3.9 GiB out of four
words of CSD is the same kind of evidence as the register file's rails
decoding to 2.8 V and 3.3 V: bytes that had no reason to produce a
recognisable number.

## It stopped at question 2 — `examples/SdWall`, and what it ruled out

```sh
make SKETCH=examples/SdWall RUN_MODE=poll run
```

This looked like the third instance of this chip's signature failure — the PWM
(§14a) and the camera front end (§7n) both took a full register
configuration and did not run — and for both of those the answer was a
functional-clock bit in `0x400E0000`, reachable only after
`sl6806_periph_group_begin()` opens the gate in front of it.

The bring-up transcribed here is the mask ROM's own and there is no reason to
doubt the part that is written down — the cold dump above proves the ROM
performs it. What would be missing is a part the vendor's code never needed
to say. `SdWall` walks `0x400E0000`'s 32 bits with "did CMD0 complete" as the
witness, the way `examples/AudioWall` walks it with buffer time, and then
tries the four places where the bootloader's version of this bring-up differs
from the ROM's.

Three readings of its output:

1. **One bit turns it on.** That bit is the SD host's functional clock, and
   it becomes the fifth attributed bit of `0x400E0000`.
2. **The group bring-up alone turns it on** — then it needed the PLL rather
   than a bit, which would matter to the audio block too.
3. **Nothing across 32 bits.** Then `0x400E0000` is not this block's problem
   either, and what is missing is something done at power-on that a payload
   arrives too late for. The slot's supply is the first candidate: the
   register file (§7m) carries five LDO rails and only one of them is
   attributed, and a rail that is off would explain why even the ROM's own
   CMD0 timed out with no card present.

## What is not done

- **Writing.** CMD24 and CMD25 are in the ROM's table and nothing else is
  implemented. Read the slot before you write to it.
- **Four-bit mode.** Needs a fourth pad that the ROM never configures.
  Sending ACMD6 without it would switch the card to wires this chip is not
  driving, and lose it until the next power cycle.
- **Multi-block reads.** CMD18 needs CMD12 to stop it and its own status
  handling. `sl6806_sd_read_blocks()` issues one CMD17 per block instead,
  which is slower and cannot leave the card mid-transfer.
- **Card detect.** Nothing in the dump reads a card-detect bit in this block.
  The bootloader configures a bank-2 pad as an input with a pull
  (`0x0002280B`, at `0x00822310`) next to code that touches the *other*
  storage host, and that is a guess. "No card" is currently a pair of
  timeouts, not a pin.
- **MMC.** The ROM branches to CMD1 when CMD55 times out. This driver reports
  "no card (or an MMC)" and stops, because with nothing ever having answered
  in this slot there would be no way to tell a working transcription of the
  MMC path from a broken one.
