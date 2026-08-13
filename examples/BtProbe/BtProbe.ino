/*
 * BtProbe - read the SL6806's candidate Bluetooth window, and then try the
 * gate the vendor uses to open it.
 *
 * WHAT CHANGED SINCE THE FIRST VERSION OF THIS SKETCH
 *
 * It used to be read-only and expect flat zero, because the two things
 * standing between a payload and this window - a PLL and a bit in 0x400E0000
 * - were both unknown. They are not any more. cores/sl6806/sl6806_bt.h
 * decodes the vendor's shared bring-up at 0x00D9A7FC in full, and the
 * registration at 0x00D98C9C names this block's own two ids:
 *
 *     0x400E0000 bit 0   its functional clock
 *     0x400E0008 bit 0   its reset
 *
 * So this sketch now has two halves. The first is the old read-only pass over
 * a much longer register list - thirty registers rather than seventeen, since
 * scanning the neighbourhood for every 0x400E2xxx literal turned up three
 * clusters where the first pass had found one. The second calls
 * sl6806_bt_begin() and reads them all again.
 *
 * ---------------------------------------------------------------------
 *  THE SECOND HALF WRITES, AND ONE OF THE WRITES IS A PLL
 * ---------------------------------------------------------------------
 * It starts the PLL at 0x40080008 with the vendor's own configuration word,
 * sets 0x4008011C to 0x31, enables module clock 46 and takes two bits in
 * 0x400E0000. The argument that this is safe rather than reckless is in the
 * header: 0x4008011C differs by one bit between the vendor's on and off
 * values, which is an enable and not a clock-source reparent, and the PLL is
 * started rather than switched onto anything. Nothing here touches a divider
 * the core or USB is running on.
 *
 * That said, this is the most invasive sketch in the tree. In payload mode it
 * cannot brick anything - flash is never written - but it can plausibly hang
 * the device, and a hang takes the console with it. Unplug, hold the boot
 * button, plug back in.
 *
 * If you want the old behaviour, build with -DBTPROBE_READ_ONLY=1 and the
 * second half is skipped.
 *
 * Each address is printed BEFORE it is read, and the sketch waits for the
 * console ring to drain before touching it, so the last line of a transcript
 * after a fault names the address that caused it.
 *
 *     make SKETCH=examples/BtProbe RUN_MODE=poll run
 *
 * ---------------------------------------------------------------------
 *  MEASURED 2026-08-13, P20 Player - THE GATE OPENS
 * ---------------------------------------------------------------------
 * The sequence ran, the device stayed on the bus, and:
 *
 *     PLL   (0x40080008) = 0xD0010C04    written 0xC0000C04, and it came back
 *                                        with bit 28 (LOCK) and bit 16 set
 *     gate  (0x400E0000) = 0x00000021    bits 5 and 0, both HELD
 *     reset (0x400E0008) = 0x00000001
 *
 * **The PLL locks from a payload.** docs/sl6806_re_notes.md 14a read it
 * stopped at 0x00000801 in bootloader mode and left "what unlocks it" open
 * for two sections; 0x4008011C = 0x31 was left untried because it might have
 * reparented the core or USB clock. It does not - the console survived the
 * whole sequence.
 *
 * Four registers in the window came up from zero:
 *
 *     +0x078 = 0x0C019A14      +0x1B4 = 0x00003301
 *     +0x07C = 0x06060502      +0x21C = 0x00400000
 *
 * and they stay up across a re-upload, which is how a second run with
 * -DBTPROBE_READ_ONLY=1 saw them in its cold pass.
 *
 * **The four counters stayed at zero.** So the window is powered and partly
 * readable, and the link controller is not running - which is what you would
 * expect with no configuration written. That is the next thing to attack, and
 * it needs the config struct decoded, not another probe.
 *
 * WHAT COUNTS AS A RESULT
 *
 *   1. **Flat zero in both passes.** The gate is not this, or not only this.
 *      Still worth reporting: it would mean the vendor's own sequence,
 *      performed faithfully, does not open the group - which is a much
 *      sharper negative than the one docs/BLUETOOTH.md currently records.
 *   2. **Zero cold, anything at all after the unlock.** The wall §14a and §15
 *      document is down, and it is down for the camera and the PWM too,
 *      because it is the same wall and the same module clock.
 *   3. **The counters at +0x228..+0x234 moving between the two samples at
 *      the end.** That is a running link controller, and it is the single
 *      most valuable line this sketch can print: it would turn "a plausible
 *      register window" into "confirmed hardware" in one reading.
 */

#include <Arduino.h>
#include "sl6806_console.h"
#include "sl6806_bt.h"
#include "sl6806_module.h"

typedef struct {
    uint32_t    offset;
    const char *note;
} reg_t;

