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
