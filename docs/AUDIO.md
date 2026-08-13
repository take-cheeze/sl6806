# Audio: the block at `0x40009000`, found and transcribed

| Layer | State |
|---|---|
| Hardware | **Present and identified.** `0x40009000` is the SoC's audio controller: two DMA directions, a DAC path with two channels, three analogue microphone inputs, three playback and four capture volume channels, and a 128-word coefficient RAM. |
| Clocks | **Known, and both reachable from a payload.** Mask ROM module id 37, and romclk id 19 (`0x40080088` bit 0). Plus a sample-rate PLL at CRU `+0x10`/`+0x14` with two settings: 24.576 MHz and 22.579 MHz. |
| Data path | **Half decoded.** The block's own descriptor registers (`+0x10C`/`+0x108`/`+0x104`) are read out of the vendor and confirmed to hold — but the block does not carry the bytes. The **general DMA controller at `0x40001000`, module clock 33**, does, and this driver has never programmed it. |
| Driver | **Written; the block wakes and does not run.** [`cores/sl6806/sl6806_audio.c`](../cores/sl6806/sl6806_audio.c), 93 host tests, four examples. |
| Confirmed on hardware | **Yes, 2026-08-13.** Cold every register reads flat zero; after module 37 and romclk 19, thirty come up at sensible reset defaults. |
| Sound | **No.** And the reason is now identified rather than guessed: `examples/AudioWall` proved the engine **moves no memory at all** (0 of 64 words in a capture buffer), and the missing piece is the general DMA controller, not the route, the bit clock, or any bit of `0x400E0000` — all 32 of which were walked with no effect. |

## MEASURED, 2026-08-13 — the block is there

`examples/AudioProbe` on a P20 Player. Cold, every register reads
`0x00000000`. After two writes — `sl6806_module_enable(37)` and romclk 19,
which is `0x40080088 |= 1` — thirty of them come up together:

```
+0x018  0x00000200                +0x200  0x02300700   capture control
+0x07C  0x24924924   3-bit = 4    +0x208  0x00000800   capture status
+0x080  0x24924924                +0x20C  0x00800000   capture address
+0x100  0x00000700   pb control   +0x224  0x00010801   capture ch0, bit 16 SET
+0x108  0x00000800   pb status    +0x228  0x0000A000   capture ch0 level = 0
+0x10C  0x00800000   pb address   +0x254/8, +0x284/8, +0x2B4/8   ch1..ch3, same
+0x134  0x00010801   pb ch0, bit 16 SET
+0x138  0x0000A1FF   pb ch0 level = 0x1FF
+0x154/8, +0x174/8   ch1, ch2, identical
```

**No PLL, nothing near `0x400E0000`.** Two clock writes and the block wakes.
Three things this settles:

1. **It is the audio controller.** Playback levels reset to `0x1FF` — the
   maximum — and capture levels to `0`. A DAC comes up open and an ADC comes
   up shut. That also confirms the TX/RX assignment in this document is the
   right way round.
2. **Bit 16 is the mute**, and it resets *set* on all seven channels. It is
   the one bit the vendor's init clears, and it clears it on exactly the three
   playback channels. That was `[I]`; the reset default makes it `[M]`.
3. **Both buffer-address registers reset to `0x00800000`**, the base of SRAM.
   A DMA address register initialised to the start of memory is about as
   unambiguous as a reset value gets.

Two things read differently from this document's prediction: `+0x108` and
`+0x208` both reset with **bit 11** set, which is neither interrupt flag and
has no name, and `+0x100` resets with `[10:8]` set.

## RETRACTED — "the data path runs" was wrong

`examples/ToneDemo` got `+0x108 = 0x12BC0910` back from a 4796-byte buffer.
`0x12BC` is 4796, the length exactly, and the completion flag set on its own,
five hundred times running. This document called that "the DMA path confirmed
end to end".

`examples/AudioWall` then asked the question that reading could not. It
pre-filled a capture buffer with a pattern, handed it to the RX descriptor,
waited 100 ms and counted:

```
after 100015 us: 0 of 64 words changed
memory untouched - the completion flag is not a transfer
```

**The engine does not move a byte.** So the completion flag is not completion;
a descriptor is being accepted and retired, which is exactly as consistent
with a transfer that never happened. `SL6806_AUD_IRQ_DONE` is probably not
"done" — an error or abort flag raising instantly on a block with no data path
fits every observation better, and the header now marks it `[?]`.

