# SL6806 Arduino framework — bring-up status

**As of 2026-08-06.** Device under test: one P20 Player, USB `301a:2800`,
bootloader inquiry `SMTLINK DEVICE 2.00`.

This is a snapshot for a hardware debugging session, not repo documentation.
The durable findings live in `docs/sl6806_re_notes.md`, `docs/LCD.md` and the
README.

---

## 1. Confirmed working, on real hardware

Everything in this table was observed on the device, not inferred.

| Capability | What proved it |
|---|---|
| 4 MiB flash dump | `sl6806-checkdump` passes; both HLKJ CRC16s verify |
| Mask ROM dump (`0`–`0x7D000`) | 93.1% of 7480 branches resolve internally |
| SRAM dump (`0x00800000`+256K) | vector table 83.4% ROM-range pointers; ROM stack readable |
| `sl6806-dumpram`, incl. `init` and resume | produced both dumps above |
| Payload build → upload → run | `Hello` output appeared |
| Boot ROM calls `_start` | anything printed at all |
| `.bss` cleared, C++ constructors run | banner and variant name printed |
| Heap + `String` | `heap+String ok, 6806 reporting in` — only prints if `malloc` **and** the C++ runtime work |
| Console ring (device → host) | monitor read it |
| Monitor finds the ring by symbol | `monitoring console at 0x008265cc` |
| `_start` returns cleanly; ROM keeps servicing USB | monitor kept polling after `setup()` |
| Vendor SCSI handler (`scsi_cb`) fires | it *is* what the monitor polls |
| USB mode detection (after fix) | `bootloader mode confirmed (SMTLINK DEVICE 2.00)` |
| Host status query (`0x53544154`) | 281 polls in 60 s, every one answered |
| `sl6806-calibrate` end to end on hardware | it ran, and it diagnosed a real fault (§2) |
| **CPU clock = 64.000 MHz** | 94 samples/20 s, bracket 63,961,008..64,039,063 — one whole MHz inside it |
| SysTick as the time source | `counter=24-bit` after the DWT was rejected |

**This is the entire payload toolchain, end to end, except for driving
`loop()`.**

## 2. Measured negatives

### The cycle counter does not run. This is the big one.

**2026-08-06, on hardware.** 281 status polls over 60 s, every one answered,
every one reporting **the same cycle count**. `sl6806_cycles()` had been
returning 0 the entire time. The device also reported `counter=32-bit`, i.e.
its own probe believed the DWT was working.

Both halves matter:

- **The DWT cycle counter is not usable on this part.** The register reads a
  constant. Whether it is unimplemented (reads junk) or present but unclocked
  (reads zero) is still open — the status reply now carries the raw value, so
  the next run that reports it settles it. Academic now that SysTick works,
  but worth recording.
- **The probe was wrong.** `sl6806_time_init()` wrote 0 to `CYCCNT` and
  accepted *nonzero* as proof it ticked. A constant nonzero passes that test.
  It now requires the register to **change**, and falls back to SysTick, and
  reports `tick_mask = 0` if neither moves.

**This is also the cause of the poll-mode wedge**, which had been blamed on
blocking for too long. It was never a duration problem:

```c
target = sl6806_cycles() + ms * (F_CPU / 1000);
while (sl6806_cycles() < target)   /* never terminates on a frozen counter */
    yield();
```

`delay()` cannot finish, ever. In `RUN_MODE=poll` that loop runs inside the
boot ROM's USB handler, so the device leaves the bus and stays off it until
it is unplugged — exactly what was seen twice. It also explains why capping
the delay at 1 ms changed nothing: the cap lowers a target the counter never
approaches.

`delay()` and `delayMicroseconds()` now return immediately, once, with an
explanation, when there is no time source. Wrong timing is recoverable; a
wedged link mid-session is not.

### The ROM's `cb2` is not a periodic callback — now measured directly

`Hello` in the default `RUN_MODE=hook` printed all of `setup()`'s output and
then never ticked. Since the monitor kept working, `_start` returned and the
ROM was healthy — it simply never called `cb2`.

