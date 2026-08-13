/*
 * PadMap - a census of every pad on the board, and the card-detect hunt.
 *
 * ===================================================================
 *  WHY THIS IS NOT ANOTHER "WATCH THE BUS" SKETCH
 * ===================================================================
 * It cannot be one. examples/PadScope measured this and docs/LCD.md records
 * it: **the pad input buffer is switched off in every function nibble except
 * 0 and 14.** A pad muxed to a peripheral reads 0 whatever the pin is doing,
 * so no software test can watch a peripheral's bus on this chip. That table
 * cost the LCD investigation a detour, and it cost examples/SdPads a whole
 * hardware run - which asked whether the SD clock toggles on pin 12, in
 * function 2, and got "stuck low" from a disabled buffer.
 *
 * So this sketch stays inside the one mode where software can see: function
 * 0, a plain input, where a pull is visible and an external driver wins.
 *
 * ===================================================================
 *  [M] THE CENSUS, 2026-08-13 - AND WHAT 0 IN A FUNCTION NIBBLE MEANS
 * ===================================================================
 * The first run of this sketch produced the board's pad map, and it decides
 * which pads the pull test should be aimed at:
 *
 *     bank 0   pins 0..5    0,1,2,3 on function 2; 4,5 parked
 *     bank 1   pins 0..31   9 on function 3; the rest parked
 *     bank 2   pins 0..5    0,1 on function 2; 2..5 parked
 *     bank 3   pins 0..12   all parked
 *     bank 4   pins 0..16   all parked
 *     bank 5   pins 0..5    all parked
 *
 * Every bank has a contiguous run of function 15 from pin 0, and **every pin
 * above that run reads 0 in the function nibble**. Six banks all showing the
 * same shape is not a coincidence: `0` means the pin does not exist, `15`
 * means a real pad the boot ROM has parked. **Eighty pads are bonded out**,
 * and the ROM assigns exactly six of them.
 *
 * That is why the first version of this test measured nothing. It tested
 * "pads already in function 0" - which is to say, every pin that is not
 * there - and dutifully reported that all of them read 0 under both pulls.
 * The pads that exist are the parked ones, and those are what it tests now.
 *
 * ⚠ **The census is only the boot state on a freshly powered device.**
 * Uploading a payload does not reset the chip, so pads an earlier sketch
 * configured stay configured: two runs now have shown bank 1 pins 12, 13 and
 * 14 in function 0 with the input register reading 0x7000, which is
 * examples/SdPads having left them as pulled-up inputs, not anything the ROM
 * did. Power-cycle before trusting a census - and note that a function 0
 * inside a bank's configured run is the signature of exactly that.
 *
 * ===================================================================
 *  [M] TWO RUNS, AND EXACTLY ONE PIN DIFFERS
 * ===================================================================
 * Second and third runs, identical in every respect except one pin:
 *
 *     bank 1 pin 10:   HIGH (up=1 down=1)  ->  (up=0 down=1)
 *
 * Every other driven pin, and all forty-six floating ones, read the same
 * both times. So bank 1 pin 10 is the only thing on this board that changed
 * state between two runs of the same sketch.
 *
 * **It is also the pin the mask ROM waits for an edge on** (see below), which
 * makes it the boot key as much as it makes it card detect, and those are not
 * distinguishable from one log. Two things have to be controlled before it
 * means anything: whether a card was in the slot, and whether the boot key
 * was held. If the key was held for one run and not the other, this is the
 * boot key and nothing else.
 *
 * And `up=0 down=1` is not a state at all - it is the exact inverse of
 * following the pull, which is what a pad reads when it is sampled before it
 * has settled. The first version of this sketch read the input register in
 * the instruction after applying the pull. It now waits, and samples until
 * the level holds still; see pad_settled().
 *
 * ===================================================================
 *  [M] WHAT THE SECOND RUN FOUND, empty slot
 * ===================================================================
 * Forty-six parked pads follow their pull, so the mechanism works. The ones
 * that do not are driven from outside the chip:
 *
 *     bank 1 pin 0    LOW        bank 2 pins 2,3,4  LOW
 *     bank 1 pin 10   HIGH       bank 2 pin 5       HIGH
 *     bank 1 pins 16,17,19..31   LOW   (18 floats)
 *     bank 3 pin 0    LOW
 *
 * **Bank 1 pins 10 and 11 are the boot ROM's own inputs**, which is a lead
 * worth following independently of SD. ROM 0x3C1F0 configures pin 10 as
 * function 14 with pull selector 11 - a pull-up - then sets an interrupt
 * mode, enables it, and polls the pending bit 500 times; ROM 0x3C19C does
 * the same for pin 11 with selector 5, a pull-down. A pin with a pull-up
 * that the ROM waits for an edge on is an active-low button, and pin 10
 * reading HIGH here is that button not being pressed. docs/DUMPING.md still
 * says the boot key is "trial and error".
 *
 * That also explains PadScope's table: functions 0 and 14 are the two that
 * leave the input buffer alive because **both are input modes** - 0 plain,
 * 14 interrupt-capable.
 *
 * ===================================================================
 *  WHAT IT DOES
 * ===================================================================
 *   1. **A census, with no writes at all.** For all six banks, every pin's
 *      function nibble and its input level, exactly as found. Nobody has
 *      recorded what this board's pads are configured as in bootloader mode,
 *      and it is the missing half of README item 5 - the pad controller is
 *      done, the pinout is not.
 *
 *   2. **A pull test on the pads the boot ROM has parked.** For each pin in
 *      function 15 - a real pad the ROM has switched off, so nothing inside
 *      the chip is driving it - configure it as a plain input with a pull-up,
 *      read, with a pull-down, read, then park it again and put its pull
 *      back. Three outcomes per pin:
 *
 *        follows the pull    nothing is connected, or a high-impedance load
 *        stuck high          something outside drives it high
 *        stuck low           something outside drives it LOW
 *
 *      The last one is the interesting one. **A microSD socket's card-detect
 *      contact is a switch to ground that closes when a card is inserted**,
 *      so a pin that follows its pull with an empty slot and reads stuck low
 *      with a card in it is the card-detect pin - which nothing in the dump
 *      identifies (23) and which the SD driver currently substitutes with a
 *      pair of command timeouts.
 *
 * ===================================================================
 *  RUN IT TWICE AND DIFF
 * ===================================================================
 *     make SKETCH=examples/PadMap RUN_MODE=poll run      (empty slot)
 *     ... insert a microSD, then run it again
 *
 * Any pin that differs between the two logs is wired to the slot. That is
 * this board's SD pinout, and the card-detect contact, neither of which is
 * known. It is also the only card-presence test available, since the pads
 * carrying the bus are unreadable while they are muxed to the controller.
 *
 * ===================================================================
 *  WHAT IT WILL NOT DO
 * ===================================================================
 * **It never drives a pad, and it never touches a pad the boot ROM has
 * assigned to a peripheral** - six of them on this board, listed below. The
 * notes are explicit about why: driving all 192 pads wedges the device,
 * because something USB needs is among them, and a wedged device takes its
 * own console with it. A parked pad is driving nothing, and making it an
 * input with a pull for a few microseconds is the weakest intervention that
 * can still answer the question. Every pad is parked again afterwards and
 * every pull is restored.
 *
 * Output is paced one section per USB poll so nothing is lost.
 *
 * ===================================================================
 *  THE PULL TABLE, DECODED
 * ===================================================================
 * 7f found the table at ROM 0x0006501C and left its contents unread. Twelve
 * entries, selectors 4..15, each a word whose bits [1:0] go to the "pull a"
 * register at bank+0x24 and whose bits [17:16] go to "pull b" at bank+0x2C
 * (ROM 0x736):
 *
 *     selector   4    5    6    7    8    9   10   11   12   13   14   15
 *     pull a     1    1    0    0    2    0    0    2    3    3    3    3
 *     pull b     2    3    0    0    0    0    0    2    0    1    2    3
 *
 * Selector 8 is known to hold a plain input high (docs/LCD.md), so **a = 2 is
 * pull-up**; a = 1 is therefore [I] pull-down, and selector 4 is the only
 * clean pull-down in the table. a = 0 is no pull. a = 3 is [?] - possibly a
 * bus keeper, and deliberately not used here.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_mmio.h"
#include "sl6806_padctl.h"
#include "sl6806_console.h"
}

/* [V] 7f, offsets from a bank base. */
#define FUNC_REG(base, pin)  ((base) + 0x000u + ((pin) >> 3) * 4u)
#define IN_REG(base)         ((base) + 0x010u)
#define PULLA_REG(base, pin) ((base) + 0x024u + ((pin) >> 4) * 4u)
#define PULLB_REG(base, pin) ((base) + 0x02Cu + ((pin) >> 4) * 4u)