What survives from that run: the length register holds what it is given, and
`+0x10C`/`+0x20C` hold an address. Those are real. The conclusion drawn from
them was not.

Three smaller things the same runs settled, which do stand:

| Register | Written | Read back | Reading |
|---|---|---|---|
| `+0x10C`, `+0x20C` | an SRAM address | the same | both address registers hold |
| `+0x138` | `0x1AA` | `0x800001AA` | the 9-bit level field is right; bit 31 is hardware-set, `[?]` what |
| `+0x104` | `0x00100001` | `0x00100000` | the watermark holds; **bit 0 does not stick** — write-only or self-clearing |

And **bit 11 of `+0x108`** clears when a descriptor is armed and returns
afterwards — an idle flag.

## FOUND — the audio block does not move its own data

`0x00D97ED8`, inside the audio HAL, is the only code in that whole region that
loads `0x40001000`:

```c
/* walk 8 slots of a channel table, claim a free one */
if (!module_clock_is_enabled(33))  module_clock_enable(33);
chan = 0x40001000 + ch * 0x40;
chan[0] &= ~(1 << 30);      /* ... and a dozen more control fields */
```

§7c already had this block: *"`0x40001000`, DMA channel registers, 8 channels
at `+ch*0x40`, `{ctrl, src, dst, len}`"*. **The audio controller is a FIFO
with a request line.** The bytes are carried by the general DMA controller,
which this driver has never programmed and whose module clock — **id 33** —
it has never enabled.

That explains every observation at once: descriptors accepted, memory
untouched, completion instant, and neither the output route nor the bit clock
nor 32 bits of `0x400E0000` changing any of it.

The route setter and the bit clock are still correct transcriptions and still
needed. They were never what was standing in the way.

## MEASURED, third run — the routes take, and the DMA is still not paced

All four routes wrote cleanly (`+0x08` went from `0x00000000` to `0x20832083`
and friends) and every one of them still completed a 25 ms buffer in a **mean
of 10 µs**. 4796 bytes in 10 µs is 480 MB/s on a 64 MHz Cortex-M4 — not a
transfer, a descriptor being retired as fast as it can be read.

That run also had a bug worth recording: the sweep OR'd each route on top of
the last, because the vendor's setter is a read-modify-write and the sketch
never cleared `+0x08` between attempts. Route 1 should have been `0x2043` and
read `0x20C3`. **Only route 0 was actually tested.** Fixed.

### The bit clock, which is the thing that was missing

The route setter is called from the vendor's *init* path. Its **stream start**
is a different routine, `0x00D9662C`, and nothing here had read it:

```c
module_clock_disable(2); delay(10);
module_clock_enable(2);  delay(10);
romclk_set(44, 8);       /* ROM 0x2B1A: 0x40080094[10:8] = 8 - 1 */
romclk_enable(44);       /* 0x40080094 |= 1 */
0x40009400 |= 0x80;
/* and one of: 0x40009080[11:9] = 6, 0x4000907C[26:24] = 6, 0x4000907C[5:3] = 6 */
```

`ROM 0x2B1A` belongs to the mask ROM's **third** clock family — the setter at
`0x289C`, which takes an id and a value, as against the enable-only family at
`0x20EC` that `sl6806_romclk.h` tabulates. Its entire body for id 44 is a
3-bit divider at `[10:8]` of `0x40080094`, written as *value − 1*.

**The arithmetic settles what it is.** The vendor passes 8:

```
24,576,000 / 8 = 3,072,000 = 64 × 48000
22,579,000 / 8 = 2,822,375 = 64 × 44100
```

Both master clocks land on exactly 64 fs — the bit clock for 32-bit stereo
frames. A divider that produces 64 fs from either family is the audio bit
clock and nothing else.

`sl6806_audio_clock_start()` performs it, `begin()` calls it, and `ToneDemo`
sweeps the three source modes against the four routes — twelve combinations,
each timed. One deliberate deviation: the vendor cycles module clock 2 off and
on, this only enables it. Turning off an unidentified module clock from the
code that turns it off is what this codebase has a rule about.

## MEASURED, fourth run — twelve for twelve, and the third functional clock

All twelve combinations, and every register holding exactly what was written:

```
PLL sel  (40080010) = 0x00103060     [6:5] = 3, as written
PLL rat  (40080014) = 0x8C498C00     ratio [30:14] = 0x3126, bit 31, bit 10
bitclk   (40080094) = 0x00000701     divider 8, enabled
DAC      (+0x008)   = 0x20832083 / 0x20432043 / 0x00020002 per route
ctrl     (+0x108)   = 0x12BC0910     the descriptor, every time
```