**Confirmed independently 2026-08-06:** a 60-second status run in hook mode
reported `0 loop() calls` against 281 polls. The counter is incremented by the
core on every path that runs `loop()`, so this is the ROM's behaviour measured
directly rather than inferred from silence. `examples/CallbackProbe` is now
only needed to find out *which other* slot might fire, not whether `cb2` does.

Recorded in `cores/sl6806/sl6806_rom.h`.

### SysTick works, and its 24-bit wrap is now a real constraint

With the DWT rejected the core falls through to SysTick, which **does** count:
that is what made the 64 MHz measurement possible. The cost is the wrap
period — 2²⁴ / 64 MHz = **262 ms**, against ~67 s for a working 32-bit DWT.

`sl6806_cycles()` only accumulates a wrap when it is read, so any code that
runs for a quarter of a second without calling `millis()`, `delay()` or
`yield()` silently loses time. An ordinary `loop()` is fine; a long
computation is not. Recorded at the top of `cores/sl6806/wiring_time.c`.

It also constrains calibration: the first run polled every 212 ms, 81% of a
wrap, and got away with it. `sl6806-calibrate` now picks its own interval from
the counter width and a pilot estimate of the clock — measured round trips are
~12 ms, so it settles near 50 ms, a fifth of a wrap. Forcing the old 200 ms
against a simulated 64 MHz device produces a confident, wrong 3.4 MHz, caught
only by the consistency check.

If the wrap ever becomes the binding constraint, `SYSTICK_CTRL_CLKSOURCE = 0`
selects the external reference (typically the core clock ÷ 8), which would buy
~2.1 s. It is optional on Cortex-M4, so it needs the same "does it move" probe
— and it would decouple the tick rate from `F_CPU`, which `millis()` currently
assumes are the same thing.

### The vendor SRAM routines are not resident in bootloader mode

`0x0080E842` (`lcd_write_cmd`), `0x0080E8D8` (`lcd_write_data`) and
`0x00811C7C` (`gpio_write`) contain uniformly random bytes — entropy 7.96,
50.08% ones, no repeated 16-byte block in 256 KB. The dump is faithful (see
§1), so this is a real absence: in bootloader mode the application that
installs those drivers has never run.

Consequence: a payload cannot call them. The "ROM route" to a display or to
GPIO is closed.

### The mask ROM does not contain what was hoped

No reference to the LCD controller at `0x400D9000` anywhere — the boot ROM has
no display driver. No GPIO-shaped register block. It does not hold the
application's SRAM driver blob either (delta search: best 15/251 against a
noise floor of 13–14).

## 3. Untested on hardware

| Item | Status |
|---|---|
| `RUN_MODE=poll` | **Run twice, wedged twice.** Cause found and fixed (§2) — but the fix has not itself been run on the device. Test #1. |
| Any callback slot other than `cb2` | `examples/CallbackProbe` written, never run. `cb2` itself is now settled (§2). |
| Serial RX (host → device) | never exercised |
| `F_CPU` at 64 MHz in ordinary use | **measured**, but no sketch has yet been run with the corrected default |

⚠ **`RUN_MODE=takeover`** kills USB by design — `_start` never returns, so
there is no monitor and no output. It is not a workaround for a sketch that
will not tick; it gives you a `loop()` you cannot observe.

The hazardous `ClockCalibrate` sketch has been **deleted** — it blocked
`setup()` for ~32 s, which in payload mode is 32 s of dead USB. Its job is now
done from the host without blocking anything (§6). It is in git history if it
is ever wanted.

## 4. Known impossible today

| | Why |
|---|---|
| Display output | No `sl6806_lcd_bus_t` exists. `sl6806_panel_get()` returns NULL by design. `GfxDemo` runs, draws into RAM, passes its own readback checks, and shows nothing. |
| GPIO | Registers unknown, and the vendor back end has nothing to plug into (§2). `Blink` cannot blink. |
| `shiftOut`, `shiftIn`, `pulseIn`, `pulseInLong` | Written and correct by construction; built on `digitalWrite`/`digitalRead`, so no pins means no function. |
| `analogRead`, `analogWrite`, `tone`, `attachInterrupt` | Report by design; no ADC/timer/pin-interrupt registers. |
| `MODE=firmware` | Unproven, and the only thing that can leave a non-booting device. Nothing here needs it. |

