/*
 * SdCtrl - the enable bits the reset takes away. Sweep CTRL after bring-up.
 *
 * ===================================================================
 *  THE OBSERVATION THIS IS BUILT ON
 * ===================================================================
 * examples/SdClock, 2026-08-13, one line in the middle of a negative result:
 *
 *     CTRL <- 0x40000003 (no reset bit)  reads 0x40000002
 *
 * Bit 1 stuck. **Bit 0 did not.** And after a full bring-up the same register
 * reads 0x40000000, with bit 1 clear as well.
 *
 * Put that next to what the mask ROM does. Its reset is one function,
 * ROM 0x3D88, and it is three instructions:
 *
 *     CTRL |= 7                  bits 0, 1 and 2 in a single write
 *     while (CTRL & 4) ;         wait for bit 2 to clear
 *
 * If bit 2 is a reset that clears the block - and the reading above says it
 * clears bit 1, because bit 1 holds when written on its own and does not
 * survive the sequence - then **the vendor sets two bits and immediately
 * wipes them**. Every bring-up in the dump does this, the ROM's and the
 * bootloader's both, and the transcription in cores/sl6806/sl6806_sd.c
 * faithfully reproduces it. Nothing in this project has ever set those bits
 * *after* the reset, because nothing in the vendor's code does.
 *
 * That is the whole hypothesis: the block is held disabled by its own
 * bring-up, and the fix is one write in an order the vendor never used.
 *
 * It is worth being honest about how it could be wrong. Bit 0 might be a
 * self-clearing kick that already did its job; bits [1:0] might be a
 * two-bit field where 3 is not a legal value; the ROM's order might be
 * right and the block disabled for an entirely different reason. All three
 * of those look identical from outside, and all three are answered by trying
 * every setting and watching one witness.
 *
 * ===================================================================
 *  WHAT THIS SKETCH DOES
 * ===================================================================
 *   1. Says which bits of CTRL are writable at all - one write of every bit
 *      except the reset, one read back. Nobody has recorded this, and it
 *      bounds every sweep below.
 *   2. Brings the controller up the ROM's way, then applies one candidate
 *      CTRL value, then sends CMD0. Sixteen combinations of the four bits
 *      the ROM ever touches - 0, 1, 3 and 25 - plus a pass that walks every
 *      writable bit on its own.
 *   3. Reports, for every attempt, three things and not one: what CTRL read
 *      back, **whether the command's start bit was ever seen latched**, and
 *      what the status register said. Those are different failures. "The
 *      start bit did not latch" means the command register is not live;
 *      "it latched and cleared with an empty status" means the controller
 *      ran the command and reported nothing.
 *
 * The witness stays CMD0 - no card, no data phase, no response.
 *
 *     make SKETCH=examples/SdCtrl RUN_MODE=poll run
 *
 * ===================================================================
 *  WHAT THE ANSWERS MEAN
 * ===================================================================
 *   - **A CTRL value makes CMD0 complete.** Then the SD host works, the
 *     driver gains one write, and the reason the vendor's own code never
 *     needed it becomes the interesting question rather than the blocker.
 *   - **The start bit never latches in any configuration.** Then the command
 *     register is not live no matter what CTRL says, and the block is gated
 *     by something outside itself - back to the power-cycle test in
 *     docs/SD.md, and then to the slot's supply.
 *   - **The start bit latches everywhere and the status stays zero.** Then
 *     the command goes out and the status register is what is broken, which
 *     would be the first time on this chip that a status register rather
 *     than a clock was the problem - and worth ruling in carefully, by
 *     watching the CMD line on a scope if one is to hand.
 */

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

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
 * One attempt. Bring the controller up, OR `extra` into CTRL afterwards,
 * send CMD0, and report everything that could distinguish one failure from
 * another.
 *
 * Returns 1 if the command completed.
 */
static int attempt(const char *label, uint32_t extra)
{
    uint32_t ctrl, cmd_first, sta, cmd_end;
    unsigned n;
    int latched, ok;

    if (sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT)
        != SL6806_SD_OK) {
        Serial.print("  ");
        Serial.print(label);
        Serial.println("   bring-up itself failed");
        return 0;
    }

    if (extra)
        sl6806_mmio_set(SL6806_SD_BASE + SL6806_SD_CTRL, extra);
    ctrl = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CTRL);

    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA, SL6806_SD_STA_CLEAR_ALL);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);

    /* The very next read. If the start bit is not here it was never taken,
     * and no amount of waiting afterwards can tell you that. */
    cmd_first = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CMD);
    latched = (cmd_first & 0x80000000u) ? 1 : 0;

    for (n = 0; n < 20000u; n++)
        if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
            & SL6806_SD_STA_CMDDONE)
            break;

    sta = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA);
    cmd_end = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CMD);
    ok = (sta & SL6806_SD_STA_CMDDONE) ? 1 : 0;

    Serial.print("  ");
    Serial.print(label);
    Serial.print("  CTRL ");
    printHex(ctrl);
    Serial.print("  start ");
    Serial.print(latched ? "LATCHED" : "no     ");
    Serial.print("  STA ");
    printHex(sta);
    Serial.print("  CMD ");
    printHex(cmd_end);
    Serial.println(ok ? "   *** CMD0 COMPLETED ***" : "");
    Serial.flush();

    return ok;
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 SD CTRL sweep ===");
    Serial.println("The mask ROM sets CTRL bits 0 and 1 in the same write as");
    Serial.println("the reset bit, and the reset appears to clear them. This");
    Serial.println("sets them afterwards instead. See the sketch header.");
    Serial.println();
    Serial.flush();

    done = false;
}

