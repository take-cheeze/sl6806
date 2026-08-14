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
 *     hardware_init does that this does not.
 *
 * [M] 2026-08-14, the first run: **it booted.** The panel and the loss of the
 * upload endpoint both said so, which also confirms §7m's load address on
 * hardware. But the application's USB came up unstable - unstable enough that
 * `lsusb` could not be run against it - so the card question is still open,
 * and this sketch now runs the bootloader's clock sequence before handing
 * over. See FIRMBOOT_CLOCKS below.
 *
 * ===================================================================
 *  IF ITS USB IS UNSTABLE, TRY THE CABLE
 * ===================================================================
 * [M] It was, twice - once without the clock sequence and once with it. The
 * likeliest reason has nothing to do with what this sketch configures.
 *
 * **The application is re-initialising USB on a link that is already
 * enumerated.** In a normal boot the host has never seen this device: power
 * comes up, the bootloader runs, the application starts, and only then does
 * anything appear on the bus. Here the boot ROM has been enumerated as
 * `301a:2800` for the whole session, with an open connection and a host-side
 * device object, and the application reconfigures the controller underneath
 * all of that without a disconnect. A host presented with a device that stops
 * answering and starts answering differently, with no detach in between, does
 * exactly what you saw.
 *
 * **So unplug the USB cable after the jump and plug it back in.** This is a
 * player with a battery: the application keeps running, and the host gets a
 * clean enumeration of whatever the device now is. It costs nothing and needs
 * no code, and if `301a:2801` appears afterwards then the instability was the
 * handover and not the firmware.
 *
 * The alternative - detaching USB properly before jumping - needs the USB
 * controller's registers, and this project does not have them: 7l looked at
 * `0x40040000`, called it "most likely a USB controller", and left it there.
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
#include "sl6806_hwinit.h"
}

/*
 * ===================================================================
 *  THE TWO EXTRA STEPS ARE SEPARATELY SELECTABLE, AND BOTH DEFAULT OFF
 * ===================================================================
 * [M] Three results so far:
 *
 *   neither step            boots, and the application's USB is unstable
 *   both steps              does not boot at all
 *
 * "Does not boot" is a worse default than "boots badly", so both are off
 * unless you ask for them. Turning them on one at a time is the experiment,
 * and it takes two runs:
 *
 *     make SKETCH=examples/FirmBoot RUN_MODE=poll run \
 *          EXTRA_FLAGS=-DFIRMBOOT_MODULES_OFF=1
 *
 *     make SKETCH=examples/FirmBoot RUN_MODE=poll run \
 *          EXTRA_FLAGS=-DFIRMBOOT_CLOCK_TREE=1
 *
 * Whichever of those fails to boot is the one that breaks it, and that is
 * worth more than either of them working: it names a step the bootloader
 * performs that a payload cannot survive, which is the shape of the whole
 * remaining question about bootloader mode.
 *
 * **FIRMBOOT_MODULES_OFF** calls ROM 0x3BFC, the first thing hardware_init
 * does: every module clock on the chip off, then a delay. The application
 * expects a chip in that state and a payload hands it one where the boot
 * ROM's USB is enumerated and talking, which is the likeliest reason for the
 * unstable enumeration. Note that the copy above already happened, so flash
 * going quiet no longer matters - that ordering cost one run.
 *
 * **FIRMBOOT_CLOCK_TREE** calls the ROM routines behind 0x008206D0: the
 * source selects, the rate, two ROM-clock disables, two flash-host writes and
 * the four dividers. Any of those could be what a payload cannot survive -
 * the dividers change the core clock this code is executing on.
 */
#ifndef FIRMBOOT_MODULES_OFF
#define FIRMBOOT_MODULES_OFF 0
#endif
#ifndef FIRMBOOT_CLOCK_TREE
#define FIRMBOOT_CLOCK_TREE 0
#endif

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
    Serial.print("  PLL now      ");
    Serial.print(sl6806_pll_hz() / 1000000u);
    Serial.println(" MHz");
    Serial.print("  modules off  ");
    Serial.println(FIRMBOOT_MODULES_OFF ? "yes (ROM 0x3BFC)"
                                        : "no  (-DFIRMBOOT_MODULES_OFF=1)");
    Serial.print("  clock tree   ");
    Serial.println(FIRMBOOT_CLOCK_TREE ? "yes (0x008206D0's sequence)"
                                       : "no  (-DFIRMBOOT_CLOCK_TREE=1)");
    Serial.println();
    Serial.println("--- what to watch after the jump ---");
    Serial.println("  the panel: the P20 interface means it booted");
    Serial.println("  the host:  301a:2801 mass storage means the card works");
    Serial.println();
    Serial.println("  IF USB IS UNSTABLE: unplug the cable and plug it back");
    Serial.println("  in. The application keeps running on the battery, and");
    Serial.println("  the host gets a clean enumeration instead of a device");
    Serial.println("  that changed shape without detaching. See the header.");
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

    /*
     * COPY FIRST. The copy reads XIP flash, and the next step switches off
     * every module clock including the flash controller's. Doing it the other
     * way round copies garbage out of an unclocked controller and branches
     * into it - measured 2026-08-14, and the device simply did nothing.
     */
    {
        int err = sl6806_firm_copy(&firm);

        if (err != SL6806_FIRM_OK) {
            Serial.print("copy refused: ");
            Serial.println(why(err));
            parsed = 0;
            return;
        }
    }

    /*
     * hardware_init's order, with flash no longer needed. Both take the
     * console with them - USB's module clock is among the ninety-six - which
     * is why they come after the flush and immediately before the handover.
     */
#if FIRMBOOT_MODULES_OFF
    sl6806_hwinit_modules_off();
#endif
#if FIRMBOOT_CLOCK_TREE
    sl6806_hwinit_clocks();
#endif

    sl6806_firm_enter(&firm);

    /* Only reached if the entry refused, which parse should have prevented. */
    Serial.println("sl6806_firm_enter() refused - nothing was entered.");
    parsed = 0;
}
