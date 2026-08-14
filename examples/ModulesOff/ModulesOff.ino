/*
 * ModulesOff - does a payload survive ROM 0x3BFC, and what was on before it?
 *
 * ===================================================================
 *  THE QUESTION
 * ===================================================================
 * ROM 0x3BFC switches off all ninety-six module clocks and delays. It is the
 * first thing the bootloader's hardware_init does (§24), and adding it to
 * examples/FirmBoot stopped the device booting at all - while the clock tree,
 * tested separately, boots fine with the same unstable USB as before. So the
 * module-clock step is the one that breaks it, and there are two quite
 * different reasons it might:
 *
 *   1. **The payload dies during the call.** It runs from SRAM and calls into
 *      the mask ROM; if either needs a module clock that goes off, nothing
 *      after that instruction ever executes.
 *   2. **The payload survives and the application starves.** Only the first
 *      0x5862 bytes of the application are copied to SRAM - the bulk of it
 *      runs XIP from flash at 0x00C10000 - so an application entered with the
 *      flash controller's clock off faults the moment it branches into XIP.
 *      The bootloader survives the same call because everything it needs
 *      afterwards it enables again by name, flash included.
 *
 * Those need opposite fixes and this tells them apart, which four rounds of
 * reasoning about FirmBoot did not.
 *
 *     make SKETCH=examples/ModulesOff RUN_MODE=poll run
 *
 * ===================================================================
 *  HOW IT ASKS
 * ===================================================================
 * Snapshot the six gate and shadow registers, print them, call ROM 0x3BFC,
 * put the snapshot straight back, and print again.
 *
 * **If the second block prints, the payload survives** and the answer is (2):
 * FirmBoot needs to re-enable flash - and better, the snapshot printed here
 * is exactly the set of module clocks the boot ROM leaves on, which nothing
 * has recorded and which names the candidates.
 *
 * **If the console never comes back, the answer is (1)** and the payload
 * cannot make that call at all. Unplug; nothing is written to flash.
 *
 * The restore is the whole safety of this: the same six words go back
 * immediately, so whatever the ROM had running is running again a few
 * microseconds later.
 */

#include <Arduino.h>

extern "C" {
#include "sl6806_hwinit.h"
#include "sl6806_mmio.h"
}

#define CRU 0x40080000u

/* [V] §15b: three banks of 32 ids, gate and shadow. ROM 0x3BFC zeroes all
 * six of these. */
static const uint32_t REGS[6] = { 0x60u, 0x64u, 0x68u, 0x70u, 0x74u, 0x78u };
static const char *NAMES[6] = {
    "gate   0-31 ", "gate  32-63 ", "gate  64-95 ",
    "shadow 0-31 ", "shadow 32-63", "shadow 64-95"
};

static uint32_t saved[6];
static bool done;

static void printHex(uint32_t v)
{
    static const char *hex = "0123456789ABCDEF";

    Serial.print("0x");
    for (int i = 7; i >= 0; i--)
        Serial.print(hex[(v >> (i * 4)) & 0xF]);
}

static void dump(const char *what)
{
    Serial.print("  ");
    Serial.println(what);
    for (unsigned i = 0; i < 6u; i++) {
        uint32_t v = sl6806_mmio_read(CRU + REGS[i]);

        Serial.print("    +0x");
        Serial.print(REGS[i], HEX);
        Serial.print("  ");
        Serial.print(NAMES[i]);
        Serial.print("  ");
        printHex(v);
        if (v) {
            Serial.print("   ids:");
            for (unsigned b = 0; b < 32u; b++)
                if (v & (1u << b)) {
                    Serial.print(' ');
                    Serial.print((i % 3u) * 32u + b);
                }
        }
        Serial.println();
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806: surviving ROM 0x3BFC ===");
    Serial.println("All ninety-six module clocks off, then straight back on.");
    Serial.println("If the second block never appears, the payload did not");
    Serial.println("survive the call - unplug, nothing was written.");
    Serial.println();
    Serial.flush();
    done = false;
}

void loop()
{
    unsigned i;

    if (done)
        return;
    done = true;

    Serial.println("--- as the boot ROM leaves it ---");
    dump("module clocks on before anything:");
    for (i = 0; i < 6u; i++)
        saved[i] = sl6806_mmio_read(CRU + REGS[i]);
    Serial.println();
    Serial.println("  Those ids are what the boot ROM keeps running in");
    Serial.println("  bootloader mode. Nothing has recorded them before.");
    Serial.println();
    Serial.println("  Calling ROM 0x3BFC now, then restoring immediately.");
    Serial.flush();

    /* The call, and the restore. Nothing prints in between: the console's own
     * clock is among the ninety-six. */
    sl6806_hwinit_modules_off();
    for (i = 0; i < 6u; i++)
        sl6806_mmio_write(CRU + REGS[i], saved[i]);

    Serial.println();
    Serial.println("--- and back ---");
    Serial.println("  *** THE PAYLOAD SURVIVED ROM 0x3BFC ***");
    dump("module clocks after the restore:");
    Serial.println();
    Serial.println("  So FirmBoot's failure is not the call itself. The");
    Serial.println("  application runs XIP from flash and only its first");
    Serial.println("  0x5862 bytes are copied to SRAM, so it starves the");
    Serial.println("  moment it branches into flash with that clock off.");
    Serial.println("  The fix is to re-enable flash after the call - and the");
    Serial.println("  ids printed above are where to look for which one.");
    Serial.println("done.");
}