void loop()
{
    uint32_t writable = 0, ctrl;
    unsigned found = 0;

    if (done)
        return;
    done = true;

    /* --- 1: which bits of CTRL are writable? ---------------------- */

    Serial.println("--- which CTRL bits hold ---");
    sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT);

    for (unsigned bit = 0; bit < 32u; bit++) {
        uint32_t before, after;

        if (bit == 2u)
            continue;               /* the reset; poking it here proves nothing */

        before = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CTRL);
        sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CTRL,
                          before | (1u << bit));
        after = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CTRL);
        if (after & (1u << bit))
            writable |= 1u << bit;
        sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CTRL, before);
    }

    Serial.print("  writable mask ");
    printHex(writable);
    Serial.print("   bits:");
    for (unsigned bit = 0; bit < 32u; bit++)
        if (writable & (1u << bit)) {
            Serial.print(' ');
            Serial.print(bit);
        }
    Serial.println();
    Serial.println("  (bit 2, the reset, was skipped)");
    Serial.println();
    Serial.flush();

    /* --- 2: the sixteen combinations the ROM's four bits give ----- */

    Serial.println("--- CTRL bits set AFTER the reset ---");
    Serial.println("  0 and 1 are what the ROM sets alongside the reset;");
    Serial.println("  3 is what it toggles on its own (ROM 0x4668/0x4674);");
    Serial.println("  25 is what it sets before a transfer (ROM 0x4734).");
    Serial.println();

    found += attempt("baseline, nothing extra ", 0u);

    for (unsigned combo = 1; combo < 16u; combo++) {
        char label[24];
        uint32_t extra = 0;

        if (combo & 1u) extra |= 1u << 0;
        if (combo & 2u) extra |= 1u << 1;
        if (combo & 4u) extra |= 1u << 3;
        if (combo & 8u) extra |= 1u << 25;

        /* One character per bit, in the order 0 1 3 25, so the column
         * reads as a picture of what was set. */
        snprintf(label, sizeof(label), "bits %c%c%c%c        ",
                 (combo & 1u) ? '0' : '.',
                 (combo & 2u) ? '1' : '.',
                 (combo & 4u) ? '3' : '.',
                 (combo & 8u) ? 'H' : '.');

        found += attempt(label, extra);
    }
    Serial.println();

    /* --- 3: every writable bit on its own ------------------------- */

    Serial.println("--- every other writable bit, on its own ---");
    for (unsigned bit = 0; bit < 30u; bit++) {
        char label[24];

        if (bit == 2u || !(writable & (1u << bit)))
            continue;
        if (bit == 0u || bit == 1u || bit == 3u || bit == 25u)
            continue;               /* already covered above */

        snprintf(label, sizeof(label), "bit %-2u          ", bit);
        found += attempt(label, 1u << bit);
    }
    Serial.println();

    /* --- 4: and the reset done first, enables second -------------- */

    /*
     * The explicit form of the hypothesis: run the ROM's reset, wait it out,
     * and only then write the enables - rather than asking for both in one
     * store the way every bring-up in the dump does.
     */
    Serial.println("--- reset first, then the enables, as separate writes ---");
    sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CTRL, 0x00000004u);
    for (unsigned n = 0; n < 20000u; n++)
        if (!(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CTRL) & 4u))
            break;
    ctrl = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CTRL);
    Serial.print("  after a bare reset CTRL = ");
    printHex(ctrl);
    Serial.println();

    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CTRL, 0x40000003u);
    Serial.print("  then CTRL <- 0x40000003 reads ");
    printHex(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CTRL));
    Serial.println();

    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA, SL6806_SD_STA_CLEAR_ALL);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);
    Serial.print("  CMD right after the store ");
    printHex(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CMD));
    Serial.println();
    for (unsigned n = 0; n < 20000u; n++)
        if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
            & SL6806_SD_STA_CMDDONE)
            break;
    Serial.print("  STA ");
    printHex(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA));
    Serial.println((sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
                    & SL6806_SD_STA_CMDDONE) ? "   *** CMD0 COMPLETED ***"
                                             : "   no");

    Serial.println();
    Serial.print("=== ");
    Serial.print(found);
    Serial.println(" configuration(s) made CMD0 complete ===");
    Serial.println("If every line said 'start no', the command register is");
    Serial.println("not live whatever CTRL holds, and the next test is the");
    Serial.println("power cycle in docs/SD.md. If they all said LATCHED with");
    Serial.println("an empty status, the command went out and the status");
    Serial.println("register is the thing that is wrong.");
    Serial.println("done.");
}
