/*
 * SdLife - does anything in this block have a running clock?
 *
 * ===================================================================
 *  WHAT SdScope SETTLED, AND WHAT IT DID NOT
 * ===================================================================
 * 2026-08-13. Seventy-two registers snapshotted, one CMD0 sent, snapshotted
 * again. **Exactly one register moved, and it was the one that was written.**
 * The thirty-two back-to-back samples taken immediately afterwards were all
 * zero - status zero, and the command register's busy bit already gone in the
 * very first sample.
 *
 * Put that against examples/SdCtrl, which read the command register one MMIO
 * access earlier and saw the start bit set. Both are true, and together they
 * say something precise: **the busy bit lasts less than one register access,
 * about 300 ns.** At the identification clock, divider 7, shifting a 48-bit
 * command out takes tens of microseconds. So the block is not executing the
 * command slowly, or waiting on anything. It is discarding it.
 *
 * Also settled that run: CLKCR bits 12..15 and 31, the five that no bring-up
 * in the dump ever writes, do not exist - every one was dropped.
 *
 * What none of it settles is the question underneath: **does the core of this
 * block have a clock at all?** Every register that has ever been read here is
 * on the register interface, which is clocked by module 36 and demonstrably
 * works - writes stick. Nothing anyone has read tells you whether the logic
 * behind those registers is running. A peripheral whose bus interface is
 * clocked and whose core is not looks exactly like this: it takes writes,
 * returns them, and does nothing.
 *
 * ===================================================================
 *  THREE TESTS THAT NEED NO CARD AND NO HYPOTHESIS
 * ===================================================================
 *   1. **Does anything move on its own?** Sample all 72 registers a few
 *      hundred times and report any word that ever differs from itself.
 *      Free-running counters, FIFO levels and state machines all show up
 *      here. If literally nothing in the block ever changes without being
 *      written, its core is stopped - and that is a fact rather than an
 *      inference from a command that failed.
 *
 *   2. **Does the FIFO work?** The data port at +0x200 is a real FIFO with
 *      flags in +0x48: bit 2 says the receive side is empty, bit 3 says the
 *      transmit side is full (ROM 0x464A and 0x4658). Writing words into it
 *      and watching those flags exercises the datapath without a card, a
 *      command, or a clock on the bus to the slot. **If the flags move, the
 *      core is alive and the fault is in the command path. If they do not,
 *      the core is stopped and nothing about SD is worth investigating until
 *      that changes.** This is the sharpest test in the sketch.
 *
 *   3. **The eight bits of CLKCR's [30:23] field.** The vendor uses exactly
 *      two values there - 0x20000000 to identify and 0x10000000 to run,
 *      which are single bits 29 and 28 - and the field is eight bits wide.
 *      The other six have never been set by anything. One of them being a
 *      clock enable would explain everything above.
 *
 * ===================================================================
 *  AND IT PACES ITS OWN OUTPUT
 * ===================================================================
 * Four runs in a row have opened with "[lost output - device outran the poll
 * rate]". The console is a 2 KB ring that overwrites rather than blocking,
 * and in RUN_MODE=poll the host only drains it between loop() calls - so a
 * sketch that prints a screenful without returning loses the beginning of it,
 * every time, and the beginning is where the register dump is.
 *
 * This one is a state machine like examples/BtProbe: one section per loop()
 * call, and it will not start a section until the ring has actually emptied.
 * Slower to watch, and nothing is lost.
 *
 *     make SKETCH=examples/SdLife RUN_MODE=poll run
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_sd.h"
#include "sl6806_mmio.h"
#include "sl6806_module.h"
#include "sl6806_console.h"
}

#define N_LOW   64u
#define N_HIGH  8u
#define N_REGS  (N_LOW + N_HIGH)

static int      phase;
static unsigned cursor;
static bool     done;
static uint32_t seen[N_REGS];
static uint32_t changed[N_REGS];

static void printHex(uint32_t v, int digits = 8)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = digits - 1; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

/* Do not print until the host has collected what was printed last time.
 * examples/BtProbe's, for the same reason. */
static bool drained()
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

static uint32_t off_of(unsigned i)
{
    return (i < N_LOW) ? (i * 4u) : (0x100u + (i - N_LOW) * 4u);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 SD block: is its core clocked? ===");
    Serial.println("Paced output - one section per USB poll, so nothing is");
    Serial.println("lost. See the sketch header.");
    Serial.flush();

    phase = 0;
    cursor = 0;
    done = false;
}

