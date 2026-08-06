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

`examples/ClockCalibrate` is written this way and is the shortest example of
the pattern.

## Step 2 — the clock

`F_CPU` is a placeholder (120 MHz, unverified). Every `millis()`, `micros()`
and `delay()` is off by exactly the ratio between it and the truth, so one
measurement fixes all timing at once.

With any sketch loaded and running:

```sh
tools/sl6806-clockcal build/Hello.sym
```

It samples for thirty seconds and prints the measured clock, the ratio against
what the build assumed, and an error bar. Then rebuild:

```sh
make SKETCH=examples/Blink F_CPU=<measured> upload
```

Pass `F_CPU` from then on — it is a property of the chip, not of the sketch.

**How it works, and why it is not a sketch.** The device stamps its cycle
counter on every console poll, taken inside the USB transaction the host is
timing; the tool fits a line through those stamps against its own wall clock
and the slope is the clock. The device never blocks. The obvious alternative —
have the sketch `delay(1000)` thirty times while you hold a stopwatch — would
violate the one rule thirty times over.

## Step 3 — serial input

The console is bidirectional. Type into the monitor and the characters arrive
at `Serial.read()`. `tools/sl6806-monitor --send TEXT` does it in one shot.

## What cannot work yet

Do not spend bench time on these; they are blocked on reverse engineering, not
on your hardware.

| | Why |
|---|---|
| Display output | The panel is fully recovered and the controller is located, but the command-list opcodes are not decoded. `sl6806_panel_get()` returns NULL by design. `GfxDemo` runs, draws into RAM, and shows nothing. |
| `digitalWrite` / `digitalRead` | The GPIO registers are unknown. The mask ROM has been dumped and does not contain them, and the vendor's own GPIO routine is not resident in bootloader mode. `Blink` cannot blink; it reports instead. |
| `shiftOut` / `shiftIn` / `pulseIn` | Built on the digital calls, so exactly as available as those are. |
| `analogRead` / `analogWrite` / `tone` / `attachInterrupt` | No registers known. These report rather than silently doing nothing. |
| `MODE=firmware` | Unproven, and the only thing here that can leave a non-booting device. Nothing in this document needs it. See [FLASHING.md](FLASHING.md). |

## Recovery

Unplug, hold the boot key, plug back in.

Nothing in the payload workflow writes to flash, so there is nothing to
repair. Keep a `dump.bin` from `tools/sl6806-dumpram` and the flash dumper
anyway — it costs nothing and it is the only way back if you later try
`MODE=firmware`.

## What the tests do and do not cover

`make test` runs 226 checks: the console ring, the drawing and panel stacks,
the host tools' parsing and arithmetic, and real ARM images executed under
Unicorn.

The emulator models memory, not USB timing. That is exactly why the wedge
described under "The one rule" got past a green test suite. **Anything about
how long the device spends inside the ROM's handler can only be settled on the
bench.**