## 5. What the display actually needs

The panel itself is **fully recovered**: 240×296 at controller offset (0,12),
RGB565 (`COLMOD 0x55`), `MADCTL 0x00`, a 33-command vendor init sequence plus
sleep/wake/on/off — all regenerable from a dump with `tools/sl6806-panelseq`,
and checked command-by-command by 53 host tests.

The LCD controller is at **`0x400D9000`**, found via the HLKJ bootloader,
which logs its own function name `HAL_lcdc_module_init`. Register map in
`cores/sl6806/sl6806_lcdc.h`. The handoff is understood:

```
store the command-list address to  +0x88
set bit 0 of                       +0x80
set bit 0 of                       +0x84
```

**The one remaining blocker** is the command-list opcodes — `0xABAB0005` and
`0xCDCDxx03`/`0xCDCDxx02` — and specifically the `b` field in each record. It
is a descriptor byte shifted left 8 and differs between the two window
records, which is the shape a DCS command opcode would have; but it is read
from `+0x0D`/`+0x0E`, and the application's panel descriptor keeps CASET/RASET
at `+0x11`/`+0x12`. Two readings, recorded rather than guessed, because the
difference decides whether the controller or the driver emits the command
byte.

This probably needs a logic analyser on the panel bus, or tracing
`lcdc_set_descriptor` against a live transfer. Another dump will not settle it.

## 6. The clock problem — method built, run, and it found something worse

`F_CPU` is a placeholder. Every `delay()` and `millis()` is off by one constant
ratio, so one measurement fixes all timing at once.

Host-side calibration is **built**. The device answers a second sentinel
address — `0x53544154`, "STAT", alongside the console's "CONS" — with its own
free-running cycle counter plus a few counters, straight out of the ROM's USB
handler (`cores/sl6806/sl6806_stat.h`). `tools/sl6806-calibrate` asks
repeatedly, stamps each answer with host wall-clock time and regresses cycles
against seconds. The device times nothing and blocks nothing.

```sh
make SKETCH=examples/Hello RUN_MODE=poll calibrate
```

Design points worth keeping:

- **The query is side-effect free.** It does *not* drive `loop()`, even in
  poll mode, so every round trip costs the same and the sketch's own work
  cannot leak into the measurement as jitter.
- **It reports a bracket, not just a number.** Each round trip pins the
  counter to an *interval* of host time, so the tool reports the range of
  rates no pair of samples can exclude. The bracket narrows with duration, not
  sample count: ~0.6% over 12 s, a few parts in 10⁴ over a minute.
- **It refuses rather than averages.** If no single rate fits all samples the
  answer is reported as untrustworthy. That is the detector for the one real
  failure mode: `sl6806_cycles()` only notices a hardware wrap when it is
  read, so polling slower than the counter wraps silently loses time and would
  otherwise produce a plausible, much-too-low clock.
- **The reply carries `tick_mask`**, so the host knows whether the counter is
  the 32-bit DWT or the 24-bit SysTick fallback — which is what sets the
  maximum safe poll interval, and also answers from the device whether this
  Cortex-M4 implements the DWT at all.

It also reports `loops`, the count of `loop()` calls the core has made. In
default hook mode that is a direct read on §2's measured negative: poll twice a
second apart with nothing else driving the sketch, and if `loops` moved, this
ROM's `cb2` does fire after all.

**Run on hardware 2026-08-06, and it worked — after finding §2 first.**

The first run answered 281 of 281 polls and returned a flat line, which is how
the frozen DWT was found. With the probe fixed, the second run measured:

```
  measured clock : 64,000,071 Hz (64.000 MHz)
  bracket        : 63,961,008 .. 64,039,063  (+/- 0.061%)
  counter        : 24-bit  (the SysTick fallback; the DWT does not run)
```

The bracket contains **exactly one whole MHz**, and a PLL output is
overwhelmingly likely to be a round number, so the answer is taken as
**64,000,000 Hz**. That is now the `F_CPU` default in the `Makefile`, marked
`[V]` with its provenance — the PLL registers are still not found, so this is
measured, not derived.

