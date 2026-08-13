/*
 * SdWall - the SD host is configured and does not run. Walk 0x400E0000.
 *
 * ===================================================================
 *  WHAT THE FIRST HARDWARE RUN SAID
 * ===================================================================
 * examples/SdProbe, 2026-08-13, on a P20 Player with no card in the slot:
 *
 *     module 36 reads ENABLED
 *     CTRL   0x40000000      CMD    0x0000A400
 *     DTIMER 0xFFFFFF40      CLKCR  0x20070008
 *     STA    0x00000000      STA2   0x00000106
 *     result: 3 (no response)   -- CMD0 never completed
 *
 * Read that dump again knowing what writes each of those values. CTRL is
 * exactly what ROM 0x3D9A leaves, CLKCR is exactly the identification value,
 * DTIMER is exactly ROM 0x3E22's constant, and **CMD holds 0xA400 - the CMD0
 * word from ROM 0x40F4, with the start bit cleared**. That is not a plausible
 * coincidence: the mask ROM brought this block up itself before the payload
 * started, sent CMD0, waited out its 65536-iteration poll, and gave up.
 *
 * Two things follow, and they point in opposite directions:
 *
 *   - **The decode is right.** Five registers hold the five values the
 *     transcription predicts, which is the same kind of evidence as the
 *     register file's rails decoding to 2.8 V.
 *   - **The transcription is not the problem.** The vendor's own driver, in
 *     mask ROM, running before anything else on the chip, fails at the same
 *     instruction. Nothing in cores/sl6806/sl6806_sd.c can fix that.
 *
 * Which puts this in the exact position the PWM (14a) and the camera front
 * end (7n) are in: a block that takes a full configuration and does not run.
 * For both of those the answer was a functional-clock bit in 0x400E0000 -
 * one bit per peripheral, in a register that reads as dead from a payload
 * until module 46 gates it open (7n, 17).
 *
 * ===================================================================
 *  WHAT THIS SKETCH DOES
 * ===================================================================
 *   1. Brings the controller up the mask ROM's way and sends CMD0. This is
 *      the baseline, and it is expected to fail - if it passes, something
 *      about the state the payload starts in matters and that is the finding.
 *   2. Opens the 0x400Exxxx group (sl6806_periph_group_begin(), which starts
 *      a PLL and has run on hardware - see 17 and examples/BtProbe), then
 *      sends CMD0 again. The group bring-up alone may be enough.
 *   3. Walks every bit of 0x400E0000 that is not already set. For each: set
 *      the bit, redo the controller bring-up, send CMD0, report whether the
 *      command-done bit finally sets, then put the register back.
 *   4. Tries the four differences between the mask ROM's sequence and the
 *      bootloader's, which are the only two versions of this bring-up that
 *      exist in the dump.
 *
 * The witness is one bit: does STA bit 2 set after a CMD0. That is a good
 * witness because CMD0 needs no card, no data path and no response - it is
 * the smallest thing the controller can be asked to do, and it is exactly
 * what is failing.
 *
 *     make SKETCH=examples/SdWall RUN_MODE=poll run
 *
 * A card in the slot is not needed and changes nothing. Nothing here writes
 * to a card.
 *
 * ===================================================================
 *  HOW TO READ THE RESULT
 * ===================================================================
 *   1. **One bit turns it on.** Then that bit is the SD host's functional
 *      clock, it goes in sl6806_sd.h next to module 36 and ROM clock 17, and
 *      the fifth attributed bit of 0x400E0000 is known.
 *   2. **The group bring-up alone turns it on.** Then the SD host needed the
 *      PLL, not a bit - which would also be worth knowing for the audio
 *      block, whose walk (examples/AudioWall) found nothing.
 *   3. **Nothing across 32 bits.** Then 0x400E0000 is not the SD host's
 *      problem either, and the failing thing is something the ROM does at
 *      power-on that a payload arrives too late for. The slot's supply is the
 *      first candidate - the register file (7m) carries five LDO rails and
 *      only one of them is attributed - and a rail that is off would explain
 *      why even the ROM's own CMD0 timed out.
 *
 * Print the whole log either way. A negative across 32 bits is worth as much
 * as a positive and is the reason examples/AudioWall exists.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_sd.h"
#include "sl6806_mmio.h"
#include "sl6806_module.h"
}

static bool done;

static void printHex(uint32_t v, int digits = 8)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = digits - 1; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

/*
 * The witness. Bring the controller up, acknowledge everything in the status
 * register, send CMD0, and say whether the command-done bit sets.
 *
 * Returns 1 if it did. Also reports, for the caller to print, the status word
 * and the command register - because "the start bit never latched" and "the
 * start bit latched and cleared with nothing to show for it" are different
 * failures and only the second one is a clock problem.
 */
static int cmd0(uint32_t edge, uint32_t clkcr, uint32_t *sta_out,
                uint32_t *cmd_out)
{
    unsigned n;

    if (sl6806_sd_controller_begin(edge, clkcr) != SL6806_SD_OK) {
        *sta_out = 0;
        *cmd_out = 0;
        return 0;
    }

    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA, SL6806_SD_STA_CLEAR_ALL);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);

    for (n = 0; n < 20000u; n++)
        if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
            & SL6806_SD_STA_CMDDONE)
            break;

    *sta_out = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA);
    *cmd_out = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CMD);
    return (*sta_out & SL6806_SD_STA_CMDDONE) ? 1 : 0;
}