and **a mean of 10 µs in all twelve**. Nothing varies. The route is not it and
the bit clock is not it either.

This is the third time this project has met the same shape, and
`sl6806_module.h` describes it in its own opening paragraph:

> A module clock gives a peripheral its REGISTERS. It does not give it its
> FUNCTION. That distinction cost this project two peripherals and a lot of
> bench time, and both failures looked identical from the outside.

For the PWM the answer was a clock enable nothing in the vendor's firmware
writes; for the camera it was `0x400E0000` bit 6. **Only four of that
register's 32 bits are attributed**, and it could not be walked from a payload
until `examples/BtProbe` opened its gate on this same day.

`examples/AudioWall` walks it: sets one clear bit, plays four buffers, times
them, restores the register, moves on. It only ever sets bits and never clears
one that was already set, because turning off a functional clock something
else is using — the USB the console rides on, for instance — is the one way
that walk could take the device with it.

It also asks a question a playback-only test cannot: **does the DMA engine
touch memory at all?** A capture buffer is pre-filled with a pattern, handed
to the RX descriptor, and checked. If it changes, the engine really moves data
and only the clock domain is wrong. If it does not, the completion flag is not
a transfer and this document's "the data path runs" needs retracting.

### And the write test was wrong

The same run reported `write test on +0x10C: drops writes - still gated`,
about a block whose thirty registers had visibly just woken up. The test wrote
`0x5A5A5A50` and compared for equality; that is not a valid SRAM address on
this chip, and a DMA address register that ignores or masks bits outside its
own memory map is completely ordinary.

This is `sl6806_pwm.c`'s bug in a new costume — *"writing 0x40 and comparing
the read-back for equality fails on a chip where the block is already
running"*. **A write test is only a test if the value is one the register is
allowed to keep.** `AudioProbe` now writes plausible values to four registers
and prints old/written/read-back for each rather than a verdict, and
`sl6806_audio_begin()` no longer refuses to configure on the strength of it.

This is a further-along document than [BLUETOOTH.md](BLUETOOTH.md) and a less
far-along one than [LCD.md](LCD.md). The block is confirmed and the digital
side runs; what has never happened is a sound.

**Why it was reachable when nothing else is.** Every other unfinished
peripheral on this board — the PWM's counter, the camera's MCLK — is stuck
behind `0x400E0000` and the PLL in front of it (§14a, §15, §7n). Audio is
not: its two clocks are a module id and a romclk id, both of which this
framework already drives, and neither goes anywhere near that wall.

## How the block was found

Not by scanning for base addresses. By following the vendor's own device
names.

The application keeps a device registry with string names, two of which are
`/dev/audio0` and `/dev/audio1` (flash `0x00C7595C` and `0x00C75950`). The
routine that opens them is at `0x00D72418` and logs

```
-audio_driver_open _volume=====%d cfg1.is_headphone:%d
```

Reproduce with:

```sh
tools/sl6806-xref dump.bin 0x00C8EABD          # the open's log string
tools/sl6806-xref dump.bin 0x00C7595C          # "/dev/audio0"
```

Everything under that reaches hardware through a four-entry table of driver
objects at SRAM `0x0082B430`. Entry 1 is the audio stream device; its ops are
installed at `0x00D9A0A4`, and its ioctl at `0x00D9A024` forwards every
command it does not handle itself to `0x00D96824` — a 145-way jump table that
is the codec's whole control surface. Every literal that table loads is in
`0x40009000..0x400092BC`.

Two independent things confirm it before any register is decoded:

**The microphones.** `0x00D3C688` logs `[driver_audio] AMIC0_GAIN set to:%d`
(and AMIC1, AMIC2) and reaches the same dispatcher with commands `0x2D..0x32`.
Three analogue mic inputs is what an MP3 player with a voice recorder has.

**The rate split.** `0x00D979E0` computes `rate % 8000` and uses the result to
choose between two frequencies passed to `0x00807300`:

```
24,576,000 Hz  = 48000 x 512
22,579,000 Hz  ~ 44100 x 512
```

A single modulo that separates the 48 kHz family from the 44.1 kHz one, and
two master clocks at 512 fs, is not something a non-audio peripheral does.

## This corrects §7c: `0x40009000` is not the timers

[`sl6806_re_notes.md`](sl6806_re_notes.md) §7c files `0x40009000` as "timers",
on the evidence of "channels at `0x100` stride (`+0x108`, `+0x208`) with
write-1-clear flags and per-channel callbacks; register triples at `0x20`
stride".

