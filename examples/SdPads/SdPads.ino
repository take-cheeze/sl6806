/*
 * SdPads - SUPERSEDED. Its premise was already known to be false.
 *
 * ===================================================================
 *  READ THIS BEFORE THE REST OF THE HEADER
 * ===================================================================
 * This sketch asks whether the SD card clock toggles on bank 1 pin 12 by
 * reading the pad input register while the pad is muxed to function 2. **It
 * cannot work, and the reason was measured and written down before this was
 * written**: examples/PadScope swept all sixteen function nibbles across
 * eight pads and found that the pad input buffer is switched off in every
 * function except 0 and 14 (notes, and docs/LCD.md, which says in as many
 * words: "no software test can watch that bus. Anyone tempted to write
 * another one should read this table first").
 *
 * Run on hardware 2026-08-13, it duly reported pin 12 "stuck low" in every
 * phase that used function 2 - which is what a disabled input buffer reads,
 * not what a parked clock reads. Those three phases mean nothing.
 *
 * What the run did establish, in its fourth phase, is real and worth keeping:
 * with pins 12/13/14 taken back to function 0 under the ROM's own pull
 * selector, all three read HIGH. **The pads exist and their pull-ups work.**
 *
 * The general conclusion is the same as the LCD's: software is blind to this
 * bus. Watching the SD clock needs a probe on the socket, not another sketch.
 * examples/PadMap is the sketch that stays inside what software can see.
 *
 * The original header follows, kept because the reasoning in it is a good
 * example of a plausible test built on an assumption nobody re-checked.
 *
 * ===================================================================
 * SdPads - is the card clock actually coming out of the chip?
 *
 * ===================================================================
 *  WHAT SdLife ESTABLISHED
 * ===================================================================
 * 2026-08-13, and it is the first positive result this block has produced:
 *
 *     STA2 before 0x00000106   rx-empty 1  tx-full 0
 *     pushed 16 words, STA2 now 0x00200109
 *     *** THE FLAGS MOVED ***
 *
 * Sixteen words went into the data port at +0x200 and the block noticed: the
 * empty flag cleared, the full flag set, and a counter appeared in the high
 * half. **The FIFO is sixteen words deep and the datapath has a clock.** So
 * the core is not stopped, which is what everything up to that point had been
 * circling, and the fault is specifically in the command path.
 *
 * Nothing moved on its own across 200 passes over 72 registers, which is
 * consistent - with no transfer running there is nothing to count - and all
 * eight bits of CLKCR's [30:23] field hold and none of them starts a command.
 *
 * ===================================================================
 *  THE QUESTION THIS ASKS
 * ===================================================================
 * A command shifts out on the card clock. The datapath runs on the core
 * clock. Those are different clocks, and SdLife only proved the second one.
 * So: **is the card clock actually coming out of the chip, on a pin?**
 *
 * That sounds like it needs an oscilloscope. It does not, because this chip's
 * pad controller has an input register that reads the physical level of a
 * pad **regardless of what function the pad is muxed to** (7f: +0x010, one
 * bit per pin). So a pad driven by the SD controller can still be read by the
 * CPU, and a pin carrying a clock, sampled a few thousand times, comes back
 * as a mixture of ones and zeros. A pin that is parked comes back constant.
 *
 * Four things this can settle, none of which needs a scope:
 *
 *   1. **Does the clock pad toggle?** Bank 1 pin 12, the one the mask ROM
 *      configures with no pull, sampled while the controller is up and again
 *      while commands are being issued. Toggling means the clock is running
 *      and the fault is further out. Constant means the controller is not
 *      driving its clock, which is a hardware-level statement that no
 *      register dump has been able to make.
 *
 *   2. **Do the other two pads sit high?** Pins 13 and 14 are configured with
 *      pull selector 11, which is what a CMD and a DAT0 line want. If they
 *      read low with that pull applied, either the pull is not what it looks
 *      like or those are not the pads - and this board's SD pinout would then
 *      be the thing to find.
 *
 *   3. **Does inserting a card change anything?** A card holds DAT lines up
 *      through its own pull-ups. Run this once with a card and once without
 *      and diff the two logs: any pin that differs is wired to the slot, and
 *      that is a pinout discovery whatever else it settles. **This is also
 *      the card-detect pin hunt** - a slot's CD contact is a pin that changes
 *      when a card goes in and does nothing else.
 *
 *   4. **Which pads move at all.** The whole of bank 1 is sampled, not just
 *      the three the ROM names, so a card that is wired to some other pins
 *      announces itself.
 *
 * ===================================================================
 *  HOW TO RUN IT
 * ===================================================================
 *     make SKETCH=examples/SdPads RUN_MODE=poll run       (no card)
 *     ... then insert a card and run it again
 *
 * Keep both logs. The diff between them is the result; either log on its own
 * is half of it.
 *
 * Nothing here writes to a card, and nothing here drives a pad: every pad is
 * either left in the function the SD bring-up gave it or read as an input.
 * Output is paced one section per USB poll, so nothing is lost.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_sd.h"
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"
#include "sl6806_console.h"
}

/* Bank 1's input register. 7f: bank base + 0x010, one bit per pin. */
#define BANK1_IN (sl6806_pad_bank_base[1] + 0x010u)

#define SAMPLES 4000u

static int  phase;
static bool done;

