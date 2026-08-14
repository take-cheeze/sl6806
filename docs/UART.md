# The serial port at `0x40091000`, and how to listen to it

The SL6806 has a serial port, both vendor images drive it, and on hardware it
accepts bytes. This is how to receive them.

Driver: [`cores/sl6806/sl6806_uart.h`](../cores/sl6806/sl6806_uart.h). Notes:
§26 and the sections after it. 31 host tests.

## What is established, and what is not

| | |
|---|---|
| [V] | the block is at `0x40091000`, module clock 73, ROM clock 22 |
| [V] | its output pad is bank 1 pin 2, function 6 |
| [V] | the write path is ROM `0x1D0`: wait for `status & 0x1F`, store nine bits to `+0x00` |
| [M] | it accepts bytes — twenty per second, no timeouts, `examples/UartProbe` |
| [M] | `status & 0x1F` reads 1 at rest and 0 right after a burst, so bit 0 is transmit-idle |
| [I] | that it is a UART at all, rather than some other serial block |
| [?] | **the rate and the framing** — the vendor configures 1,500,000 against 48 MHz, but this driver does not program the divisor |
| [?] | **where bank 1 pin 2 comes out on the board** |

The last two are what stand between "the block accepts bytes" and "you can
read them", and they are bench problems rather than reading problems.

## Step 1: find the pin

There is no public pinout, and software cannot find it — a pad in an
alternate function is unreadable (see the LCD notes on the input buffer). So
drive it as a GPIO and look for it with a meter:

```sh
make SKETCH=examples/UartPin RUN_MODE=poll run
```

That drives bank 1 pin 2 high for five seconds, low for five, forever,
printing which half it is in. Multimeter on DC volts, black lead on the
board's ground, walk the red lead over test points and exposed pads: the one
that follows the printed state is the port's output.

It drives exactly one pad — the one the vendor drives as a UART output anyway
— and parks it again on unplug. This is **not** the pad sweep the notes warn
about; driving all 192 wedges the device because something USB needs is among
them.

## Step 2: wire it up

```
    board  bank 1 pin 2  ------->  adapter RX
    board  ground        -------   adapter GND
```

Two things to get right:

- **3.3 V logic.** Use a 3.3 V adapter, or level-shift. A 5 V adapter's RX
  input will usually read a 3.3 V signal fine, but do not connect its TX to
  the board without shifting.
- **Only TX is needed.** The board's output to the adapter's input. The
  adapter's TX is optional and only matters if you want to send *to* the
  board, which `sl6806_uart_getc()` supports.

## Step 3: the rate, which is the awkward part

The vendor's rate is **1,500,000 baud, and 8N1 is a guess**. Not every
adapter can do 1.5 Mbaud:

| Adapter | 1.5 Mbaud |
|---|---|
| FTDI FT232R / FT232H | yes, up to 3 M |
| CH340 / CH341 | yes, up to 2 M |
| CP2102**N** | yes, up to 3 M |
| CP2102 (the older one) | **no**, 1 M ceiling |
| PL2303 | varies by revision; often no |

Then:

```sh
picocom -b 1500000 /dev/ttyUSB0
# or
python3 -m serial.tools.miniterm /dev/ttyUSB0 1500000
# or
screen /dev/ttyUSB0 1500000
```

`stty` will refuse non-standard rates on some systems; pyserial sets them
through `termios2` and generally works where `stty` does not.

## Step 4: what you should see

Run `examples/UartProbe`. It prints a banner to USB, then enables the console
mirror, then sends `SL6806 uart probe N` once a second. Everything after the
mirror is enabled goes out both ways, so the adapter should show the same
text the USB console does.

Three outcomes:

1. **Clean text.** The port is real and this project has a console that
   cannot overflow and does not need the USB handler. Add
   `sl6806_uart_console_mirror(1)` to any sketch and its output stops being
   truncated.
2. **Activity but garbage.** The pin is right and the rate is not. A scope's
   bit period gives the real rate directly; failing that, try 115200, 230400,
   460800, 921600 and 3000000 in that order. Report the bit period and the
   divisor can be programmed properly — ROM `0x130`–`0x1A0` is the routine,
   and its fields are at CTRL bits 12 and 16.
3. **Nothing at all.** Either the pin is not where you connected, or bank 1
   pin 2 is not routed anywhere reachable on this board. `examples/UartPin`
   settles which.

## Using it in your own sketch

```c
#include "sl6806_uart.h"

void setup() {
    Serial.begin(115200);
    if (sl6806_uart_begin(0))          /* clocks, pad, enable */
        sl6806_uart_console_mirror(1); /* everything Serial prints, both ways */
}
```

The mirror refuses to install itself on a port that is not transmitting, so
this is safe to leave in a sketch you also run on a device with nothing
attached. `sl6806_uart_puts()` and `sl6806_uart_write()` are there if you want
to write to the port alone.

## Why this was worth building

Nine runs in the SD investigation opened with `[lost output - device outran
the poll rate]`. The USB console is a 2 KB ring that overwrites rather than
blocking, and in `RUN_MODE=poll` the host drains it only between `loop()`
calls — so any sketch that prints a register dump loses the top of it. Four
sketches in this tree are written as state machines emitting one section per
poll purely to work around that.

A serial port has no ring to overflow, and it keeps working when a payload
wedges the USB handler — which is the one failure the USB console cannot
report at all.