#define PULL_UP    11u    /* a = 2, b = 2 - the ROM's own choice for CMD/DAT */
#define PULL_DOWN   4u    /* a = 1, b = 2 - the only clean pull-down */

/* How long an internal pull gets to move a pad before it is read. A weak
 * resistor into whatever capacitance the board puts on the pin; 500 us is
 * far longer than that needs and still imperceptible across 69 pads. */
#define SETTLE_US  500u

static int  phase;
static int  bank;
static bool done;

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

static unsigned pad_func(uint32_t base, unsigned pin)
{
    return (sl6806_mmio_read(FUNC_REG(base, pin)) >> ((pin & 7u) * 4u)) & 0xFu;
}

static unsigned pad_level(uint32_t base, unsigned pin)
{
    return (sl6806_mmio_read(IN_REG(base)) >> pin) & 1u;
}

/*
 * Read a pad after giving it time to settle, and say whether it actually did.
 * Returns 0, 1, or 2 for "never stopped changing".
 *
 * THIS EXISTS BECAUSE THE FIRST VERSION DID NOT HAVE IT. It applied a pull
 * and read the input register in the next instruction, which measures the
 * level the pad had *before* the pull was applied on any pin with enough
 * capacitance to lag - a long trace, a switch, a connector. The signature is
 * a pin that reports the exact inverse of its pull (up=0, down=1), and one
 * appeared on the second run of this sketch. An internal pull is a weak
 * resistor; it needs microseconds, not nanoseconds.
 */
