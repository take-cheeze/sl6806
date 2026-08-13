# Bring-up on real hardware

Everything here is safe: the payload workflow never writes to flash, so
nothing in this document can leave you with a device that will not boot. The
worst case is a wedged USB link, and the fix is always the same — unplug it.

If you are starting from nothing, read this top to bottom once. It is ordered
so each step tells you whether the next one is worth attempting.

---

## Before you plug in

```sh
sudo systemctl stop fwupd
```

`fwupd` has been observed grabbing this board about 1.3 seconds after it
enumerates, which shows up as reads that worked a moment ago suddenly timing
out. Stopping it is not optional on a system that runs it.

Put the device in bootloader mode (hold the boot key while plugging it in) and
confirm the host agrees:

```sh
make SKETCH=examples/Hello upload
```

The upload tool decodes the SCSI INQUIRY response and prints

```
bootloader mode confirmed (SMTLINK DEVICE 2.00)
```

If it instead says the device is in **card-reader mode**, stop — uploading
there does nothing useful. Re-enter bootloader mode and try again.

## Step 1 — does anything run at all?

```sh
make SKETCH=examples/Hello RUN_MODE=poll run
```

`run` uploads and then opens the monitor. Expect a banner, a variant name, a
line saying the heap and `String` work, and then a `tick` roughly twenty times
a second.

That single screen proves the entire payload toolchain: the ROM called
`_start`, `.bss` was cleared, C++ constructors ran, `malloc` and the C++
runtime work, `setup()` ran, `_start` returned, the ROM kept servicing USB,
the vendor SCSI handler fired, the console ring is being read, and `loop()` is
being driven.

**If you get the banner and then nothing ticks**, `loop()` is not being
driven. That is what `RUN_MODE=poll` is for — see the next section.

**If the device stops answering entirely** (`LIBUSB_ERROR_TIMEOUT`), unplug
and replug it. Something blocked inside the ROM's USB command handler for
longer than the host's SCSI timeout. See "The one rule" below.

## How `loop()` gets driven

Three choices, `RUN_MODE=`:

| Mode | How `loop()` runs | Cost |
|---|---|---|
| `hook` (default) | The boot ROM's `cb2` idle callback | Free, **but `cb2` is not periodic on every ROM revision** — one unit never called it at all |
| `poll` | From the vendor SCSI handler, once per host poll | Needs a monitor connected; ~20 Hz |
| `takeover` | `_start` never returns and spins | Kills USB — no console, no output, nothing to watch |

`takeover` is not a workaround for a sketch that will not tick. It gives you a
`loop()` you cannot observe.

If your unit does not tick in `hook` mode, run `examples/CallbackProbe` in
poll mode. It installs a distinct counter in every slot of the ROM callback
table and prints the counts; `polls` is the known-good baseline. If some other
slot climbs, that is a real idle hook and worth reporting. If they all stay at
zero, polling is the only route on that ROM.

## The one rule

**In payload mode the boot ROM cannot service USB while your code is
running.** In `RUN_MODE=poll` your `loop()` runs *inside* a USB command. Block
there past the host's SCSI timeout — about a second — and the endpoint
desynchronises and the device stops answering anything at all until it is
unplugged.

This is not theoretical. A `delay(1000)` in `Hello` did exactly that.

The core caps blocking at 50 ms in poll mode and reports the first time it has
to. Take that report as a bug in the sketch, not as noise: pace `loop()` with
`millis()` instead of `delay()`.

```cpp
void loop()
{
    static uint32_t next;
    uint32_t now = millis();
    if ((int32_t)(now - next) < 0)
        return;            /* return promptly; come back next poll */
    next = now + 1000;
    ...
}
```

`examples/CallbackProbe` is written this way — its `loop()` counts polls and
returns immediately except once every eight, rather than ever blocking.

## Step 2 — the clock

