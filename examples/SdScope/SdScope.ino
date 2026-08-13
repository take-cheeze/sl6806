/*
 * SdScope - stop guessing which bit. Diff the whole block around one CMD0.
 *
 * ===================================================================
 *  WHAT THE LAST THREE RUNS ESTABLISHED
 * ===================================================================
 * examples/SdCtrl, 2026-08-13, thirty-odd configurations of CTRL, and every
 * single line said the same thing:
 *
 *     start LATCHED   STA 0x00000000   CMD 0x0000A400
 *
 * **The command register is live.** The start bit is taken when it is
 * written, and by the time the poll gives up it has cleared again. So the
 * block is not gated, not unclocked in any way that stops it accepting a
 * command, and not waiting: it takes the command, drops the busy bit, and
 * the status register says nothing happened.
 *
 * That is the third of the three outcomes SdCtrl was written to distinguish,
 * and it is the one that says the remaining fault is not where anyone has
 * been looking. Three hypotheses have been closed by measurement now:
 *
 *     0x400E0000     seven bits wide, all seven tried          (SdWall)
 *     the clock      64 settings, found already enabled        (SdClock)
 *     CTRL           every writable bit, before and after reset (SdCtrl)
 *
 * SdCtrl also mapped CTRL: only bits **1, 3, 4, 24, 25** hold, and **bit 0
 * is not writable at all** - which retires the "the reset wipes the enable
 * bits" idea for bit 0, since there is no bit 0 to wipe.
 *
 * ===================================================================
 *  SO STOP PROPOSING BITS AND MAP THE BLOCK
 * ===================================================================
 * Every sketch so far has tested one hypothesis with one witness. This one
 * has no hypothesis. It snapshots all 72 registers of the block, sends one
 * CMD0, snapshots them again, and prints every word that changed.
 *
 * That is the method that found the DVP's DMA destination register (7n) and
 * it is the right one here, because the question has become "does this block
 * do *anything* when commanded" rather than "which bit enables it". Three
 * outcomes:
 *
 *   1. **Something changes.** Then the block executes and the status is
 *      somewhere other than +0x34 - the offset comes from ROM 0x4C90 and is
 *      not in doubt, but a register that moves is a fact and a decode is an
 *      argument. Whatever moved is the next thing to read.
 *   2. **Nothing changes anywhere except CMD's own busy bit.** Then the
 *      block latches commands and does not execute them, which is a much
 *      narrower statement than "it does not work" and points outside the
 *      block - at the slot's supply, or at a card-detect input.
 *   3. **The dump is all zeros or all ones.** Then this is not the block,
 *      and 23's identification is wrong somewhere.
 *
 * It also sweeps the five CLKCR bits **no bring-up in the dump ever writes**:
 * the vendor's read-modify-write clears 0x7FFF0FFF and leaves bits 12..15 and
 * 31 alone, so nothing has ever set them and nobody knows what they are.
 *
 *     make SKETCH=examples/SdScope RUN_MODE=poll run
 *
 * ===================================================================
 *  RUN IT TWICE: ONCE WITH A CARD, ONCE WITHOUT
 * ===================================================================
 * This has not been controlled for, and it is free to control for. CMD0
 * needs no card *in principle* - it expects no response and the controller
 * only has to shift 48 bits out. But a host with a card-detect input can
 * legitimately refuse to run its clock into an empty slot, and if that is
 * what is happening here then every run so far has been asking a controller
 * to talk to a slot it knows is empty.
 *
 * The card-detect pin is not known (docs/SD.md), so this cannot be checked
 * in software. Two runs and a microSD check it in about a minute.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_sd.h"
#include "sl6806_mmio.h"
#include "sl6806_module.h"
}

/* 0x000..0x0FC is the block proper, 0x100..0x11C is the sub-block the clock
 * setup touches. The FIFO at 0x200 is deliberately NOT read: reading a data
 * port has side effects, and this sketch is supposed to observe. */
#define N_LOW   64u     /* words at 0x000..0x0FC */
#define N_HIGH  8u      /* words at 0x100..0x11C */
#define N_REGS  (N_LOW + N_HIGH)

static bool done;
static uint32_t before[N_REGS], after[N_REGS];

static void printHex(uint32_t v, int digits = 8)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = digits - 1; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

static uint32_t off_of(unsigned i)
{
    return (i < N_LOW) ? (i * 4u) : (0x100u + (i - N_LOW) * 4u);
}

static void snapshot(uint32_t *into)
{
    for (unsigned i = 0; i < N_REGS; i++)
        into[i] = sl6806_mmio_read(SL6806_SD_BASE + off_of(i));
}

/* Print every register that differs, and say how many did. */
static unsigned diff(const char *what)
{
    unsigned n = 0;

    Serial.print("  ");
    Serial.println(what);
    for (unsigned i = 0; i < N_REGS; i++) {
        if (before[i] == after[i])
            continue;
        Serial.print("    +");
        printHex(off_of(i), 3);
        Serial.print("  ");
        printHex(before[i]);
        Serial.print("  ->  ");
        printHex(after[i]);
        Serial.println();
        n++;
    }
    if (!n)
        Serial.println("    (nothing changed)");
    Serial.flush();
    return n;
}