Every word of that description is right and the conclusion is wrong.
`+0x108` and `+0x208` are this block's two DMA directions — playback and
capture. Their write-1-clear flags are the two interrupts each direction
raises. The per-channel callbacks are the four function pointers that the
IRQ 30 handler at SRAM `0x0080D8F8` dispatches to. And the register triples at
`0x20` stride are the three playback volume channels at `+0x134`.

The same section already contains the reason it cannot also be the timers:
*"FIRM's `HAL_timer_*` is at `0x40099000`, a different block"*. Nothing in the
HLKJ bootloader references `0x40009000` at all — that literal scan comes back
empty — so the attribution never had a second source.

## The register map

Addresses are absolute. Provenance as elsewhere: `[V]` read out of the dump,
`[I]` inferred, `[?]` unknown. There is no `[M]` in this document.

| Address | What | Source |
|---|---|---|
| `+0x000` | bits 24 and 25 are the two DAC enables, set separately | `[V]` `0x00D93FB0`, `0x00D93FBC` |
| `+0x008` | the DAC path — two identical 16-bit halves, one per channel | `[V]` `0x00D93F54` / `0x00D94028` |
| `+0x018` | `[9:5]`, role unknown | `[V]` `[?]` |
| `+0x02C` | ADC gain, `[20:16]` | `[V]` `[I]` |
| `+0x030` | mic gains, `[20:16]` and `[28:24]`; bits 3 and 7 | `[V]` `[I]` |
| `+0x03C` | bit 10, armed by the block init | `[V]` `[?]` |
| `+0x07C`, `+0x080` | three-bit fields, roles unknown | `[V]` `[?]` |
| `+0x100` | playback control; bit 0 set by init, `[15:12]` cleared | `[V]` `[?]` |
| `+0x104` | playback trigger: watermark `[31:16]`, bit 0 arms the interrupt | `[V]` |
| `+0x108` | playback control/status: length `[31:16]`, start bit 4, flags 8 and 10 | `[V]` |
| `+0x10C` | playback buffer address | `[V]` |
| `+0x134`+`0x20n` | playback channel *n* mute, bit 16 | `[V]` `[I]` |
| `+0x138`+`0x20n` | playback channel *n* level, `[8:0]` | `[V]` |
| `+0x200`..`+0x20C` | capture, the same four registers one page up | `[V]` |
| `+0x224`+`0x30n` | capture channel *n* mute, bit 16 | `[V]` `[I]` |
| `+0x228`+`0x30n` | capture channel *n* level, `[8:0]` | `[V]` |
| `+0x400`..`+0x5FC` | 128 words the init zeroes one at a time — the hardware EQ's coefficients | `[V]` `[I]` |

The two directions differ in both count and stride — three playback channels
at `0x20`, four capture channels at `0x30` — which is worth knowing before
anyone "unifies" them.

### The DAC path register is the one certain layout

`0x00D93F54` and `0x00D94028` are the same function written twice, once per
channel, and every constant in the second is the first shifted left by 16. So
`+0x08` is two identical halves. Within a half, from the four cases the
function branches on:

```
bits [1:0]    2 for routes 1..3, 3 for routes 1 and 3
bit  6        set for routes 1 and 3
bit  7        set for route 0
bit  13       set for routes 0, 1 and 3
bits [12:8]   a 5-bit field from the driver's own state (+0xA8)
```

"Route" is the byte at driver state `+0x113`, and the strings around it are
`-play status to spk play` and `-audio switch earphone`. So the four cases are
almost certainly speaker / headphone / line / off. **Which number is which is
not known**, and that is the single largest hole in this document.

## The data path

`0x0080D9BC` in the SRAM blob is the whole of it:

```c
play(handle, buf, len):
    if (len & 3) return -1;              /* rejected, not rounded */
    *(u32 *)0x4000910C = buf;
    field(0x40009108, 0xFFFF0000, len);
    *(u32 *)0x40009104 = ((len * 3 / 4) << 16) | 1;
```

and the start is a separate write elsewhere (`0x00D97A5E`):

```c
    *(u32 *)0x40009108 |= 0x10;
```

Capture is `0x0080D964`, identical, using `+0x204`/`+0x208`/`+0x20C`.