`F_CPU` defaults to 64,000,000 — one P20 Player, measured, not a datasheet
value. The clock and reset unit at `0x40080000` holds dividers and gates, not
a PLL multiplier, and nothing in the dump establishes the crystal, so this
came from timing the device against the host rather than from a register. If
you are bringing up a different unit, re-measure rather than assume — it is a
single scale factor, and one measurement fixes every `delay()` and `millis()`
at once:

```sh
make SKETCH=examples/Hello RUN_MODE=poll calibrate
```

**How it works, and why it does not need a sketch loop() at all.** The device
answers a status query — a sentinel address alongside the console's, handled
straight out of the ROM's USB command handler — with its own free-running
cycle counter. `tools/sl6806-calibrate` polls that, stamps each answer with
host wall-clock time, and regresses cycles against seconds. Nothing in the
sketch runs to make this happen, so it works under any `RUN_MODE` and cannot
be perturbed by whatever the loaded sketch is doing.

It reports a bracket, not just a number: the range of rates no pair of
samples can exclude, and it refuses to answer rather than average when no
single rate fits them all. See
[`cores/sl6806/sl6806_stat.h`](../cores/sl6806/sl6806_stat.h).

⚠ The DWT cycle counter does not run on the measured unit; timekeeping falls
back to the 24-bit SysTick counter, which wraps every 262 ms at 64 MHz. Wraps
are only accumulated when the counter is read, so `sl6806-calibrate` and the
core both need to read it more often than that — see the "WHY NOT A SYSTICK
INTERRUPT" comment in
[`cores/sl6806/wiring_time.c`](../cores/sl6806/wiring_time.c) for the
reasoning and its consequences for your own `loop()`.

## Step 3 — serial input

The console is bidirectional. Type into the monitor and the characters arrive
at `Serial.read()`. `tools/sl6806-monitor --send TEXT` does it in one shot.

## What cannot work yet

Do not spend bench time on these; they are blocked on reverse engineering or
on an unexplained hardware result, not on your setup.

| | Why |
|---|---|
| Display output | The panel driver is written and checked against a model by 195 host-side tests. On hardware it initialises cleanly, completes every transfer, and produces no picture — cause still unknown. See [LCD.md](LCD.md). `GfxDemo` runs, draws into RAM, and shows nothing on the glass. |
| `digitalWrite` / `digitalRead` | The pad controller is recovered and works; what is missing is *this board's pinout* — only the two reset lines have known pad ids. `Blink` reports that instead of blinking. |
| `shiftOut` / `shiftIn` / `pulseIn` | Built on the digital calls, so exactly as available as those are. |
| `analogRead` / `analogWrite` / `tone` / `attachInterrupt` | No registers known. These report rather than silently doing nothing. |
| Touch panel coordinates | The interrupt pad and reset have run on hardware; the CST816 register read that produces X/Y has not been confirmed there yet. See `examples/TouchDemo`. |
| Camera | Fully decoded register-for-register, but blocked on the TWI 0 controller base — not something to bit-bang at 1 MP. |
| `MODE=firmware` | Unproven, and the only thing here that can leave a non-booting device. Nothing in this document needs it. See [FLASHING.md](FLASHING.md). |

## Recovery

Unplug, hold the boot key, plug back in.

Nothing in the payload workflow writes to flash, so there is nothing to
repair. Keep a `dump.bin` from `tools/sl6806-dumpram` and the flash dumper
anyway — it costs nothing and it is the only way back if you later try
`MODE=firmware`.

## What the tests do and do not cover

`make test` runs the native host suite — the console ring (including the
input path above), the drawing and panel stacks, the calibration estimator
against synthetic devices, and the other host tools' parsing and arithmetic —
plus `tests/emu`, which runs real built images under an ARM emulator
(needs `arm-none-eabi-gcc`).

The emulator models memory, not USB timing. That is exactly why the poll-mode
wedge described under "The one rule" got past a green test suite once before.
**Anything about how long the device spends inside the ROM's handler can only
be settled on the bench.**