static unsigned pad_settled(uint32_t base, unsigned pin)
{
    unsigned first, i;

    delayMicroseconds(SETTLE_US);
    first = pad_level(base, pin);
    for (i = 0; i < 8u; i++) {
        delayMicroseconds(20);
        if (pad_level(base, pin) != first)
            return 2u;              /* still moving, or bouncing */
    }
    return first;
}

/* Save and restore the two 2-bit pull fields, so a pad is left as found. */
static uint32_t pull_save(uint32_t base, unsigned pin)
{
    unsigned sh = (pin & 15u) * 2u;

    return ((sl6806_mmio_read(PULLA_REG(base, pin)) >> sh) & 3u)
         | (((sl6806_mmio_read(PULLB_REG(base, pin)) >> sh) & 3u) << 16);
}

static void pull_restore(uint32_t base, unsigned pin, uint32_t saved)
{
    unsigned sh = (pin & 15u) * 2u;

    sl6806_mmio_field(PULLA_REG(base, pin), sh, 2, saved & 3u);
    sl6806_mmio_field(PULLB_REG(base, pin), sh, 2, (saved >> 16) & 3u);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 pad census ===");
    Serial.println("No pad is driven, and no pad the boot ROM assigned is");
    Serial.println("touched. Run once with an empty slot and once with a");
    Serial.println("card, and diff the two logs. Power-cycle first: pads a");
    Serial.println("previous sketch configured are still configured.");
    Serial.flush();

    phase = 0;
    bank = 0;
    done = false;
}