The three-quarter point in `+0x104` is the refill interrupt's threshold. The
handler at `0x0080D8F8` reads the control register, writes the same word back
— so the flags are write-1-to-clear — and dispatches bit 8 and bit 10 to two
different callbacks. **Which of the two is the watermark and which is
end-of-buffer is inferred**, from the fact that a three-quarter point exists
to be a warning; if a hardware run shows bit 10 arriving at 75% of a buffer,
the two names in `sl6806_audio.h` are simply swapped.

## The clocks

```
module clock 37     0x00D96754, ROM 0x1EC0 -> 0x1C5C
romclk 19           0x00D9675A, 0x008051EC -> ROM 0x20EC; 0x40080088 bit 0
0x400F70D8 bit 1    0x00D9674A, before either of them
IRQ 30              0x00D967D0..0x00D967E4, priority 2
```

The sample-rate PLL is separate from the `0x40080008` one that the
`0x400Exxxx` group needs, and it is at CRU `+0x10`/`+0x14`:

```c
while (CRU[0x14] & (1 << 11)) ;                     /* busy */
CRU[0x10] = (CRU[0x10] & 0xFC1F0000) | 0x3000 | (select << 5);
CRU[0x14] = (CRU[0x14] & 0x3800) | (1 << 31) | (1 << 10) | (ratio << 14);
```

with `select`/`ratio` of `3`/`0x3126` for the 48 kHz family and `2`/`0x186C2`
for 44.1 kHz. The ratio is 17 bits at `[30:14]`; bit 31 sits above it and is
the enable.

The hardware init at `0x00D94A6C` also borrows module 32, romclk 45 and bit 2
of `0x40000020` for exactly as long as it takes to clear the coefficient RAM,
and gives all three back. `sl6806_audio.c` does not clear that RAM and so does
not take them — but a second clock domain behind the same base address is the
sort of thing that explains a later mystery, so it is recorded.

## What is implemented

[`cores/sl6806/sl6806_audio.h`](../cores/sl6806/sl6806_audio.h) and `.c`:
bring-up in the vendor's order, buffer submit, the flags, volume and mute.
No interrupt handler — a payload shares its vector table with the boot ROM and
installing one is a much larger change than this is worth before anyone has
seen the block move.

73 host tests in `tests/host/test_audio.c` run the driver against a model of
the block and its gate. They cannot tell you the decode is right; what they
hold it to is the sequence — the descriptor order, the watermark arithmetic,
the rate split, the rejections, and a gate test that writes and reads back
rather than believing a plausible read.

## What to run, in order

```sh
make SKETCH=examples/AudioProbe RUN_MODE=poll run    # read-only, then two clocks
make SKETCH=examples/ToneDemo   RUN_MODE=poll run    # 440 Hz at 48 kHz
```

`AudioProbe` is a regression test now — one unit has answered all three of its
passes. **`ToneDemo` is the open one.** It walks the four output routes,
playing about a second on each, and prints the mean completion time per route.
Plug headphones in first.

One gotcha worth knowing: module 37 and romclk 19 **survive a re-upload**, so
`AudioProbe`'s "cold" pass is only cold if the device has been unplugged since
the last run.

Report results the way `examples/RegFileProbe`'s comment block records its
hardware run, so nobody has to repeat it.

## What is still missing

A short list, which is the point of writing this down:

1. **The output route.** `+0x08` has four settings and nobody knows which is
   the speaker and which the headphone jack. Measured: it reset to `0x00000000`
   — "off" — which is why the first `ToneDemo` was silent. `sl6806_audio_route()`
   writes all four now; which one is which is the open question.
2. **Whether the board has an amplifier enable.** The application talks about
   `spk_switch` in its settings partition (§7k). If that is a GPIO, it is not
   in the variant's pad map, and no sound will come out of a speaker without
   it. Headphones may not need it — on this board the headphone lead is also
   the FM aerial, so it is wired to something.
3. **The sample format.** 16-bit stereo is what the vendor's config struct
   looks like it carries; the byte that would settle it comes from a caller
   this analysis did not follow. Wrong pitch or noise points here first.
4. **Which of bits 8 and 10 is which.** Cheap to settle: submit a buffer and
   see which flag arrives first.
5. **The coefficient RAM.** 128 words, a second clock domain, and an
   application string that says `hardware EQ config to %d // 0: None, 1: pop,
   2: rock, ...`. Entirely unexplored, and not needed for a first sound.

Item 1 is now the only thing between here and audio, with item 2 behind it.
Everything else is tuning.

~~Whether the DMA descriptor registers accept writes~~ — settled: they do, and
a whole buffer transfers.