Consequence: the old 120 MHz placeholder made every `delay()` **1.875x too
long** and `millis()` run 1.875x too slow. All of it is now correct by one
constant.

Verified against a simulated device at a known clock, against a simulated
device whose counter is frozen, and against real ARM images under the
emulator.

## 7. Repository state

Merged to `master`: PRs #3–#8. Nothing is outstanding on a branch.

| PR | Contents |
|---|---|
| #3 | Panel sequences recovered, LCDC located, GPIO vendor seam, `shiftOut`/`shiftIn`/`pulseIn`, host panel tests |
| #4 | Notes moved into `docs/` |
| #5 | Mask ROM + SRAM findings, `checkdump --rom/--sram` |
| #6 | CI, and Unicorn emulator smoke tests |
| #7 | `3rd/actions_flash` |
| #8 | `RUN_MODE=poll` and its blocking cap, USB mode check fixed, `CallbackProbe` |

Uncommitted in the working tree: the host-side clock calibration of §6 —
`sl6806_stat.h`, the status branch in `startup_payload.c`,
`tools/sl6806-calibrate`, `tools/sl6806_dev.py` (the smtlink_dump plumbing,
now shared with the monitor), the tests below, and the deletion of
`examples/ClockCalibrate`.

### Test inventory — 269 checks, all passing

```
tests/host   test_console      30   console ring: wrapping, overflow, framing
tests/host   test_gfx          64   drawing primitives, clipping, font
tests/host   test_panel        53   panel command stream, DCS windowing
tests/tools  test_inquiry      13   USB mode detection parser
tests/tools  test_calibrate    53   clock estimator, frozen-counter and
                                    refusing-device diagnosis, wrap-safe poll
                                    rate, tool end to end
tests/emu    test_smoke        56   real ARM images under Unicorn, including a
                                    frozen counter and the Makefile flag stamp
```

Plus 12 sketch × mode builds and 3 run-mode builds, all clean under `-Werror`.

**Two of those tests exist because of this session** and would have caught its
failures: a frozen cycle counter that used to hang `delay()` forever, and a
`make` that rebuilt nothing when `RUN_MODE` changed and therefore re-uploaded
the previous mode's binary.

**What the tests cannot catch:** the emulator models memory, not USB timing.
That is exactly why the poll-mode wedge got past 184 green checks. Anything
about how long the device sits inside the ROM's handler can only be settled on
the bench.

## 8. Debug plan

Each step answers one question.

**1 — Does poll mode survive now?** The one that matters.
```sh
make SKETCH=examples/Hello RUN_MODE=poll upload
tools/sl6806-calibrate --duration 10        # same handler, never runs loop()
tools/sl6806-monitor build/Hello.sym        # this one runs loop()
```
Probe before monitoring: both go through the same handler, only the console
poll runs `loop()`. `delay()` can no longer spin forever and the clock is now
right, so the previous wedge should be gone. If it still wedges, the cause is
something else about running `loop()` inside the handler — stack depth or
re-entrancy — and not the blocking cap, which was never the problem.

Expect ~20 ticks/second and `millis()` finally advancing in real time.

**2 — Is there a real periodic callback?**
```sh
make SKETCH=examples/CallbackProbe RUN_MODE=poll run
```
`cb2` is settled (§2); this is only to find out whether `cb0`–`cb5`/`cb8`
fire. Worth running once, low priority.

**3 — Serial RX.** Type into the monitor from step 1. Cheap, and it is the
half of the console never exercised.

**4 — Confirm the clock.** `tools/sl6806-calibrate --duration 120` should
reproduce 64 MHz with a tighter bracket. It now picks its own poll interval to
stay inside the 262 ms SysTick wrap; the first run did not, and sat at 81% of
one.

### Recovery, if anything wedges

Unplug, hold the boot key, plug back in. Nothing in the payload workflow
writes to flash, so there is nothing to repair. Keep `dump.bin` as the golden
recovery image regardless.

### Before plugging in

```sh
sudo systemctl stop fwupd     # observed dropping this board ~1.3 s after enumerating
```
