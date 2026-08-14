# Audio: the block at `0x40009000`, found and transcribed

| Layer | State |
|---|---|
| Hardware | **Present and identified.** `0x40009000` is the SoC's audio controller: two DMA directions, a DAC path with two channels, three analogue microphone inputs, three playback and four capture volume channels, and a 128-word coefficient RAM. |
| Clocks | **Known, and both reachable from a payload.** Mask ROM module id 37, and romclk id 19 (`0x40080088` bit 0). Plus a sample-rate PLL at CRU `+0x10`/`+0x14` with two settings: 24.576 MHz and 22.579 MHz. |
| Data path | **Half decoded.** The block's own descriptor registers (`+0x10C`/`+0x108`/`+0x104`) are read out of the vendor and confirmed to hold what they are given — and no byte ever moves. What actually carries the data is not established. |
| Driver | **Written; the block wakes and does not run.** [`cores/sl6806/sl6806_audio.c`](../cores/sl6806/sl6806_audio.c), 93 host tests, four examples. |
| Confirmed on hardware | **Yes, 2026-08-13.** Cold every register reads flat zero; after module 37 and romclk 19, thirty come up at sensible reset defaults. |
| Sound | **No, and the cause is narrowed but not found.** `examples/AudioWall` proved the engine **moves no memory at all** (0 of 64 words in a capture buffer). Ruled out by measurement: the output route, the bit clock, and every bit of `0x400E0000`. A DMA-controller hypothesis was raised and withdrawn the same hour — see below. |

**Outside corroboration, 2026-08-13.** The chip is very probably Zhuhai
绅聚科技's 云P3 ([notes §1a](sl6806_re_notes.md)), and their announcement of it
advertises 192 kHz/24-bit playback, full-format decode, and ENC-denoised
recording with VAD. That fits the block's shape — two DMA directions, three
microphone inputs, a 128-word coefficient RAM — and it argues the path is meant
to work in production silicon rather than being a fused-off option on this SKU.
It is marketing copy, not a register spec: treat it as one more reason the
failure below is a missing enable rather than missing hardware, and nothing more.

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

`examples/AudioWall` then pre-filled a capture buffer with a pattern, handed
it to the RX descriptor, waited 100 ms and counted:

```
after 100015 us: 0 of 64 words changed
memory untouched - the completion flag is not a transfer
```

**That test is weaker than it was first written up as**, and the weakness is
worth stating before the conclusion. `sl6806_audio_begin()` performs the
vendor's *playback* bring-up; it sets `+0x100` bit 0 and never opens the
capture direction the way the vendor's mode-1 open does. So what the capture
test actually shows is "with playback configured and RX never opened, a
capture descriptor moved nothing" — not "the engine cannot move memory".

**The load-bearing evidence is the timing, and it stands on its own.** 4796
bytes in 10 µs is 480 MB/s:

| rate | 4796 bytes takes |
|---|---|
| 480 MB/s | 10 µs ← measured |
| 100 MB/s | 48 µs |
| 25 MB/s | 192 µs |
| 191 KB/s (real time at 48 kHz) | 25 ms |

480 MB/s is not a transfer a 64 MHz AHB performs. Whatever `+0x108` retires in
10 µs, it is not 4796 bytes of memory. So the completion flag is not
completion — a descriptor is accepted and retired, which is exactly as
consistent with a transfer that never happened. `SL6806_AUD_IRQ_DONE` is probably not
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

## The DMA-controller hypothesis, raised and then disconfirmed

Raised on the strength of `0x00D97ED8` — which claims a DMA channel, enables
module clock 33 and configures `0x40001000 + ch*0x40` — being the only code in
the audio HAL's address region that loads `0x40001000`. That was proximity,
not evidence, and following the calls kills it twice over.

**Every DMA entry point in the image, and who uses it:**

| Entry point | Callers |
|---|---|
| `0x00D97ED8` channel setup | `0x00D3EAC0`, whose literal pool holds `lcdc_dma_write`, `lcdc_set_descriptor`, `lv_lcd_init` |
| `0x0080DA74` start (§14b) | `0x00D998CC` and `0x0080E7FE` — both operate on the object at SRAM `0x0082B3BC` |
| `0x0080DAC8` abort, `0x0080DADE` enable, `0x0080DAFC` remaining | nothing at all |

And `0x0082B3BC` is not a guess: `0x00D99692` is `strd r3, r0, [r2]` with
`r2 = 0x0082B3BC` and `r3 = 0x400D9000`. The object is `{ base = LCDC, cfg }`.

**So the general DMA controller has exactly two call paths in the whole image
and both are the LCD controller. Nothing in the audio path touches it.** That
is a disconfirmation rather than an absence of evidence, and it was reached by
following a base assignment instead of an address range — the method the
earlier version of this section got wrong.

It also says something positive: **the audio block moves its own data.** It
has its own address, length, watermark and start registers, which is not what
a FIFO fed by an external engine looks like. Its own engine is the thing that
is not running.

### What that leaves

Ruled out by measurement: the output route, the bit clock, all 32 bits of
`0x400E0000`. Ruled out by call-graph: the general DMA controller.

The candidate with the most behind it is the **EQ sub-block at `+0x400`**. The
vendor's init takes module clock 32, romclk 45 and bit 2 of `0x40000020`,
clears the 128-word coefficient RAM, and *gives all three back* — and then its
stream start sets `0x40009400` bit 7, which this driver does. If that
sub-block sits in the playback path, it needs those clocks **held** while
streaming rather than borrowed for an init. Nothing in the vendor's code holds
them, but nothing in the vendor's code needed to: the application configures
an EQ preset and the vendor's own path may take them again there.

`sl6806_audio_eq_hold()` does it, and it was run.

## MEASURED — the EQ hold changes the block, in the wrong direction

`ToneDemo` with `-DTONEDEMO_EQ_HOLD=1`. The first thing all session to change
the block's behaviour at all, and every change points away from a fix:

| | before | after |
|---|---|---|
| `+0x400` | `0x00000000` | `0x003378B1` — the sub-block wakes |
| `+0x108` | `0x12BC0910` | `0x00BC1F7A` |
| `+0x008` route 0 | `0x20832083` | `0x00832083` |
| mean completion | 10 µs | 3 µs |

**The two register changes are the same change.** The length field at `+0x108
[31:16]` is written 4796 = `0x12BC` and reads back `0x00BC` — the top byte of
the register stopped holding. `+0x008`'s channel-1 half lost bit 13, which is
register bit 29 — also the top byte. **Bits [31:24] stopped accepting writes
across two registers**, and a length truncated from 4796 to 188 bytes is
exactly why the transfer got *faster*.