void loop()
{
    if (done || !drained())
        return;

    switch (phase) {

    /* --- 0: the gates ------------------------------------------- */
    case 0:
        Serial.println();
        Serial.println("--- gates ---");
        Serial.print("  module 36 ");
        Serial.print(sl6806_module_enabled(SL6806_SD_MODULE_ID) ? "ENABLED"
                                                                : "disabled");
        Serial.print(", 0x40080080 = ");
        printHex(sl6806_mmio_read(SL6806_SD_ROMCLK_REG));
        Serial.println();
        sl6806_sd_controller_begin(0u, SL6806_SD_CLKCR_IDENT);
        Serial.println("  controller brought up.");
        phase++;
        cursor = 0;
        return;

    /* --- 1: the register dump, four words at a time -------------- */
    case 1:
        if (cursor == 0) {
            Serial.println();
            Serial.println("--- the block, after bring-up ---");
        }
        for (unsigned k = 0; k < 8u && cursor < N_REGS; k++, cursor += 4u) {
            Serial.print("    +");
            printHex(off_of(cursor), 3);
            Serial.print("  ");
            for (unsigned j = 0; j < 4u && cursor + j < N_REGS; j++) {
                printHex(sl6806_mmio_read(SL6806_SD_BASE + off_of(cursor + j)));
                Serial.print(' ');
            }
            Serial.println();
        }
        if (cursor >= N_REGS) {
            phase++;
            cursor = 0;
        }
        return;

    /* --- 2: does anything move on its own? ----------------------- */
    case 2: {
        unsigned n = 0;

        Serial.println();
        Serial.println("--- does anything change without being written? ---");
        for (unsigned i = 0; i < N_REGS; i++) {
            seen[i] = sl6806_mmio_read(SL6806_SD_BASE + off_of(i));
            changed[i] = 0;
        }
        for (unsigned round = 0; round < 200u; round++)
            for (unsigned i = 0; i < N_REGS; i++) {
                uint32_t v = sl6806_mmio_read(SL6806_SD_BASE + off_of(i));

                if (v != seen[i]) {
                    changed[i] |= v ^ seen[i];
                    seen[i] = v;
                }
            }
        for (unsigned i = 0; i < N_REGS; i++) {
            if (!changed[i])
                continue;
            Serial.print("    +");
            printHex(off_of(i), 3);
            Serial.print("  bits that moved: ");
            printHex(changed[i]);
            Serial.println();
            n++;
        }
        if (!n) {
            Serial.println("    NOTHING moved across 200 passes.");
            Serial.println("    No counter, no flag, no state machine. The");
            Serial.println("    core behind these registers is stopped.");
        }
        phase++;
        return;
    }

    /* --- 3: the FIFO, which needs no card and no command --------- */
    case 3: {
        uint32_t sta2_before, sta2_after, sta_after;
        unsigned pushed = 0;

        Serial.println();
        Serial.println("--- the FIFO at +0x200 ---");
        sta2_before = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA2);
        Serial.print("    STA2 before ");
        printHex(sta2_before);
        Serial.print("   rx-empty ");
        Serial.print((sta2_before & SL6806_SD_STA2_RXEMPTY) ? "1" : "0");
        Serial.print("  tx-full ");
        Serial.println((sta2_before & SL6806_SD_STA2_TXFULL) ? "1" : "0");

        /* Push words in and watch the flags. A FIFO whose core is clocked
         * fills; one whose core is stopped swallows every write and reports
         * the same flags forever. */
        for (unsigned i = 0; i < 64u; i++) {
            sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_FIFO,
                              0xF1F00000u + i);
            pushed++;
            if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA2)
                & SL6806_SD_STA2_TXFULL)
                break;
        }
        sta2_after = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA2);
        sta_after = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA);

        Serial.print("    pushed ");
        Serial.print(pushed);
        Serial.print(" words, STA2 now ");
        printHex(sta2_after);
        Serial.print("  STA ");
        printHex(sta_after);
        Serial.println();

        if (sta2_after != sta2_before) {
            Serial.println("    *** THE FLAGS MOVED - the datapath is alive,");
            Serial.println("    and the fault is in the command path. ***");
        } else {
            Serial.println("    The flags did not move. Sixty-four words went");
            Serial.println("    into the data port and the block did not");
            Serial.println("    notice - its core is not running.");
        }

        Serial.print("    reading it back: ");
        for (unsigned i = 0; i < 4u; i++) {
            printHex(sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_FIFO));
            Serial.print(' ');
        }
        Serial.println();
        phase++;
        cursor = 0;
        return;
    }

    /* --- 4: CLKCR's [30:23] field, one bit at a time ------------- */
    case 4: {
        unsigned bit = 23u + cursor;
        uint32_t clkcr, sta;
        unsigned n;

        if (cursor == 0) {
            Serial.println();
            Serial.println("--- CLKCR [30:23], the field the vendor uses two");
            Serial.println("    single bits of (29 to identify, 28 to run) ---");
        }

        sl6806_sd_controller_begin(0u, (1u << bit) | (7u << 16) | 8u);
        clkcr = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_CLKCR);

        sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_STA,
                          SL6806_SD_STA_CLEAR_ALL);
        sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_ARG, 0);
        sl6806_mmio_write(SL6806_SD_BASE + SL6806_SD_CMD, 0x8000A400u);
        for (n = 0; n < 20000u; n++)
            if (sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA)
                & SL6806_SD_STA_CMDDONE)
                break;
        sta = sl6806_mmio_read(SL6806_SD_BASE + SL6806_SD_STA);

        Serial.print("    bit ");
        Serial.print(bit);
        Serial.print("\t CLKCR ");
        printHex(clkcr);
        Serial.print((clkcr & (1u << bit)) ? " held " : " DROPPED");
        Serial.print("  STA ");
        printHex(sta);
        Serial.println((sta & SL6806_SD_STA_CMDDONE)
                       ? "   *** CMD0 COMPLETED ***" : "");

        if (++cursor >= 8u)
            phase++;
        return;
    }

    /* --- 5: what to do with the answer --------------------------- */
    default:
        Serial.println();
        Serial.println("=== what this run means ===");
        Serial.println("  If nothing moved on its own AND the FIFO flags did");
        Serial.println("  not move, the block's core has no clock, and no");
        Serial.println("  amount of SD protocol work matters until it does.");
        Serial.println("  Module 36 and ROM clock 17 are both on, so the");
        Serial.println("  missing clock is a third one - and the place left");
        Serial.println("  to look is the CRU's own divider registers, +0x40");
        Serial.println("  and +0x48, which read 0x09 and 0x51 and which no");
        Serial.println("  sketch has ever written.");
        Serial.println();
        Serial.println("  If the FIFO flags DID move, the core is running,");
        Serial.println("  the datapath works, and the command path is the");
        Serial.println("  only broken part - which would make the slot's");
        Serial.println("  supply and the card-detect input the next things");
        Serial.println("  to rule out, with a card in the slot.");
        Serial.println("done.");
        done = true;
        return;
    }
}
