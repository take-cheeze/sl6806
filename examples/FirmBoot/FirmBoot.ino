/*
 * FirmBoot - start the vendor's application from a payload.
 *
 * ===================================================================
 *  READ THIS FIRST
 * ===================================================================
 * This hands the device to the stock firmware and does not come back. The
 * USB link goes with it - the application re-initialises USB as a card
 * reader - so everything this run has to say is said before it jumps, and
 * what happens afterwards is observed on the device and on the host's USB
 * bus, not in this console.
 *
 * **It writes nothing anywhere persistent.** The application is copied out of
 * the flash image already on the device into SRAM, and entered. A power cycle
 * puts you back in bootloader mode with nothing changed. That is the whole
 * difference between this and MODE=firmware, and it is why this exists first.
 *
 *     make SKETCH=examples/FirmBoot RUN_MODE=poll run
 *
 * ===================================================================
 *  WHY BOTHER
 * ===================================================================
 * To find out what bootloader mode withholds.
 *
 * The SD host takes a command and reports nothing, with seven hypotheses
 * closed by measurement (docs/SD.md): the gates are right, the pads are the
 * product's own, the datapath is clocked, the command register is live.
 * What is left is that a payload runs in bootloader mode and the code that
 * demonstrably reads cards - the bootloader's own sdupdate path - does not.
 * Measured this session: **in bootloader mode the mask ROM does not touch
 * the SD host at all**, and it does not touch the serial port either. Two
 * blocks, both off, both fine once a payload wakes them - and one of them
 * still will not run a command.
 *
 * So: does the application boot, and does the card mount?
 *
 *   - **The panel lights up with the P20 interface.** The application is
 *     running. That alone says the flash image is intact and startable from
 *     a payload, which nothing has established before.
 *   - **The host enumerates 301a:2801, mass storage, and the card mounts.**
 *     The socket, the card and the SD host all work under the vendor's
 *     firmware. The SD question then stops being "is the block broken" and
 *     becomes "what does bootloader mode withhold", which is a much better
 *     question because it has a finite answer.
 *   - **Nothing happens.** The application needs something the bootloader's
 *     hardware_init does that this does not - the PLL at 384 MHz is the
 *     first candidate (notes §25) - and the next version of this sketch does
 *     that first.
 *
 * ===================================================================
 *  WHAT IT CHECKS BEFORE JUMPING
 * ===================================================================
 * The load address is not in the header. The header names an entry, and the
 * load address is that entry less the 0x400-byte vector table - §7m's
 * finding, confirmed there by counting call targets. Getting it wrong copies
 * the image 0x400 bytes off and branches into the middle of a function, and
 * on a Cortex-M that is a hard fault with the USB link already gone: no
 * console, no message, just a device that stopped.
 *
 * So sl6806_firm_parse() derives the load address and then checks it against
 * the image: the segment's word 1 must be the entry the header named, and
 * word 0 must be a plausible SRAM stack pointer. Both hold for the shipped
 * image. 15 host tests cover the refusals.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_firm.h"
}

/* Seconds of grace before the jump, so the console can be read and the run
 * abandoned by unplugging. */
#define COUNTDOWN 8

static sl6806_firm_t firm;
static int parsed;
static int left = COUNTDOWN;
static uint32_t last;

static void printHex(uint32_t v)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = 7; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

static const char *why(int err)
{
    switch (err) {
    case SL6806_FIRM_OK:          return "ok";
    case SL6806_FIRM_ERR_HDR:     return "not a FIRM header";
    case SL6806_FIRM_ERR_ENTRY:   return "the entry is not a Thumb SRAM address";
    case SL6806_FIRM_ERR_LENGTH:  return "the length is zero or absurd";
    case SL6806_FIRM_ERR_VECTORS: return "the vector table disagrees with the header";
    default:                      return "unknown";
    }
}

void setup()
{
    int err;

    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806: start the stock application ===");
    Serial.println();
    Serial.println("This does not come back. USB goes away with it, and the");
    Serial.println("device becomes the product. Nothing is written to flash;");
    Serial.println("a power cycle undoes all of it.");
    Serial.println();

    err = sl6806_firm_read(&firm);
    parsed = (err == SL6806_FIRM_OK);

    Serial.println("--- the image in flash ---");
    Serial.print("  parse: ");
    Serial.print(err);
    Serial.print(" (");
    Serial.print(why(err));
    Serial.println(")");

    if (!parsed) {
        Serial.println();
        Serial.println("  Refusing to jump. Nothing was copied.");
        return;
    }

    Serial.print("  build stamp   ");
    printHex(firm.timestamp);
    Serial.println();
    Serial.print("  entry         ");
    printHex(firm.entry);
    Serial.println("   (Thumb)");
    Serial.print("  load address  ");
    printHex(firm.load);
    Serial.println("   (entry less the vector table)");
    Serial.print("  length        ");
    printHex(firm.length);
    Serial.println();
    Serial.print("  initial SP    ");
    printHex(firm.stack);
    Serial.println("   (from vector 0)");
    Serial.println();
    Serial.println("  The vector table agrees with the header, so the load");
    Serial.println("  address is right - see the sketch header for why that");
    Serial.println("  is the thing worth checking.");
    Serial.println();
    Serial.println("--- what to watch after the jump ---");
    Serial.println("  the panel: the P20 interface means it booted");
    Serial.println("  the host:  301a:2801 mass storage means the card works");
    Serial.println("  neither:   it needs more of hardware_init than this does");
    Serial.println();
    Serial.print("Jumping in ");
    Serial.print(COUNTDOWN);
    Serial.println(" seconds. Unplug now to abandon.");
    Serial.flush();

    last = millis();
}

void loop()
{
    if (!parsed)
        return;

    if (millis() - last < 1000u)
        return;
    last = millis();

    if (left > 0) {
        Serial.print("  ");
        Serial.println(left);
        Serial.flush();
        left--;
        return;
    }

    Serial.println();
    Serial.println("going.");
    Serial.flush();

    /* Give the host one more poll to collect that before USB disappears. */
    delay(50);

    sl6806_firm_boot(&firm);

    /* Only reached if the boot refused, which parse should have prevented. */
    Serial.println("sl6806_firm_boot() refused - nothing was entered.");
    parsed = 0;
}
