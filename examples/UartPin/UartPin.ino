/*
 * UartPin - find where bank 1 pin 2 comes out on the board.
 *
 * ===================================================================
 *  THE PROBLEM THIS SOLVES
 * ===================================================================
 * The serial port at 0x40091000 transmits (notes §"it transmits"), and its
 * output pad is bank 1 pin 2. What nobody knows is **which physical pin or
 * test point that is** - this chip has no public pinout, and the pad census
 * cannot help because a pad in an alternate function is unreadable
 * (§"the pad input buffer is off").
 *
 * So: drive that one pad as a plain GPIO output and make it obvious to a
 * meter. Five seconds high, five seconds low, forever, with the current state
 * printed to USB so you always know which half you are in.
 *
 * A multimeter on DC volts is enough. Walk it over the board's test points
 * and pads with the black lead on ground: the pin that reads ~3.3 V for five
 * seconds and ~0 V for the next five is the one. An LED and a resistor works
 * too, and so does a scope, but the meter is the thing everyone has.
 *
 *     make SKETCH=examples/UartPin RUN_MODE=poll run
 *
 * ===================================================================
 *  IS THIS SAFE
 * ===================================================================
 * The notes carry a warning worth repeating: **do not sweep pads to find a
 * rail** - driving all 192 wedges the device, because something USB needs is
 * among them (§"do not sweep pads"). This is not that. It drives exactly one
 * pad, chosen because the vendor's own firmware drives it as a UART output,
 * so it is an output either way and this only changes what it carries.
 *
 * The pad is left parked (function 15) on the way out, which is how the boot
 * ROM leaves it.
 *
 * If the board turns out to drive that pin from the other end - nothing in
 * the dump suggests it does - the two would fight. Five seconds each way at
 * whatever drive strength a pad has is not enough to damage anything, but it
 * is why this toggles slowly rather than sitting hard at one level.
 *
 * ===================================================================
 *  ONCE YOU HAVE FOUND IT
 * ===================================================================
 * Wire that point to a USB-serial adapter's **RX**, grounds common, and run
 * examples/UartProbe at 1,500,000 8N1. docs/UART.md has the details,
 * including which adapters can do that rate - a good few cannot.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_uart.h"
#include "sl6806_padctl.h"
}

/* Bank 1 pin 2, as a plain output. The UART uses function 6 on the same pad
 * (SL6806_UART_PAD); function 1 is GPIO out, and bit 6 is the initial level. */
#define PIN_HIGH  (SL6806_PAD_ID(1, 2, SL6806_PAD_FUNC_OUTPUT) | (1u << 6))
#define PIN_LOW   (SL6806_PAD_ID(1, 2, SL6806_PAD_FUNC_OUTPUT))
#define PIN_PARK  (SL6806_PAD_ID(1, 2, 15))

static uint32_t last;
static int      level = -1;

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806: find bank 1 pin 2 ===");
    Serial.println();
    Serial.println("Multimeter on DC volts, black lead to ground. Walk the");
    Serial.println("red lead over test points and pads. The one that follows");
    Serial.println("the state printed below - about 3.3 V for five seconds,");
    Serial.println("then about 0 V for five - is the serial port's output.");
    Serial.println();
    Serial.println("This drives ONE pad, the one the vendor drives as a UART");
    Serial.println("output anyway. It is parked again when you unplug.");
    Serial.println();
    Serial.flush();

    sl6806_pad_configure(PIN_LOW);
    last = millis();
    level = 0;
    Serial.println("  LOW  (expect ~0 V)");
}

void loop()
{
    if (millis() - last < 5000u)
        return;
    last = millis();

    level = !level;
    sl6806_pad_configure(level ? PIN_HIGH : PIN_LOW);

    /* pad_configure sets the level from bit 6 of the id, but drive it through
     * the write path as well: a pad that was already in function 1 does not
     * necessarily re-latch its initial level, and this sketch's whole output
     * is that level being right. */
    sl6806_pad_write(SL6806_PAD_ID(1, 2, 0), level ? 1u : 0u);

    Serial.print("  ");
    Serial.println(level ? "HIGH (expect ~3.3 V)" : "LOW  (expect ~0 V)");
}