/* What the sampler saw, per pin: the bits that were ever 1 and ever 0. */
static uint32_t ever_high, ever_low;

static void printHex(uint32_t v, int digits = 8)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = digits - 1; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

static bool drained()
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

/* Sample bank 1 as fast as the bus allows, accumulating which bits were ever
 * seen high and which ever low. A pin carrying a clock ends up in both. */
static void sample(unsigned n)
{
    ever_high = 0;
    ever_low = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t v = sl6806_mmio_read(BANK1_IN);

        ever_high |= v;
        ever_low |= ~v;
    }
}

static void report(const char *what)
{
    uint32_t toggled = ever_high & ever_low;

    Serial.print("    ");
    Serial.println(what);
    Serial.print("      ever high ");
    printHex(ever_high);
    Serial.print("   ever low ");
    printHex(ever_low);
    Serial.println();
    Serial.print("      TOGGLED   ");
    printHex(toggled);
    if (toggled) {
        Serial.print("   pins:");
        for (unsigned p = 0; p < 32u; p++)
            if (toggled & (1u << p)) {
                Serial.print(' ');
                Serial.print(p);
            }
    }
    Serial.println();

    /* The three the mask ROM configures, called out by name. */
    Serial.print("      pin 12 (clock) ");
    Serial.print((toggled & (1u << 12)) ? "TOGGLING"
                 : (ever_high & (1u << 12)) ? "stuck high" : "stuck low");
    Serial.print(", pin 13 ");
    Serial.print((toggled & (1u << 13)) ? "toggling"
                 : (ever_high & (1u << 13)) ? "high" : "low");
    Serial.print(", pin 14 ");
    Serial.println((toggled & (1u << 14)) ? "toggling"
                   : (ever_high & (1u << 14)) ? "high" : "low");
    Serial.flush();
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 SD pads: is the card clock on a pin? ===");
    Serial.println("Run once with a card in the slot and once without, and");
    Serial.println("keep both logs - the diff is the result.");
    Serial.flush();

    phase = 0;
    done = false;
}

void loop()
{
    if (done || !drained())
        return;

    switch (phase) {

    /* --- 0: before the SD bring-up touches anything --------------- */
    case 0:
        Serial.println();
        Serial.println("--- bank 1, before the SD pads are configured ---");
        sample(SAMPLES);
        report("as found:");
        phase++;
        return;

    /* --- 1: with the ROM's pad functions applied ------------------ */
    case 1:
        Serial.println();
        Serial.println("--- with the controller up and the pads on func 2 ---");
        sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT);
        sample(SAMPLES);
        report("controller up, idle:");
        phase++;
        return;

    /* --- 2: while commands are being issued ----------------------- */
    case 2: {
        /*
         * Some hosts only run the clock while a command is in flight. Issue
         * CMD0 repeatedly and sample in between, so a clock that only exists
         * during a transfer still shows up.
         */
        uint32_t high = 0, low = 0;

        Serial.println();
        Serial.println("--- while CMD0 is being issued, over and over ---");
        for (unsigned rep = 0; rep < 64u; rep++) {
            sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA,
                              SL6806_SD_STA_CLEAR_ALL);
            sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
            sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);
            sample(64u);
            high |= ever_high;
            low |= ever_low;
        }
        ever_high = high;
        ever_low = low;
        report("during commands:");
        phase++;
        return;
    }

    /* --- 3: the same pads read as plain inputs -------------------- */
    case 3:
        /*
         * Take the three pads away from the SD controller and read them as
         * inputs with the ROM's own pull selector. This separates "the
         * controller is not driving" from "the line is not there": a CMD or
         * DAT line with a pull-up, and a card in the slot, must read high.
         *
         * Bank 1 pins 12, 13 and 14, function 0, pull 11 - the ROM's pull,
         * the input function.
         */
        Serial.println();
        Serial.println("--- pins 12/13/14 as inputs with the ROM's pull ---");
        sl6806_pad_configure(0x0001600Bu);   /* pin 12, func 0, pull 11 */
        sl6806_pad_configure(0x0001680Bu);   /* pin 13 */
        sl6806_pad_configure(0x0001700Bu);   /* pin 14 */
        sample(SAMPLES);
        report("as pulled inputs:");
        Serial.println("      A card in the slot holds its CMD and DAT lines");
        Serial.println("      up through its own pull-ups, so a pin that");
        Serial.println("      reads high here and low with the slot empty is");
        Serial.println("      wired to the card.");
        phase++;
        return;

    /* --- 4: what to do with it ------------------------------------ */
    default:
        Serial.println();
        Serial.println("=== reading this ===");
        Serial.println("  pin 12 TOGGLING: the card clock is leaving the");
        Serial.println("  chip, the controller is running it, and the fault");
        Serial.println("  is on the far side - wiring, or the wrong pads.");
        Serial.println();
        Serial.println("  pin 12 stuck: the controller is not driving its");
        Serial.println("  clock at all, which no register dump could have");
        Serial.println("  told us, and the SD block's clock enable is still");
        Serial.println("  missing somewhere.");
        Serial.println();
        Serial.println("  Any pin that differs between the card-in and");
        Serial.println("  card-out runs is wired to the slot - which is");
        Serial.println("  this board's SD pinout, and the card-detect pin,");
        Serial.println("  neither of which is known.");
        Serial.println("done.");
        done = true;
        return;
    }
}