void loop()
{
    if (done || !drained())
        return;

    switch (phase) {

    /* --- the census, one bank per poll ---------------------------- */
    case 0: {
        uint32_t base = sl6806_pad_bank_base[bank];
        uint32_t in = sl6806_mmio_read(IN_REG(base));

        if (bank == 0) {
            Serial.println();
            Serial.println("--- every pad, as found ---");
            Serial.println("  f = function nibble, L = input level");
        }
        Serial.print("  bank ");
        Serial.print(bank);
        Serial.print(" base ");
        printHex(base);
        Serial.print("  input ");
        printHex(in);
        Serial.println();

        for (unsigned half = 0; half < 2u; half++) {
            Serial.print("    pins ");
            Serial.print(half * 16u);
            Serial.print("-");
            Serial.print(half * 16u + 15u);
            Serial.print("  f:");
            for (unsigned k = 0; k < 16u; k++) {
                unsigned pin = half * 16u + k;

                printHex(pad_func(base, pin), 1);
                Serial.print(' ');
            }
            Serial.print("  L:");
            for (unsigned k = 0; k < 16u; k++)
                Serial.print((in >> (half * 16u + k)) & 1u);
            Serial.println();
        }

        if (++bank >= SL6806_PAD_NBANKS) {
            phase++;
            bank = 0;
        }
        return;
    }

    /* --- the pull test, one bank per poll ------------------------- */
    case 1: {
        uint32_t base = sl6806_pad_bank_base[bank];
        unsigned n_tested = 0, n_absent = 0, n_follows = 0;

        if (bank == 0) {
            Serial.println();
            Serial.println("--- parked pads (function 15), pulled both ways ---");
            Serial.println("  'follows' = floating, 'HIGH'/'LOW' = driven from");
            Serial.println("  outside. A pin that follows with an empty slot");
            Serial.println("  and reads LOW with a card in it is card detect.");
        }

        Serial.print("  bank ");
        Serial.print(bank);
        Serial.print(": ");

        for (unsigned pin = 0; pin < 32u; pin++) {
            unsigned func = pad_func(base, pin);
            uint32_t saved_pull;
            unsigned up, down;

            /* Function 0 is either a pin that does not exist - above the
             * bank's bonded run - or a real pad an earlier payload left as
             * an input, since uploading does not reset the chip. Those look
             * identical from here, so both are counted and neither is
             * tested; the census above is what tells them apart. */
            if (func == 0u) {
                n_absent++;
                continue;
            }
            /* Anything the boot ROM has actually assigned is off limits:
             * six pads on this board, and something USB needs is among
             * them. Only parked pads are ours. */
            if (func != 15u)
                continue;

            saved_pull = pull_save(base, pin);

            /* Configure properly rather than poking the pull registers of a
             * parked pad: sl6806_pad_configure() is ROM 0x93C, and it sets
             * the function nibble, the drive and both pull fields. Writing
             * the pull alone leaves a parked pad parked, which is what the
             * first version of this sketch did and why it measured nothing.
             */
            sl6806_pad_configure(SL6806_PAD_ID(bank, pin, 0) | PULL_UP);
            up = pad_settled(base, pin);
            sl6806_pad_configure(SL6806_PAD_ID(bank, pin, 0) | PULL_DOWN);
            down = pad_settled(base, pin);

            /* Park it again, exactly as found, and put its pull back. */
            sl6806_pad_set_func(SL6806_PAD_ID(bank, pin, 15), 15u);
            pull_restore(base, pin, saved_pull);

            n_tested++;
            if (up == 1u && down == 0u) {
                n_follows++;
                continue;           /* floating: nothing connected */
            }

            Serial.print("pin ");
            Serial.print(pin);
            if (up == 2u || down == 2u)
                Serial.print(" UNSTABLE ");
            else if (up == 1u && down == 1u)
                Serial.print(" HIGH ");
            else if (up == 0u && down == 0u)
                Serial.print(" LOW ");
            else
                Serial.print(" INVERTED ");   /* still settling, or worse */
            Serial.print("(up=");
            Serial.print(up);
            Serial.print(" down=");
            Serial.print(down);
            Serial.print(")  ");
        }

        Serial.print("  [");
        Serial.print(n_tested);
        Serial.print(" parked pads tested, ");
        Serial.print(n_follows);
        Serial.print(" floating, ");
        Serial.print(n_absent);
        Serial.println(" in function 0 and skipped]");

        if (++bank >= SL6806_PAD_NBANKS)
            phase++;
        return;
    }

    /* --- what to do with it --------------------------------------- */
    default:
        Serial.println();
        Serial.println("=== reading this ===");
        Serial.println("  Diff this log against the other run. Any pin that");
        Serial.println("  changes is wired to the card slot.");
        Serial.println();
        Serial.println("  A pin that goes from 'follows' to LOW when a card");
        Serial.println("  is inserted is the card-detect switch, and it goes");
        Serial.println("  straight into sl6806_sd.h - the driver currently");
        Serial.println("  has to guess at card presence from two command");
        Serial.println("  timeouts.");
        Serial.println();
        Serial.println("  Pads whose function nibble is NOT 0 are the ones");
        Serial.println("  the boot ROM has already assigned to peripherals.");
        Serial.println("  That list is this board's pinout, and no one has");
        Serial.println("  written it down before.");
        Serial.println("done.");
        done = true;
        return;
    }
}