So the EQ hypothesis is dead: holding those enables makes it worse, and the
vendor borrows them briefly for a reason. But something in there moves the
register window, and that is the only lever the block has responded to at all.

## DECODED — bit 2 of `0x40000020` is a register-window switch

`examples/AudioWindow`, all eight combinations:

```
m32 rc45 pad2 |  length reads  |  +0x400
 0   0    0   |  0x12BC ok     |  0x80
 0   0    1   |  0xBC  TRUNC   |  0x3378B1
 0   1    0   |  0x12BC ok     |  0x80
 0   1    1   |  0xBC  TRUNC   |  0x3378B1
 1   0    0   |  0x12BC ok     |  0x80
 1   0    1   |  0xBC  TRUNC   |  0x3378B1
 1   1    0   |  0x12BC ok     |  0x80
 1   1    1   |  0xBC  TRUNC   |  0x3378B1
```

**And that table was wrong.** A second run, from a known state:

```
 0   0    1   |  0x12BC ok     |  0x80        <- differs
 0   1    1   |  0x12BC ok     |  0x80        <- differs
 1   0    1   |  0xBC  TRUNC   |  0x3378B1
 1   1    1   |  0xBC  TRUNC   |  0x3378B1
```

**It takes module clock 32 *and* the pad-mux bit, together.** The first run had
module 32 silently already on — inherited from the `ToneDemo` EQ build, because
module clocks survive a re-upload — and its `m32=0` rows never turned it off,
so they measured `m32=1` while claiming otherwise. romclk 45 genuinely does
nothing.

`0x40000020` is the pad/pin function mux (§7c), so this is a register-window
switch and not a clock, which is why none of the clock analysis was going to
find it. It is still the first outright decode in five runs rather than another
elimination — it just needed two runs to get right.

With the switch **clear**, `+0x400` reads `0x80` — the bit
`sl6806_audio_clock_start()` writes there, so it is an ordinary register
holding what it was given. With it **set**, `+0x400` reads `0x3378B1` and the
top byte of `+0x108` and `+0x008` stops accepting writes. That is exactly why
the vendor's init takes the bit for as long as it takes to clear
`+0x400..+0x5FC` and then gives it straight back: it is how you reach the
coefficient RAM, and while you hold it the ordinary registers are not all
there.

**Both halves survive a re-upload**, and that is now the third time inherited
state has produced a confident wrong reading in this block — the first two
being the "cold" pass that wasn't and the length field truncated before
`ToneDemo` wrote anything. A sketch that inherits the window open has the top
byte of its length field silently dropped.

`sl6806_audio_begin()` closes both halves before doing anything else,
`sl6806_audio_coeff_window()` moves them together, and `AudioWindow` prints
the state it starts from rather than assuming one. **Establish state; do not
inherit it** is now a rule in this block, alongside "a completion flag is not
a transfer" and "a literal's address region is not its owner".

### It is a 24-bit RAM over the whole aperture, not a narrowing

`AudioWindow`'s second half read all 61 mapped registers in both windows.
**Every one differs**, including those that read `0` with the window closed,
and **every switched value fits in 24 bits** (largest `0xFB6E49`).

Two checks say what it is:

| offset | closed | open | |
|---|---|---|---|
| `+0x008` | `0x20832083` | `0x00832083` | = closed `& 0xFFFFFF` |
| `+0x108` | `0x12BC0800` | `0x00BC1F6A` | `0x12BC0800 & 0xFFFFFF = 0xBC0800`, so the length reads `0xBC` |

So "the length field truncates to 8 bits" was never a narrowed field. **The
whole aperture is replaced by a memory 24 bits wide**; a read-modify-write of
the length hits RAM, and `0x12BC << 16` does not fit in 24 bits.

And the fingerprint that settles it: **`+0x10C` reads `0x00827A24` with the
window open** — the buffer address `ToneDemo` printed in the EQ-hold run
(`buffer at 0x827A24`). `+0x10C` is exactly where `sl6806_audio_play()` writes
that address. The write went into the window and is still there, sessions
later. That is memory, and it kept what it was given.

**This voids the EQ-hold run entirely.** Every register write it made — the
routes, the DAC enables, the descriptor — went into this RAM instead of the
block. That run measured nothing about the audio path; its "3 µs" is the time
to retire a descriptor the block never saw. Its only real content is the
evidence above.

24 bits is what an audio DSP's coefficient memory is, which fits the vendor
clearing `+0x400..+0x5FC` through this window and nothing else. The aperture is
wider than those 128 words though — `0x000..0x5FC` all read distinct values,
with no aliasing at `0x200` or `0x400`.

### What it opens

The question was whether a control register nobody has seen hides behind the
switch — the right shape of thing to explain a block that accepts every
descriptor and moves no data.

**Answered: no.** All 61 registers differ because the aperture *is* memory
while the switch is open, not a second set of registers. There is nothing
hidden there.

So the window question is closed and audio's is not. Ruled out so far: the
output route, the bit clock, all 32 bits of `0x400E0000`, the general DMA
controller, the vendor's system clock init, and now the coefficient window.
The block accepts a descriptor and retires it in 10 µs having moved nothing,
with every register holding exactly what the vendor's own code puts there.