static void dumpAll(const uint32_t *r)
{
    for (unsigned i = 0; i < N_REGS; i += 4u) {
        Serial.print("    +");
        printHex(off_of(i), 3);
        Serial.print("  ");
        for (unsigned j = 0; j < 4u && i + j < N_REGS; j++) {
            printHex(r[i + j]);
            Serial.print(' ');
        }
        Serial.println();
    }
    Serial.flush();
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 SD block map, one CMD0 wide ===");
    Serial.println("No hypothesis. Snapshot 72 registers, send CMD0,");
    Serial.println("snapshot again, print what moved. Run this once with a");
    Serial.println("card in the slot and once without - see the header.");
    Serial.println();
    Serial.flush();

    done = false;
}

void loop()
{
    static uint32_t burst_sta[32], burst_cmd[32];
    unsigned moved;

    if (done)
        return;
    done = true;

    /* --- 1: the block as found ------------------------------------ */

    Serial.println("--- the whole block, as found ---");
    Serial.print("  module 36 ");
    Serial.print(sl6806_module_enabled(SL6806_SD_MODULE_ID) ? "ENABLED"
                                                            : "disabled");
    Serial.print(", ROM clock 17 ");
    Serial.println((sl6806_mmio_read(SL6806_SD_ROMCLK_REG)
                    & SL6806_SD_ROMCLK_BIT) ? "on" : "off");
    snapshot(before);
    dumpAll(before);
    Serial.println();

    /* --- 2: what bring-up moves ----------------------------------- */

    Serial.println("--- what the bring-up changes ---");
    sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT);
    snapshot(after);
    diff("registers that bring-up moved:");
    Serial.println();

    /* --- 3: what ONE CMD0 moves ----------------------------------- */

    /*
     * The measurement this sketch exists for. Snapshot, command, snapshot.
     * The status register is acknowledged first so that anything set
     * afterwards was set by this command and not left over.
     */
    Serial.println("--- what one CMD0 changes ---");
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA, SL6806_SD_STA_CLEAR_ALL);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);

    snapshot(before);
    sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);

    /*
     * Thirty-two back-to-back samples with nothing in between, captured
     * before anything is printed. A status bit that sets and self-clears in
     * a handful of cycles would be invisible to a poll loop that formats
     * output as it goes, and "the command completed too fast to see" is a
     * different world from "the command did nothing".
     */
    for (unsigned i = 0; i < 32u; i++) {
        burst_sta[i] = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA);
        burst_cmd[i] = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CMD);
    }

    for (unsigned n = 0; n < 20000u; n++)
        if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
            & SL6806_SD_STA_CMDDONE)
            break;

    snapshot(after);
    moved = diff("registers that the command moved:");

    Serial.println("  the first 32 samples after the store, STA and CMD:");
    for (unsigned i = 0; i < 32u; i += 4u) {
        Serial.print("    ");
        for (unsigned j = 0; j < 4u; j++) {
            printHex(burst_sta[i + j]);
            Serial.print('/');
            printHex(burst_cmd[i + j] >> 28, 1);
            Serial.print(' ');
        }
        Serial.println();
    }
    Serial.println("    (each pair is STA and the top nibble of CMD, so an 8");
    Serial.println("     means the busy bit was still set at that sample)");
    Serial.println();
    Serial.flush();

    /* --- 4: the CLKCR bits nothing has ever written ---------------- */

    /*
     * The vendor's clock setup is a read-modify-write that clears 0x7FFF0FFF
     * and ors its value in, so bits 12..15 and 31 keep whatever they had and
     * no code in the dump ever sets them. Five bits nobody has tried.
     */
    Serial.println("--- CLKCR bits 12..15 and 31, which nothing ever sets ---");
    {
        static const unsigned BITS[5] = { 12, 13, 14, 15, 31 };

        for (unsigned k = 0; k < 5u; k++) {
            uint32_t clkcr = SL6806_SD_CLKCR_IDENT | (1u << BITS[k]);
            uint32_t sta, readback;
            unsigned n;

            sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT);
            sl6806_mmio_set(SL6806_SD_BASE + SL6806_SD_CLKCR, 1u << BITS[k]);
            readback = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CLKCR);

            sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA,
                              SL6806_SD_STA_CLEAR_ALL);
            sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
            sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);
            for (n = 0; n < 20000u; n++)
                if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
                    & SL6806_SD_STA_CMDDONE)
                    break;
            sta = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA);

            Serial.print("  bit ");
            Serial.print(BITS[k]);
            Serial.print("\t CLKCR ");
            printHex(readback);
            Serial.print((readback & (1u << BITS[k])) ? " held " : " DROPPED");
            Serial.print("  STA ");
            printHex(sta);
            Serial.println((sta & SL6806_SD_STA_CMDDONE)
                           ? "   *** CMD0 COMPLETED ***" : "");
            Serial.flush();
            (void)clkcr;
        }
    }

    Serial.println();
    Serial.println("=== summary ===");
    Serial.print("  registers moved by one CMD0: ");
    Serial.println(moved);
    if (moved <= 1u) {
        Serial.println("  One or none means the block takes the command and");
        Serial.println("  executes nothing. That is outside this block now:");
        Serial.println("  the slot's supply rail, or a card-detect input the");
        Serial.println("  controller respects. Run this again with a card in");
        Serial.println("  the slot before concluding anything.");
    } else {
        Serial.println("  More than one register moved, so the block DOES");
        Serial.println("  execute. Whatever moved is the real status, and");
        Serial.println("  docs/SD.md's register map needs it.");
    }
    Serial.println("done.");
}