static void report(const char *what, uint32_t edge, uint32_t clkcr)
{
    uint32_t sta, cmd;
    int ok = cmd0(edge, clkcr, &sta, &cmd);

    Serial.print("  ");
    Serial.print(what);
    Serial.print("  STA ");
    printHex(sta);
    Serial.print("  CMD ");
    printHex(cmd);
    Serial.println(ok ? "   *** CMD0 COMPLETED ***" : "   no");
    Serial.flush();
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 0x400E0000 walk, CMD0 as the witness ===");
    Serial.println("The SD host takes its whole configuration and never");
    Serial.println("completes a command - and neither does the mask ROM's");
    Serial.println("own driver. See this sketch's header and docs/SD.md.");
    Serial.println();
    Serial.flush();

    done = false;
}

void loop()
{
    uint32_t initial, sta, cmd;
    unsigned found = 0;

    if (done)
        return;
    done = true;

    /* --- 1: the baseline ------------------------------------------ */

    Serial.println("--- baseline: the mask ROM's own sequence ---");
    report("as the ROM does it ", 0u, SL6806_SD_CLKCR_IDENT);
    Serial.println();

    /* --- 2: the group gate ---------------------------------------- */

    Serial.println("--- with the 0x400Exxxx group open ---");
    Serial.print("  module 46: ");
    Serial.println(sl6806_periph_group_begin() ? "up" : "REFUSED");

    initial = sl6806_mmio_read(SL6806_PERIPH_ENABLE_REG);
    Serial.print("  0x400E0000 = ");
    printHex(initial);
    Serial.println();
    if (initial == 0) {
        Serial.println("  Zero means the gate did not open and the walk below");
        Serial.println("  cannot mean anything. Stop here and report this.");
    }
    report("group open         ", 0u, SL6806_SD_CLKCR_IDENT);
    Serial.println();
    Serial.flush();

    /* --- 3: the walk ---------------------------------------------- */

    Serial.println("--- walking 0x400E0000, one bit at a time ---");
    Serial.println("  Bits already set are skipped; each bit is put back");
    Serial.println("  before the next is tried.");
    Serial.println();

    for (unsigned bit = 0; bit < 32u; bit++) {
        int ok;

        if (initial & (1u << bit))
            continue;

        sl6806_mmio_set(SL6806_PERIPH_ENABLE_REG, 1u << bit);

        /* A bit that will not hold is not a bit that does anything, and
         * saying so is cheaper than a witness that could never fire. */
        if (!(sl6806_mmio_read(SL6806_PERIPH_ENABLE_REG) & (1u << bit))) {
            Serial.print("  bit ");
            Serial.print(bit);
            Serial.println("\t does not hold");
            sl6806_mmio_write(SL6806_PERIPH_ENABLE_REG, initial);
            continue;
        }

        ok = cmd0(0u, SL6806_SD_CLKCR_IDENT, &sta, &cmd);

        Serial.print("  bit ");
        Serial.print(bit);
        Serial.print("\t STA ");
        printHex(sta);
        Serial.print("  CMD ");
        printHex(cmd);
        Serial.println(ok ? "   *** CMD0 COMPLETED ***" : "");
        Serial.flush();

        if (ok)
            found++;

        sl6806_mmio_write(SL6806_PERIPH_ENABLE_REG, initial);
    }

    Serial.print("  0x400E0000 restored to ");
    printHex(sl6806_mmio_read(SL6806_PERIPH_ENABLE_REG));
    Serial.println();
    Serial.println();

    /* --- 4: the bootloader's version of the same bring-up ---------- */

    /*
     * The mask ROM and the HLKJ bootloader both configure this block, and
     * they differ in exactly four places. None of them is obviously a clock,
     * which is why the driver follows the ROM - but all four are cheap to
     * try, and the bootloader is the one of the two that actually reads
     * cards in the field.
     */
    Serial.println("--- the bootloader's four differences ---");

    report("CTRL[31:30] = 0xC0", 0x80000000u, SL6806_SD_CLKCR_IDENT);
    report("run clock now      ", 0u, SL6806_SD_CLKCR_RUN);
    report("both              ", 0x80000000u, SL6806_SD_CLKCR_RUN);

    sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT);
    sl6806_mmio_clr(SL6806_SD_BASE + SL6806_SD_REG18, 1u);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA, SL6806_SD_STA_CLEAR_ALL);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);
    for (unsigned n = 0; n < 20000u; n++)
        if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
            & SL6806_SD_STA_CMDDONE)
            break;
    sta = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA);
    Serial.print("  +0x18 bit 0 cleared  STA ");
    printHex(sta);
    Serial.println((sta & SL6806_SD_STA_CMDDONE) ? "   *** CMD0 COMPLETED ***"
                                                 : "   no");

    Serial.println();
    Serial.print("=== ");
    Serial.print(found);
    Serial.println(" bit(s) made CMD0 complete ===");
    if (!found) {
        Serial.println("A clean negative across the whole register. That");
        Serial.println("rules out this chip's usual answer, and points at");
        Serial.println("something done at power-on that a payload is too");
        Serial.println("late for - the slot's supply rail first. Read the");
        Serial.println("header, and report the whole log.");
    }
    Serial.println("done.");
}