/* Every offset cores/sl6806/sl6806_bt.h names, in address order. */
static const reg_t regs[] = {
    { 0x000, "base - unexamined" },
    { 0x010, "cluster 1 config, 12-bit field" },
    { 0x014, "cluster 1 config, 12-bit field" },
    { 0x020, "cluster 1 config, 8-bit field" },
    { 0x044, "cluster 1 config, 8-bit field" },
    { 0x048, "cluster 1 config, 7-bit field (constant 30 at the call site)" },
    { 0x04C, "cluster 1 config, 7-bit field" },
    { 0x050, "cluster 1 config, 16-bit field" },
    { 0x054, "cluster 1 config, 12-bit field" },
    { 0x058, "cluster 1 config, 12-bit field" },
    { 0x070, "cluster 1 config, 18-bit field" },
    { 0x078, "cluster 1 config, packed 8/10/14-bit fields" },
    { 0x07C, "cluster 1 config, four packed bytes" },
    { 0x180, "cluster 2" },
    { 0x184, "cluster 2" },
    { 0x188, "cluster 2 - the most-written register in the window" },
    { 0x18C, "cluster 2" },
    { 0x190, "cluster 2" },
    { 0x194, "cluster 2" },
    { 0x198, "cluster 2" },
    { 0x19C, "cluster 2" },
    { 0x1A4, "cluster 2" },
    { 0x1B0, "cluster 2" },
    { 0x1B4, "cluster 2" },
    { 0x1B8, "cluster 2" },
    { 0x200, "CTRL - written 0xFFFFFFFF during reset, bit 0 read as a flag" },
    { 0x214, "shared control - bits 24 and 25 set by two clusters" },
    { 0x218, "STATUS - top nibble compared against 4" },
    { 0x21C, "unexamined" },
    { 0x220, "unexamined" },
    { 0x228, "CLK0 / in-block reset bit 31 - read as 25 bits" },
    { 0x22C, "CLK1 - 30 bits, plus request bit 31 and grant bit 30" },
    { 0x230, "CLK2 - 30 bits" },
    { 0x234, "CLK3 - 30 bits" },
    { 0x280, "unexamined" },
    { 0x300, "cluster 3" },
    { 0x304, "cluster 3" },
    { 0x30C, "cluster 3" },
    { 0x310, "cluster 3" },
    { 0x318, "cluster 3" },
    { 0x320, "cluster 3" },
    { 0x32C, "cluster 3" },
};
#define NREGS ((int)(sizeof(regs) / sizeof(regs[0])))

static int  idx;
static int  pass;
static bool announced;
static bool done;
static uint32_t cold[NREGS];

/* Same rationale as examples/RegFileProbe: printing an address is not the
 * same as delivering it, and blocking to force delivery would stop the USB
 * service that drains the ring in the first place. So the wait is a state:
 * announce, return, and only read once the ring has actually emptied. */
static bool roomToPrint(void)
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

static void printHex(uint32_t v)
{
    Serial.print("0x");
    Serial.print(v, HEX);
}

#ifndef BTPROBE_READ_ONLY
static void unlock(void)
{
    uint32_t c[4];

    Serial.println();
    Serial.println("--- the gate ---");
    Serial.print("module 46 already up? ");
    Serial.println(sl6806_module_enabled(SL6806_BT_GROUP_MODULE_ID) ? "yes"
                                                                    : "no");

    Serial.println("sl6806_bt_begin(): power request, PLL, module 46,");
    Serial.println("0x4008011C = 0x31, 0x400E0000 bits 5 and 0, reset.");
    Serial.flush();

    Serial.print("  -> ");
    Serial.println(sl6806_bt_begin() ? "0x400E0000 held bit 0"
                                     : "0x400E0000 DID NOT hold bit 0");

    Serial.print("  PLL   (0x40080008) = ");
    printHex(sl6806_mmio_read(0x40080008u));
    Serial.println();
    Serial.print("  gate  (0x400E0000) = ");
    printHex(sl6806_mmio_read(SL6806_PERIPH_ENABLE_REG));
    Serial.println();
    Serial.print("  reset (0x400E0008) = ");
    printHex(sl6806_mmio_read(SL6806_PERIPH_RESET_REG));
    Serial.println();

    sl6806_bt_clocks(c);            /* seed the comparison */
    Serial.println();
}
#endif /* BTPROBE_READ_ONLY */

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 Bluetooth window probe ===");
    Serial.println("Base 0x400E2000 - see docs/BLUETOOTH.md.");
    Serial.println("Pass 1 reads cold. Pass 2 reads after the vendor's gate");
    Serial.println("sequence, which starts a PLL - read this sketch's header.");
    Serial.println();
    Serial.println("--- pass 1: cold ---");
    Serial.flush();

    idx = 0;
    pass = 1;
    announced = false;
    done = false;
}

void loop()
{
    uint32_t addr, v;

    if (done)
        return;

    if (!announced) {
        if (!roomToPrint())
            return;
        addr = SL6806_BT_ADDR(regs[idx].offset);
        Serial.print("  +0x");
        Serial.print(regs[idx].offset, HEX);
        Serial.print("  (");
        printHex(addr);
        Serial.print(") ..");
        announced = true;
        return;                  /* let the host collect it */
    }

    if (sl6806_console_space() < (int)SL6806_CONSOLE_SIZE)
        return;

    announced = false;

    addr = SL6806_BT_ADDR(regs[idx].offset);
    v = sl6806_mmio_read(addr);

    Serial.print(" = ");
    printHex(v);
    if (pass == 1) {
        cold[idx] = v;
        Serial.print("  ");
        Serial.println(regs[idx].note);
    } else if (v != cold[idx]) {
        Serial.print("  CHANGED from ");
        printHex(cold[idx]);
        Serial.print("  ");
        Serial.println(regs[idx].note);
    } else {
        Serial.println("  (unchanged)");
    }

    idx++;
    if (idx < NREGS)
        return;

#ifndef BTPROBE_READ_ONLY
    if (pass == 1) {
        unlock();
        Serial.println("--- pass 2: after the gate ---");
        idx = 0;
        pass = 2;
        return;
    }
#endif

    Serial.println();
    Serial.println("sampling the four counters eight times. Any of them");
    Serial.println("moving on its own is the strongest hardware signal this");
    Serial.println("probe can give.");
    Serial.println();

    for (int k = 0; k < 8; k++) {
        uint32_t c[4];
        int moved = sl6806_bt_clocks(c);

        Serial.print("  ");
        for (int i = 0; i < 4; i++) {
            printHex(c[i]);
            Serial.print(i == 3 ? "" : " ");
        }
        Serial.println(moved ? "   <-- MOVED" : "");
        delay(50);
    }

    Serial.println();
    Serial.println("done.");
    done = true;
}
