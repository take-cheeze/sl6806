/*
 * PadScope - which pad functions leave the input buffer alive?
 *
 * The LCD bring-up is stuck on an ambiguity. Bank 1 pins 1-8 in function 2
 * read 0 forever, even with a pull-up applied, and there are two readings:
 *
 *   a) the input buffer is switched off for a pad in an alternate function,
 *      so the register cannot see the pin whatever it does; or
 *   b) the buffer works and the LCD controller is holding the pad low.
 *
 * Those need opposite next steps. So sweep: put each pad through every
 * function nibble with a pull-up applied and read it back. Function 0 is the
 * control - the buffer is known to work there, so it must read 1.
 *
 *   - every non-zero function reads 0  -> the buffer is off outside GPIO
 *     mode. Software can never watch this bus.
 *   - some read 1 and function 2 reads 0 -> the buffer survives, and
 *     something is pulling function 2's pad down: the controller is alive on
 *     those pins.
 *   - function 2 reads 1 like its neighbours -> nothing drives the pad in
 *     function 2 at all, so function 2 is not what connects this controller
 *     and the mux is wrong.
 *
 * ---------------------------------------------------------------------
 *  WHY THIS IS A STATE MACHINE AND NOT A LOOP IN setup()
 * ---------------------------------------------------------------------
 * The first version did the whole sweep in setup() and produced *nothing* -
 * not one line, not even the banner. The reason is structural and worth
 * knowing before writing any other probe for this framework:
 *
 *     startup_payload.c installs the USB handler AFTER setup() returns.
 *
 * Serial output goes into a ring buffer that only the host's polling drains,
 * and the host can only poll once that handler exists. So anything setup()
 * prints is unreadable until setup() finishes, and a setup() that hangs or
 * faults takes its own diagnostics down with it.
 *
 * Hence: setup() returns immediately, and the sweep advances one step per
 * loop() call. Each step announces what it is about to do and returns, so the
 * host has drained that line before the risky write happens. If a particular
 * pad function wedges the device, the last line printed names it.
 *
 * Only bank 1 pins 1-8 are touched - the panel's own pads - so a mistake here
 * cannot take out USB or flash.
 */

#include <Arduino.h>
#include "sl6806_padctl.h"
#include "sl6806_mmio.h"
#include "sl6806_lcdc.h"

/* Measured on this board: the selector that holds a floating input high. */
#define PULL_UP 8u

static sl6806_color_t band[64 * 8];
static uint32_t bank1;

/* Sweep position. Each (pin, function) takes two loop() calls: announce,
 * then act. */
static unsigned pin = 1, fn = 0;
static bool announced;
static bool done;

static int readPin(unsigned p)
{
    return (int)((sl6806_mmio_read(bank1 + 0x10) >> p) & 1u);
}

/* Put a pad back to what the display driver wants. */
static void restore(unsigned p)
{
    if (p == 7)
        sl6806_pad_configure(0x000138CBu);
    else
        sl6806_pad_configure(SL6806_PAD_ID(1, p, 2));
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 pad function scope ===");
    Serial.println("For each bank 1 pad and function, with a pull-up applied:");
    Serial.println("does the input register still see the pin?");
    Serial.println("Function 0 is the control and must read 1.");
    Serial.println("Function 1 is an output and is skipped.");

    bank1 = sl6806_pad_bank_base[1];

    /* Bring the controller up, so the sweep happens in exactly the state the
     * panel refuses to work in. */
    Screen.begin(band, 64, 8);
    Serial.print("lcd bus: ");
    Serial.println(sl6806_lcd_bus() ? sl6806_lcd_bus()->name : "none");
    Serial.println();
}

void loop()
{
    if (done)
        return;

    if (fn == 1) {                 /* output mode says nothing about the buffer */
        fn++;
        return;
    }

    if (!announced) {
        Serial.print("pin ");
        Serial.print(pin);
        Serial.print(" fn ");
        Serial.print(fn);
        Serial.print(" ... ");
        announced = true;
        return;                    /* let the host drain that before we act */
    }

    sl6806_pad_configure(SL6806_PAD_ID(1, pin, fn) | PULL_UP);
    delayMicroseconds(200);
    Serial.println(readPin(pin));

    announced = false;
    if (++fn > 15) {
        fn = 0;
        restore(pin);
        if (++pin > 8) {
            done = true;
            Serial.println();
            Serial.print("all pads restored; bank 1 input now 0x");
            Serial.println(sl6806_mmio_read(bank1 + 0x10), HEX);
            Serial.println("=== done ===");
        }
    }
}