That needed `sl6806_module_disable()` (ROM `0x1CE8`, whose order is the
reverse of the enable's: gate first, then shadow). It is the dangerous
direction, so the sketch only ever switches off an id it switched on.

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

## The 10 µs needs auditing before anything else is swept

Written 2026-08-14, straight after the PWM investigation closed, because that
investigation ended by finding that four separate "clean negatives" were one
instrument reporting its own limit — and this document's central negative has
the same shape.

The claim is that a 4796-byte descriptor retires in a mean of 10 µs, which
would be 480 MB/s, which a 64 MHz AHB cannot do, so nothing is transferred.
**The reasoning is sound if and only if the 10 µs is a measurement.** Here is
how it is taken, in `examples/AudioWall`:

```c
sl6806_audio_play(...);
start = micros();
while (micros() - start < 100000)
    if (sl6806_audio_done())
        return micros() - start;
```

When the flag is already up, what comes back is the cost of **one iteration**
of that loop — a `micros()`, an MMIO read, a write-1-to-clear write, another
`micros()`. At the 312 ns per register access measured for the LCDC, one
iteration being of the order of 10 µs is entirely plausible. If it is, then
"10 µs" does not mean 480 MB/s. It means *the flag was already set the first
time anything looked*, and it carries no timing information at all.

And the tell is already in this document, three sections up: **"a mean of
10 µs in all twelve. Nothing varies."** An observable that does not vary
across a twelve-way sweep is usually a broken instrument, not a discovery.
That exact sentence, in the form `commits 0/4` across 168 configurations, cost
the PWM work four rounds — and the PWM counter had been running the whole
time. See `docs/sl6806_re_notes.md` §32.

### What `examples/AudioLen` checks, in order

1. **The instrument's floor.** The same poll loop against a condition that is
   never true. Every audio timing number this project has quoted has to be
   read against that figure.

2. **Whether the flag is stale.** Read `TX_CTRL` before starting anything;
   write the acknowledgement; read back. Nothing has ever verified that
   `SL6806_AUD_IRQ_DONE` clears — if it is not write-1-to-clear, every
   completion reading taken so far is void, and "retires instantly" is just a
   bit that was already up.

3. **Whether completion time scales with length.** This is the control the
   question turns on, and it has never been run: the sweeps so far varied
   routes, clock sources and all 32 bits of `0x400E0000` while holding the
   length at 4796 bytes throughout. `AudioLen` spans 128 bytes to 64000 — a
   500:1 range.

   | Result | Meaning |
   |---|---|
   | time rises with length | data **is** moving; only the rate is wrong, and this document's central negative is void |
   | time flat across 500:1 | nothing moves, and the negative stands for a better reason than it had |

That is the same control that settled the PWM — halve the period and the time
must halve. Here: double the length and the time must double. It is a stronger
test than any register sweep, because no configuration bit can fake a
proportionality.

### [M] First run — the two tidy explanations are dead, and the control did not run

**1. The 10 µs is a real reading.** The poll loop's floor is **1.73 µs per
iteration**, so `AudioWall`'s 10 µs is about six iterations — comfortably
above it. The completion is genuinely fast; it is not the loop reporting its
own cost. The argument from 480 MB/s stands as far as it goes.

**2. The flag is not stale.** `TX_CTRL` reads `0x00000800` before anything
starts, and `SL6806_AUD_IRQ_DONE` is bit 10 — **clear**. So the descriptor is
not retiring against a bit that was already up.

That run also turned up a small new register fact: **bit 11 is set and
survives two write-backs**, so it is a status bit rather than write-1-to-clear,
and it is the first thing known about what else lives in that word. Worth a
census later.

**3. The length control did not run.** All eight rows printed `us 0, iters 0`,
and `iters == 0` is reachable only on the early-return path — so
`sl6806_audio_play()` rejected every call. Its guard is

```c
if (!buf || !len || (len & 3u) || (addr & 3u) || len > 0xFFFFu) return -1;
```

Every length passes. The rejection was `(addr & 3u)`: `static int16_t wave[]`
is only **2-byte aligned**, and the buffer landed on an odd word boundary.

[!] And note what that column looked like: eight rows of zeros, in a test
whose stated criterion was *"flat µs across a 500:1 span of length = nothing
moves."* A rejected row and a flat row are indistinguishable in the reported
value. The only reason this was caught is that the raw `iters` counter was
printed beside the derived number — which is the same thing that caught the
void `PwmMode` sweep, and the argument for printing raw state rather than
verdicts.

Fixed: the buffer is `__attribute__((aligned(4)))`, the buffer address and its
alignment are printed, and a rejected row now prints `REJECTED ... not a
measurement` instead of a zero. **A test must not be able to report a failure
to run as a result.**

So the question is exactly where it was, minus two explanations, and the
length sweep still has to be run.

### [M] RETRACTED — the DMA moves data. The central negative was one length, measured many ways.

`examples/AudioLen`, with the buffer word-aligned so the rows actually ran:

```
 len      us   iters
   128     3      1
  2048     5      2
  4796    10      5
  8192    17      8
 16384    32     16
 64000   125     64
```

Linear fit: **`us = 0.001926 × len + 1.43`** — about 519 bytes/µs with a fixed
1.4 µs of overhead, negligible residuals, across a 500:1 span of length.

**A descriptor that is retired without doing anything takes constant time.**
This does not. So something proportional to the data is happening, and this
document's central claim — *"the block accepts a descriptor and retires it in
10 µs having moved nothing"* — is **void**. Every sweep behind it varied
routes, clock sources and all 32 bits of `0x400E0000` while holding the length
at 4796 bytes, so the one variable that could have exposed it was never moved.

The 480 MB/s impossibility argument also needs correcting: it was computed
against a 64 MHz core, and §30 of the RE notes measured the PLL already
running at **192 MHz**. ~130 M words/s is about two thirds of a word per cycle
on such a bus — an ordinary DMA rate, not an impossible one.

**What is true instead is more useful than the negative was.** The DMA runs at
bus speed and *nothing paces it at the sample rate*: real time for 48 kHz
stereo 16-bit is 192 KB/s, the measured rate is 2667× that, and the ratio is
constant at every length. That is a DMA draining into a sink which never
back-pressures.

One anomaly kept rather than dropped: **length 32768 timed out** (60 ms,
30968 iterations) while 64000 completed in 125 µs. `0x8000` is exactly bit 15
of the 16-bit length field. Unexplained.

### [V] The audio master clock the vendor starts, and this framework never has

`sl6806_audio_begin()` does module 37, romclk 19 and the bit clock. The
vendor's own path at `0x00D9A224` does all of the following *first*, and none
of it appears anywhere in `cores/`:

```
romclk_enable(0)
0x00807300(24576000)          <- an audio PLL
module_enable(87)
romclk_enable(31)
romclk_enable(56)
0x4009B04C = (v & ~0x0F000000) | 0x0E000000
0x4009B050 = 27
0x4009B040 |= 0x80000000 | 1
```

`0x00807300` is a **second PLL**, distinct from the `0x40080008` one that
`sl6806_pwm.h` documents, and its multiplier is chosen by the audio rate:

| freq | mult | mode |
|---|---|---|
| 24576000 | `0x3126` | 3 |
| otherwise (`0x01588738`) | `0x186C2` | 2 |

```
poll 0x40080014 bit 11 until clear
0x40080010 = (v & ~0xFC1F0000) | 0x3000 | (mode << 5)
0x40080014 = (v & 0x3800) | 0x80000000 | 0x400 | (mult << 14)
```

A multiplier selected by sample rate is an audio PLL by construction.
**`0x4009B000` is a block this project has never named**, and module 87,
romclk 31 and romclk 56 have never been attributed. There is also a fallback
at `0x00D9A1FE` — `clk_setsrc(57, 42)` — taken when the PLL flag is clear.

This is the same shape as the PWM's romclk 39: an entire clock chain sitting
in XIP that the driver never performs, on a block that is otherwise fully
configured.

### What `examples/AudioPll` runs

The vendor's chain, one step per `loop()` call with each step announced before
it happens, then the length sweep again.

| Result | Meaning |
|---|---|
| completion moves toward 25 ms for 4796 bytes | the DAC now paces the DMA — audio is running |
| completion stays at ~10 µs | this chain is not the pacing, and the question becomes what consumes the FIFO |

The linearity should persist either way; what changes is the slope, by a
factor of 2667. There is no way to misread that, which is the point after five
rounds of ambiguous eyeballing on the backlight.

### [M] The vendor's clock chain is fully applied, and it is not the pacing

`examples/AudioPll`. The chain was performed step by step with readback, and
the length sweep run before and after. **Nothing changed** — 2048 to 64000
bytes, ~520 B/µs, 2667× real time, identical either side.

What each step actually did:

| Step | Result |
|---|---|
| `romclk_enable(0)` | set |
| audio PLL `0x00807300(24576000)` | **already programmed — see below** |
| `module_enable(87)` | acked |
| `romclk_enable(31)` | **a no-op in the ROM** — id 31 dispatches to `0x24BE`, a bare `bx lr`. The generated table is right to omit it; the vendor calls a dead id. |
| `romclk_enable(56)` | set |
| `0x4009B04C/50/40` | took: `0x0E900000`, `0x1B`, `0x80000001` |

**The audio PLL is already running at exactly 24.576 MHz.** `0x40080014`
reads `0x8C498C00`, which is bit-for-bit what `0x00807300` writes for
24576000 — multiplier `0x3126`, bit 31, bit 10, bits [13:11] preserved — and
`0x40080010` reads `0x00103060`, carrying mode 3 and `0x3000` exactly as the
routine sets them. Whatever brings the chip up leaves the audio PLL configured
for the 48 kHz family.

That also explains the `BUSY TIMEOUT`: bit 11 is not a busy flag. It is inside
the `0x3800` field the vendor's write *preserves*, and it currently reads 1.
The vendor's `lsls #20 ; bmi` loop only terminates when the PLL is
unconfigured, i.e. at cold boot before anything sets it. The probe's bounded
version correctly declined to write — and would have written the value that is
already there.

**So this is a clean negative**, and the first in this section that is not an
instrument artifact: the sweep is linear (the instrument works), every step was
applied and confirmed by readback, and the one step that mattered was already
in the target state, verified against the vendor's own formula.

### Where that leaves it

The DMA runs at bus speed. The master clock is up at the right frequency. What
is missing is whatever makes the output stage **consume** samples at the frame
rate, and three things are worth trying in order:

1. **The bit clock's parent.** `sl6806_audio.c` writes divider 8 and the enable
   into `0x40080094` (romclk 44), but nothing has ever established what feeds
   that divider or whether the audio PLL is its source. A derivation the way
   romclk 39's was derived, plus an implemented-bit census of `0x40080094`.
2. **`0x4009B000`.** Three registers were written because the vendor writes
   three. The block has never been censused, named, or read; it sits directly
   in the audio clock path and is the least-explored thing in it.
3. **The output enable inside `0x40009000`.** The DAC path is configured and
   the levels are at maximum, but nothing is known to *start* the serialiser.

The length sweep is now the standing instrument for all three: real time is
1×, and anything that introduces pacing moves the slope by a factor of 2667.

### [!] Correction to the section above — the audio PLL was already ours

Two claims in the preceding section overstate what was new, and the mistake is
the one this project keeps making: publishing a novelty claim without grepping
for the thing first.

**The audio PLL registers were already decoded here.** `sl6806_audio.h` has
carried `SL6806_AUD_PLL_SEL` (`0x40080010`) and `SL6806_AUD_PLL_RATIO`
(`0x40080014`) with the identical `KEEP`/`SET` masks, the identical
`ratio << 14` placement, and the `rate % 8000` family selection, for as long as
this document has existed. `0x00807300` is the routine those constants were
read out of. Calling it "a second PLL, distinct from the `0x40080008` one" is
right; implying it was newly found is not.

**And `sl6806_audio_begin()` already writes it.** `audio_pll_set()` is called
from line 179 of `sl6806_audio.c`. So when `examples/AudioPll` reported the
PLL "already programmed for exactly 24.576 MHz", what it had actually
confirmed is that **the driver's own write, performed seconds earlier in
`setup()`, landed correctly**. That is a useful result — `audio_pll_set()`
demonstrably works, and the `0x8C498C00` readback validates the whole
`KEEP`/`SET`/shift decode against hardware — but it is not evidence about how
the chip comes up, and the section above reads as though it were.

One further detail the comparison turned up: the driver's busy loop is bounded
and then **writes anyway**, while the probe's returned early on timeout. The
driver's behaviour is why the register holds a correct value at all; had the
probe's early return been the driver's, the PLL would never have been set.

**What is genuinely new from the XIP read stands:** `romclk_enable(0)`,
`module_enable(87)`, `romclk_enable(31)` (a ROM no-op), `romclk_enable(56)`,
and the three registers in `0x4009B000` — that block really is unnamed
anywhere in this tree, and none of that chain is performed by
`sl6806_audio_begin()`. So does the negative: applying all of it, with
readback confirming each step, moved the length sweep not at all.

## `examples/ToneDemo`, rewritten around what is left

The old sketch re-ran twelve route/source combinations against a single
descriptor length and reported a mean. That is the measurement whose constant
10 µs was mistaken for a negative, and re-running it would only reproduce the
mistake — so the sketch now goes after the parts of the pacing path that have
never been examined, with an instrument that cannot report a constant where a
slope belongs.

**A — state, and a census of the pacing path.** One bit at a time, restored
after each, on `0x40080094` and on the three `0x4009B000` registers the vendor
writes. All-ones in a single write would be quicker and is exactly how the
PWM's pair register came to be recorded as `0x10F` for a year when it is
`0x1FF`: the probe wrote `0x3F0F`, which does not set bits [7:4] at all. Any
implemented bit outside what the driver writes is a field nobody has tried.

**B — pacing, measured at two lengths.** A single length cannot separate "the
DMA is unpaced" from "the DMA is not running" — both give a small constant.
Two can: unpaced is bus speed, paced is 192 KB/s, and the two differ by 2667×.
Reported as bytes/µs and as a multiple of real time so the comparison needs no
arithmetic from the reader.

**C — the bit clock divider.** `sl6806_audio.c` writes `DIV - 1 = 7` into
`0x40080094[10:8]`, so the vendor's divide-by-8 is field 7 and fields 0..6 —
divide by 1 through 7 — have never been tried. The justification for 8 is that
24.576 MHz / 8 is 64 fs for 32-bit stereo frames, which assumes a frame size
nothing has confirmed.

**D — module 2 cycled off and on**, which is the one difference between the
vendor's stream start and ours that has never been reproduced. It only ever
switches off an id it immediately switches back on; `sl6806_module.h` records
that the disable direction is the dangerous one.

The sine still plays throughout, so a success is audible before it is
readable — but **the ratio is the result**. Five rounds of the PWM work were
lost to trusting an impression over a number, and one of them to a panel that
was blinking rather than dimming.

### [M] `ToneDemo` first run — the census earned its keep, phases B–D did not

**The census found the thing it was built to find.**

| Register | implemented | holds | implemented but **never written** |
|---|---|---|---|
| `0x40080094` bit clock | `0x00000711` | `0x00000701` | **bit 4** |
| `0x4009B040` | `0x80000039` | `0x80000001` | bits 3, 4, 5 |
| `0x4009B04C` | `0x3FFFFFFF` | `0x0E900000` | 25 bits |
| `0x4009B050` | `0x000000FF` | `0x0000001B` | bits 2, 5, 6, 7 |

`0x40080094` implements bit 0 (enable), bits [10:8] (divider) and **bit 4**,
and nothing in this tree or in the vendor's code has ever written bit 4.

Put that next to the PWM's clock register. `0x400800F4` is bit 0 = enable,
**bit 4 = source select** — and on the PWM, setting bit 4 swaps a 24 MHz source
for a 25 kHz one. Same CRU, same family, same bit position, same role for
bit 0. If `0x40080094` follows the family then **bit 4 selects what feeds the
bit clock**, and a bit clock fed from nothing is precisely a DMA that drains
without being paced. `ToneDemo` phase `e` tries it both ways.

**Phases B, C and D measured nothing.** `pace()` used `small = 4796` and
`big = sizeof(wave)` — and `sizeof(wave)` *is* 4796, because `FRAMES` is
`PERIOD * 11` = 1199 frames = 4796 bytes. It timed the same length twice. So
the divider sweep and the module-2 cycle each compared one length against
itself, and neither says anything about pacing.

That is the third instrument of this kind in this investigation, after the
`PwmMode` sweep that measured inside the settling window and the `AudioLen`
rows that were all rejected on alignment. The fix is the same shape as before:
the two lengths are now 480 and 4796 from the same buffer, the ratio is printed
so a broken pair is visible as `1.0:1`, and the long row is deliberately not
made bigger — at real time 4796 bytes is 25 ms, inside the 60 ms poll bound,
whereas a 10× buffer would be 250 ms and would read as a timeout on the very
run where pacing finally worked.

**And one real observation fell out of the bug.** Two descriptors submitted
back to back with nothing between them: the first completed in 10–11 µs and
**the second timed out at 60 ms, on all ten rows**. `examples/AudioLen`, which
prints between transfers, never saw it. That is either a genuine re-arm
requirement between descriptors or an artifact of the gap — worth knowing
either way, so `pace()` now puts an explicit 2 ms settle between the two
timings rather than depending on `printf` for it.

No sound, which is expected: nothing is paced, and the only thing that has ever
been established about the output stage is that it is configured.

### [M] `ToneDemo` second run — the instrument is sound, and two more clean negatives

The two-length pair now differs, and the numbers validate against an
independent measurement: a two-point slope of **540 B/µs** against
`examples/AudioLen`'s eight-point fit of **519 B/µs**. Two sketches, two
instruments, the same rate.

The printed ratio is 3.6:1 for a 10:1 length ratio, and that is the fixed
overhead rather than a fault — `AudioLen` measured a constant 1.4 µs term, and
`480 × 0.00185 + 1.4 = 2.3` against a measured 3, `4796 × 0.00185 + 1.4 = 10.3`
against a measured 11. The *slope* is what carries the result; the ratio is
there so a pair that has collapsed to one length shows up as `1.0:1`.

Two negatives, both taken with a working instrument and real variation applied:

- **The bit clock divider is not the pacing.** All eight settings of
  `0x40080094[10:8]` — divide by 1 through 8 — give 2270× real time. The
  vendor's 8 is not special, and nor is anything else.
- **Cycling module 2 off and on changes nothing.** The one difference between
  the vendor's stream start and ours that had never been reproduced, now
  reproduced, with no effect.

Also worth recording: `TX ctrl +0x108` read `0xFA000910` on the previous run
and `0x12BC0910` on this one. `0x12BC` is 4796, this sketch's length;
`0xFA00` is 64000, which is `AudioLen`'s longest row and a sketch that had run
earlier in the same power session. The length field simply holds whatever was
last written, across payloads — a reminder that uploading does not reset this
block either.

**Phase `e` has still not been run.** It is the one the census pointed at, and
it is the only untried item on the list.

### [!] `e` did nothing because it was never wired up

The one experiment the census pointed at has still not run, and the reason is
a two-character omission: the key dispatch read

```c
if (key == 'a' || key == 'b' || key == 'c' || key == 'd')
```

while the switch below it had grown a `case 'e':`. Pressing `e` therefore set
no phase, printed nothing, and returned — **no output at all**, which is
indistinguishable from a device that has stopped responding. A hardware round
was spent discovering that the only untried item had silently not been tried.

Fixed two ways, and the second is the general one:

1. the dispatch is a range, `key >= 'a' && key <= 'e'`, so adding a case cannot
   leave it behind;
2. **every keypress now produces output** — the accepted key is echoed as
   `[e]`, an unrecognised one says so, and the switch's `default:` reports a
   phase with no implementation instead of falling through silently.

A probe that can accept input and give no sign of it is a probe that can waste
a bench session, and this one did. The same rule as the rejected rows and the
constant columns: **the transcript must show what happened, including
nothing.**

The runs either side of it reproduce cleanly — baseline and all eight divider
fields at 2270–2497× real time, module 2 unchanged — so the negatives above
stand and only `e` is outstanding.

### [M] Bit 4 is not it either — and the vendor's stream start read properly

Phase `e` finally ran. `0x40080094` bit 4 clear and set, readback confirming
both (`00000701` / `00000711`), and **2270× real time either way.** Bit 4 is
not the pacing.

That exhausts every candidate this document had. So, before sweeping the 32
implemented-but-never-written bits the census turned up, the vendor's code was
read properly — which is what cracked the PWM.

**`0x4009B000` appears exactly once in the entire 4 MB image**, at the literal
`0x00D9A298` already disassembled. The vendor writes those three registers and
nothing else; there is no further static material about that block.

**The stream start at `0x00D9662C`, in full, against what the driver does:**

| vendor | `sl6806_audio_clock_start()` |
|---|---|
| `module_disable(2)` | **not done** |
| `delay(10)` | — |
| `module_enable(2)` | `module_enable(2)` |
| `delay(10)` | `delay(10)` |
| `romclk_setdiv(44, 8)` | `mmio_field(0x40080094[10:8], 7)` |
| `romclk_enable(44)` | `mmio_set(0x40080094 bit 0)` |
| `[0x40009400] \|= 0x80` | `EQ_CTRL \|= START` — same register, same bit |
| mode field 4 → 6 | same three cases |
| `if ([0x4000940C] & bit31)` … | **not done, and never read** |

(`0x00807214` tail-calls ROM `0xBC1E`, one of the two scaled busy-waits decoded
during the PWM work, so the vendor's `0x807214(10)` is `delay(10)`.)

Two gaps, and the second is the larger:

1. **The module-2 cycle.** The vendor disables, waits, enables, waits;
   `sl6806_audio.c` only enables, and its comment records the deviation
   deliberately. The `d` phase that "tested" it omitted both delays, so it
   tested a bare off/on rather than the vendor's sequence. Now fixed.
2. **`0x4000940C` bit 31 gates twenty-one further calls** — ten to
   `0x00D958B4` over odd indices 1..19, one with 0, ten to `0x00D95854` over
   the even ones, then `0x00D95CC4`. Nothing here has ever read that register,
   let alone run any of it. Its neighbour at `+0x400` is `EQ_CTRL`, so this is
   plausibly the coefficient load rather than pacing — but "plausibly" is how
   four wrong conclusions in this file started, so the sketch now reads it and
   prints whether bit 31 is set.

### [M] Everything on the list is now negative — and the linearity argument does not prove what it claimed

The full sweep, all with a verified two-length instrument and readback on every
applied change:

| Tried | Result |
|---|---|
| four output routes, three source modes | 2270× |
| all 32 bits of `0x400E0000` | 2270× |
| bit clock divider, all 8 fields | 2270× |
| module 2, bare off/on | 2270× |
| module 2, the vendor's cycle **with both `delay(10)`s** | 2270× |
| bit clock `0x40080094` **bit 4**, clear and set | 2270× |
| the vendor's wider clock chain (romclk 0/31/56, module 87, `0x4009B000`) | 2270× |
| audio PLL | already correct |

And `+0x40C` reads `0x02200000` — **bit 31 clear**, so the vendor skips its
twenty-one extra calls in this state too. That is not an untried difference; it
is no difference at all.

#### [!] The linearity argument is weaker than this document now claims

The retraction above says: *a descriptor retired without doing anything takes
constant time; completion is linear in length; therefore data moves.* **That
does not follow.** Two things are linear in length:

1. a DMA reading N bytes of SRAM at bus speed;
2. a hardware **length counter** decrementing at bus speed, reading nothing.

Linearity proves the block *processes the length*. It does not prove it touches
RAM. The retraction of "nothing is transferred" was right to reject the old
480 MB/s reasoning — that was computed against the wrong clock — but it
replaced it with a conclusion its evidence does not carry.

Only a test that **observes memory** separates the two, and a TX engine cannot
be caught in the act; RX can. The single capture test ever run is already
recorded here as weak, and the reason is now exact: `sl6806_audio_begin()` sets
`TX_ENABLE` bit 0 at `sl6806_audio.c:154` and **never touches `RX_ENABLE`**, so
that test submitted a capture descriptor to a disabled direction.

`ToneDemo` phase `f` runs it properly — an address-derived pattern in a
dedicated buffer, `RX_ENABLE` bit 0 set, and the same test repeated with it
clear, because "memory changed" is only evidence if it does not also change
with the direction switched off.

| Outcome | Meaning |
|---|---|
| changed with RX set, unchanged with it clear | the DMA really moves memory; only the pacing is missing, and the retraction stands on proper evidence |
| unchanged both ways | the length is a counter and nothing has ever been transferred — which would retract the retraction |

That is the question this block has actually been sitting on since the
beginning, and nothing has ever asked it with the capture direction enabled.

### [M] Capture never runs — and phase `f` answered a different question than it asked

```
RX enable clear -> +0x200 = 02300700   TIMED OUT after 60081 us, 0 of 512 changed
RX enable SET   -> +0x200 = 02300701   TIMED OUT after 60082 us, 0 of 512 changed
```

Two findings, and a flaw in the test.

**The capture direction does not retire at all.** 60 ms, no completion flag,
against a TX direction that retires in 10 µs. That asymmetry is itself
informative: a generic length counter would retire on *both* directions, since
it is the same mechanism one page apart. So TX's retire is not simply a
counter running down.

**`+0x200` bit 0 does not clear.** The second run of the phase read `02300701`
on its "clear" row, so both rows had RX enabled and its control was not a
control. Only the first run's pair is valid. The sketch now prints the register
beside the intent and flags a row whose write did not take, because reporting
what was *meant* rather than what *happened* is how a non-control gets read as
a control.

**And the test cannot answer its own question.** Watching a capture buffer says
nothing about whether the DMA reads memory when the direction being watched
never runs. `f` established that RX does not work; it did not establish
anything about TX.

#### The test it should have been: vary the source, not the destination

The question is whether the direction that *does* complete reads its source.
That needs no second direction and no memory to be written:

> A transfer's time depends on how fast its **source** can be read.
> A length counter's does not.

Phase `g` submits the same length from three sources of very different speed —
SRAM, XIP flash (off-chip over SPI, far slower), and the mask ROM. All three
are plainly readable, so nothing can fault.

| Outcome | Meaning |
|---|---|
| times differ by source | the DMA reads memory — settled, and only the pacing is missing |
| times identical | it reads nothing, the length is a counter, and the retraction in this document is itself retracted |

Same shape as the length sweep that started this, moved to the one variable
nothing has ever varied.

## [M] SETTLED — the DMA does not read its source. The original negative was right.

```
SRAM (wave)  00838b38  4796 bytes in 9 us  (532 B/us)
XIP flash    00c10000  4796 bytes in 9 us  (532 B/us)
```

**Identical.** XIP flash is off-chip SPI, and the bootloader sets that clock to
32 MHz (`romclk_set(6, 12)`, 384 MHz / 12). Reading 4796 bytes from it:

| mode | rate | time |
|---|---|---|
| single-bit SPI | 4 MB/s | 1199 µs |
| dual | 8 MB/s | 600 µs |
| quad | 16 MB/s | 300 µs |
| **measured** | | **9 µs** |

532 MB/s out of SPI flash is impossible by any margin, cache or not. A transfer's
time depends on how fast its source can be read; this one does not depend on it
at all. **The DMA does not read its source.**

(The mask ROM row was rejected: `sl6806_audio_play()` refuses a NULL buffer,
which is correct of it. The sketch now uses `0x00000100`. Two sources four
orders of magnitude apart already settle it.)

### So the retraction is itself retracted

`docs/AUDIO.md`'s original conclusion — *the block accepts a descriptor and
retires it having moved nothing* — **was right.** What was wrong was only its
reasoning: 480 MB/s was called impossible against a 64 MHz core, and §30 had
already measured the bus PLL at 192 MHz, where that rate is unremarkable. The
conclusion survived its own bad argument.

And my replacement was worse, because it was confidently stated: *linear in
length, therefore data moves.* A length counter decrementing at bus rate is
linear in length too. ~519 B/µs is about 130 M words/s, which against a 192 MHz
bus is a counter retiring roughly one word per one-and-a-half cycles — exactly
what a descriptor being read down without any fetch looks like.

The state of the block, on evidence that now holds:

| | |
|---|---|
| TX descriptor | retires in time linear in length, **independent of source memory** — an internal counter |
| RX descriptor | never retires at all |
| memory | never observed to change, in either direction |
| routes, source modes, `0x400E0000`, bit clock divider and bit 4, module 2 cycle, the vendor's wider clock chain, audio PLL | all applied with readback, all negative |

So `0x40009000` is configured, its registers hold what the vendor's code puts
there, and **nothing in it fetches**. The question is no longer "what paces the
DMA" — it is "what makes the DMA fetch at all", which is a different and
earlier question than this document has been asking since the beginning.

### What that leaves

The census already mapped the unexplored surface: 25 implemented-but-never-written
bits in `0x4009B04C`, four in `0x4009B050`, three in `0x4009B040`. Beyond that,
the audio block's own registers have never had an implemented-bit census of
their own — only `0x40009400` and `0x4000940C` have been read, and the reset
values of thirty others were recorded without ever asking which of their bits
are writable.

That is where a fetch enable would be, if it is anywhere reachable.

## `examples/AudioBeep` — the demo, and why it is honest about being silent

`examples/ToneDemo` is now seven phases of instrumentation and is unreadable as
an example. `AudioBeep` is the twenty-line version: begin, route, volume,
unmute, `sl6806_audio_play()`, four notes of an arpeggio.

**It makes no sound today**, because the block does not fetch — and it says so
rather than leaving the reader to suspect their headphones. It measures instead
of claiming:

> A buffer of N bytes of 48 kHz 16-bit stereo is N/192 ms of audio. If the
> hardware is really playing it, the completion **cannot arrive sooner than
> that** — a DAC consuming at 48 kHz is what paces the DMA, and that is exactly
> what "it works" means here.

So each note prints its retire time against the 120 ms it lasts, and the sketch
ends with one of two verdicts:

- **NOT PLAYING** — with the fastest retire quoted, and a pointer to this
  document. Expected today.
- **PACED AT REAL TIME** — the block is playing. If nothing is audible at that
  point the fault has moved *past* the DMA to routing, the amplifier enable or
  the jack, which is a different and much shorter search.

That second branch is the point of writing it now. It is the regression that
will announce success without being modified, the day someone finds what makes
the block fetch. A demo that prints "playing!" and makes no sound would be
worse than no demo at all.

### [M] `AudioBeep` confirms the model, and a new lead in the submit path

Four notes, 23040 bytes each, retiring in 43–44 µs against 120 ms of audio.
**536 B/µs** — another point on the same line, at a length nothing had tested,
and the sketch correctly printed `NOT PLAYING` with the number rather than
claiming anything.

Reading the submit path afterwards, two things check out and one does not.

**The descriptor is right.** The vendor's TX submit at `0x0080D9BC` is, exactly:
`TX_ADDR = addr`; `TX_CTRL[31:16] = len`; then it assembles
`(3 × len / 4) << 16 | 1` in a stack temporary and stores it to `TX_TRIG`.
That is `sl6806_audio_trigger_word()` to the bit, three-quarter watermark and
all.

**The start is right.** `0x00D97A5E` is `TX_CTRL |= 0x10`, and its sibling
branch does the same to `RX_CTRL` at `+0x208`.

**But there is a priming path the driver has never performed.** At
`0x00D97A20`, before any of that:

```
[driver_state + 0x128] = r7          ; a flag, not MMIO
TX_CTRL |= 0x10                      ; START set FIRST
submit(0x0080D9BC, buf = state+0x118, len = 16)   ; a SIXTEEN-BYTE descriptor
while ([driver_state + 0x128]) ;     ; spin until an ISR clears the flag
0x00D94B0C()
```

So the vendor **sets START before the descriptor**, submits a 16-byte primer
from a buffer inside its own driver state, and then *waits for the completion
interrupt to fire* before doing anything else. `sl6806_audio_play()` writes the
descriptor and then sets START, never primes, and has no interrupt at all.

Whether a 16-byte primer with START pre-set is what arms the fetch is not
established — it is one reading of one call site, and this document has
promoted several of those to conclusions it had to withdraw. But it is the
first thing found in the fetch path that the driver demonstrably does not do,
and it is cheap to try: submit 16 bytes with START already set, wait, then
submit the real buffer and see whether the retire time moves off 43 µs.

### The priming path, read carefully — and what of it can actually be tried

`0x00D97A20` looked like a hardware handshake. It is not, and the difference
matters enough to state before anyone builds on it.

```
[state + 0x128] = 1
TX_CTRL |= 0x10                       START set FIRST
submit(state + 0x118, len = 16)       a SIXTEEN-BYTE descriptor
while ([state + 0x128]) ;             spin
0x00D94B0C()
```

The base is `[0x0082B310]`, the audio driver's **state pointer** — and two
instructions earlier the callback `0x0080D955` is stored into the same struct.
So `+0x128` is a flag the vendor's *interrupt handler* clears, not a register,
and `+0x10C` in that routine is a state byte rather than `TX_ADDR`. A probe
that spun on those addresses as MMIO would be reading SRAM and would either
hang or quietly measure nothing.

That is worth recording as a near miss: the byte-width accesses (`strb` into a
register block that is otherwise all 32-bit) were the tell, and they were
visible in the first disassembly.

**What remains testable is real, though, and it is two things:**

1. **START before the descriptor.** The vendor has the channel running when the
   descriptor lands. `sl6806_audio_play()` writes the descriptor and starts
   afterwards. If the block latches on a running channel rather than on the
   START edge, that is the difference.
2. **A 16-byte primer** submitted before the real buffer.

Both are in `examples/AudioPrime`, with a fourth mode that primes before *every*
buffer in case the primer arms a single fetch rather than the channel.

The detector needs no judgement: 23040 bytes is 120 ms of audio, unpaced
retires in ~43 µs, and a factor of **2791** is not something anyone has to
squint at.

### [M] The submit path is exhausted too — and the question moves earlier

`examples/AudioPrime`: START before the descriptor, a 16-byte primer, and a
primer before every buffer. **All 43–45 µs, all unpaced.** The descriptor was
already bit-exact against the vendor's, so nothing in the submit path was ever
the problem.

The primer row is a useful consistency check rather than a null: 16 bytes
retired in **1 µs**, against a model of `16 × 0.00185 + 1.4 = 1.43`. The
counter model now holds at both ends of a **1440:1** length range, 16 bytes to
23040.

So the question is no longer what paces the DMA. **It is what lets the DMA
reach the bus at all**, which is earlier than anything this document has been
asking.

### `examples/AudioFetch` — the walk that has never been run

**The detector is the source-memory test, not the pacing one, and the choice is
the point.** Pacing needs two things to be true: the engine must fetch *and*
the DAC must consume at 48 kHz. Fetching needs only the first. So a source-speed
difference appears the moment the bus master wakes, even with nothing
downstream ready — strictly the more sensitive instrument, and the one that
fails first.

| | |
|---|---|
| SRAM and flash the same | still not reading |
| flash slower than SRAM | **it is reading memory** |

Signal to noise is about thirty: reading 4796 bytes over 32 MHz SPI is ≥300 µs
even in quad mode, against ~10 µs not reading, and the logs show ±1 µs of
scatter.

§14a walked all 128 module ids once and got nothing — but it scored on CTRL
bit 28 of the *PWM*, which is not a run flag, and §32 records that walk as void.
**No walk has ever been run against the audio block with a working detector.**
This is that walk, over both clock families.

Ids are only ever switched **on**; `sl6806_module.h` records the disable
direction as the dangerous one and the sketch never uses it. Each id is
announced and the call returns before the write, so a hang names the id.

The walk is cumulative on purpose — a combination that only works together is
still reachable — which means a hit names the *last* id enabled, not
necessarily the only one that mattered. Re-run from cold with just that one to
confirm.

### [!] The walk was void — and its own baseline said so in the first line

```
[b]
  baseline      SRAM     9 us   flash 60001 us   READS MEMORY
```

**Nothing had been enabled, and the baseline already claimed the DMA reads
memory.** That is impossible, so the instrument was broken before the walk
began — and the walk then reported 128 of 128 module ids and 56 of 56 romclk
ids as hits, which is 184 discoveries or one bug.

`60001` is the poll bound. The flash row did not read slowly; it **never
retired**. And the cause is already recorded two sections above: `fetches()`
submitted two descriptors back to back with no gap, and *the second submission
after no gap times out* — measured on all ten rows of `ToneDemo`'s first
`pace()`, which is why that function has a `settle()` in it. `AudioFetch` did
not.

Three fixes, and the third is the one that matters:

1. `settle()` between the two timings, as `pace()` has.
2. **A timeout is not a slow read.** Hitting the bound means the descriptor
   never retired, which is a different fault; it now scores as "not a
   measurement" rather than as an enormous time. Scoring it as a time is
   precisely what turned every row into a hit.
3. **The baseline is now a gate that can fail.** It refuses to run either walk
   and prints `THE BASELINE ALREADY 'READS MEMORY'. It cannot - nothing has
   been enabled. The instrument is broken and the walk is refused.`

That third point is the lesson, and it is the fourth time this investigation
has hit the same wall. The sketch **had** a baseline, designed for exactly this
failure — and it printed the same cheerful `READS MEMORY` verdict as every
other row, so the control could not contradict the result. A control that
cannot fail loudly is not a control; it is decoration. The comment above it
even read *"two equal numbers here means the detector is working"* while the
code printed a verdict without ever checking that they were equal.

## [M] The clock space is exhausted — the problem is not a clock

With the detector fixed and its baseline passing (`SRAM 9 µs, flash 9 µs,
equal`), both walks ran clean:

```
128 module ids, 0 hit(s)
 56 romclk ids, 0 hit(s)
```

**And the walks are cumulative.** By the end of `m` every one of the 128 module
clocks was on; by the end of `r` every romclk id as well — and the DMA still
never touched its source. That is a far stronger statement than 184 separate
negatives: with essentially every clock in the chip enabled, the audio DMA does
not read memory.

This is also the first walk of the clock space ever run against this block with
an instrument that works, gated by a control that can fail. §14a's walk was
void; this one is not.

**So whatever stops the block fetching is not a clock.** That closes the line
of enquiry this document has followed since it opened — "configured and not
running" was read as a clocking problem for its whole history, and it is not
one.

### What is left, cheapest first

1. **An implemented-bit census of the audio block itself.** Its thirty
   registers have had their *reset values* recorded and nothing more — nobody
   has ever asked which of their bits are writable. That question found four
   hidden bits in the PWM's pair register after a year of it being "known", and
   bit 4 of the audio bit clock. `examples/AudioFetch` phase `c` does it over
   the ten registers in the path, one bit at a time with a restore after each.
   The `unwritten` column it prints — implemented but never written by the
   driver — is the search space.
2. **The source address may not be what we think.** `+0x10C` is taken as a
   buffer pointer because the vendor writes one there, but a descriptor-pointer
   format would look identical from the outside and would explain a length that
   counts down against nothing.
3. **A bus-master enable outside both clock families.**
4. **A generate-rather-than-fetch mode**, in which the length is consumed
   without a source being read at all. That would fit every measurement taken.

### [V] The descriptor-pointer hypothesis is ruled out, statically

`+0x10C` might have been a descriptor-list pointer rather than a buffer
pointer — a format that would look identical from outside and would explain a
length counting down against nothing. It is not. There are four call sites of
the TX submit `0x0080D9BC`:

| site | buffer | length |
|---|---|---|
| `0x00D97A3C` | `state + 0x118` | 16 |
| `0x00D99F62` | `[0x0082B42C] + 0x40` | 16 |
| `0x0080FE4A` | `state + 0x40` | 16 |
| **`0x00D99FBA`** | **ROM `0xC81C`(state→0x1C)** | **`[state + 0x38]`, a halfword** |

Three pass a fixed 16, which is what made "16 = the descriptor size" tempting.
The fourth does not: it takes a pointer from a queue accessor and a **variable
byte length** out of driver state. A descriptor pointer does not come with a
caller-supplied variable length, and a 16-byte descriptor would not be
submitted with a length of 4796 from anywhere.

So `+0x10C` is a buffer pointer and `TX_CTRL[31:16]` is a byte count, exactly
as `sl6806_audio_play()` assumes. The three 16s are small fixed buffers — four
stereo frames each, most plausibly silence — not descriptors.

That leaves, of the four hypotheses:

1. **the audio block's own unwritten bits** — `examples/AudioFetch` phase `c`,
   never run;
2. ~~the source address is a descriptor pointer~~ — ruled out here;
3. a bus-master enable outside both clock families;
4. a generate-rather-than-fetch mode, which still fits every measurement.
