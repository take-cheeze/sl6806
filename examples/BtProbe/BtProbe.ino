/*
 * BtProbe - read the SL6806's candidate Bluetooth register block on
 * hardware.
 *
 * WHAT THIS IS LOOKING FOR
 *
 * docs/BLUETOOTH.md and cores/sl6806/sl6806_bt.h work out, from the flash
 * dump alone, that 0x400E2000 is very likely a real peripheral: it is the
 * one literal address anywhere in the 1.8 MB application that is loaded by
 * the code sitting next to the HCI command dispatcher, and that code does a
 * reset-pulse / config-fanout sequence shaped like every other peripheral
 * bring-up in this codebase.
 *
 * EXPECT FLAT ZERO. docs/sl6806_re_notes.md §14a - reached independently,
 * chasing the backlight - already read this exact address from the host in
 * bootloader mode and got all zeros, the same as 0x400E0000 and 0x40084000:
 * the whole block sits behind a PLL and a 0x400E0000 enable gate that
 * nothing in this codebase has unlocked yet without risking the core or USB
 * clock. This sketch exists to confirm a payload sees the same thing the
 * host command does (nobody has checked that the two paths agree here), and
 * to be ready to re-run the moment that gate opens for any reason - see
 * "Next actions" item 11 in the notes.
 *
 * ---------------------------------------------------------------------
 *  READ THIS FIRST - the same warning examples/MmioProbe carries
 * ---------------------------------------------------------------------
 * Reading an unmapped address on a Cortex-M raises a BusFault, and in
 * payload mode the boot ROM owns the fault handler, so the likely outcome
 * is that the device stops responding. That is not damage: unplug, hold the
 * boot button, plug back in. Flash is never touched here.
 *
 * THIS SKETCH ONLY READS. Not one write - unlike LCD or GPIO, there is no
 * confirmed-safe register in this block yet, and 0x400E2000 sits right next
 * to the clock and reset unit's neighbourhood in the address map, so a
 * blind write here is exactly the kind of thing that can gate off a rail.
 *
 * Each address is printed BEFORE it is read, and the sketch waits for the
 * console ring to actually drain before touching the address - see the
 * comment on `announced` below, copied from examples/RegFileProbe, which is
 * what makes the last line of a monitor transcript trustworthy after a
 * fault instead of just the last line that happened to be buffered.
 *
 *     make SKETCH=examples/BtProbe RUN_MODE=poll run
 *
 * WHAT COUNTS AS A RESULT. Flat 0x00000000 across every offset is the
 * expected outcome - it matches the host-side read in §14a, and just means
 * the payload path agrees with the host command. 0xFFFFFFFF would disagree
 * with that read and be worth a second look. If SL6806_BT_STATUS (+0x218)
 * changes between the two watch passes at the end, that would mean the PLL/
 * 0x400E0000 gate is no longer where §14a found it - unexpected, and worth
 * following up either way.
 */

#include <Arduino.h>
#include "sl6806_console.h"
#include "sl6806_bt.h"

typedef struct {
    uint32_t    offset;
    const char *note;
} reg_t;

/* Every offset cores/sl6806/sl6806_bt.h currently names. */
static const reg_t regs[] = {
    { 0x000, "base + 0x000 - unexamined so far" },
    { 0x010, "config fanout target, 12-bit field" },
    { 0x014, "config fanout target, 12-bit field" },
    { 0x020, "config fanout target, 8-bit field" },
    { 0x044, "config fanout target, 8-bit field" },
    { 0x048, "config fanout target, 7-bit field" },
    { 0x04C, "config fanout target, 7-bit field" },
    { 0x050, "config fanout target, 16-bit field" },
    { 0x054, "config fanout target, 12-bit field" },
    { 0x058, "config fanout target, 12-bit field" },
    { 0x070, "config fanout target, 18-bit field" },
    { 0x078, "config fanout target, packed 8/10/14-bit fields" },
    { 0x07C, "config fanout target, four packed byte fields" },
    { 0x200, "SL6806_BT_CTRL - written 0xFFFFFFFF during reset, bit1 read elsewhere" },
    { 0x214, "SL6806_BT_LOAD - bit24 set before the config fanout" },
    { 0x218, "SL6806_BT_STATUS - top nibble read and compared against 4" },
    { 0x228, "SL6806_BT_RESET - bit31 cleared/delayed/set as a reset pulse" },
};
#define NREGS ((int)(sizeof(regs) / sizeof(regs[0])))

static int  idx;
static bool announced;
static bool done;

/* Same rationale as examples/RegFileProbe: printing an address is not the
 * same as delivering it, and blocking to force delivery would stop the USB
 * service that drains the ring in the first place. So the wait is a state:
 * announce, return, and only read once the ring has actually emptied. */
static bool roomToPrint(void)
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 Bluetooth register block probe ===");
    Serial.println("Read-only. Base 0x400E2000 - see docs/BLUETOOTH.md.");
    Serial.println("Each address is printed before it is read, so if this");
    Serial.println("stops, the last address shown is the one that faulted.");
    Serial.flush();

    idx = 0;
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
        addr = SL6806_BT_BASE + regs[idx].offset;
        Serial.print("  +0x");
        Serial.print(regs[idx].offset, HEX);
        Serial.print("  (0x");
        Serial.print(addr, HEX);
        Serial.print(") ..");
        announced = true;
        return;                  /* let the host collect it */
    }

    if (sl6806_console_space() < (int)SL6806_CONSOLE_SIZE)
        return;

    announced = false;

    addr = SL6806_BT_BASE + regs[idx].offset;
    v = *(volatile uint32_t *)addr;
    Serial.print(" = 0x");
    Serial.print(v, HEX);
    Serial.print("  ");
    Serial.println(regs[idx].note);

    idx++;
    if (idx >= NREGS) {
        Serial.println();
        Serial.println("first pass complete. Watching STATUS (+0x218) and");
        Serial.println("CTRL (+0x200) for 8 samples - a value that moves on");
        Serial.println("its own, unprompted, is the strongest hardware");
        Serial.println("signal this probe can give.");
        Serial.println();

        for (int k = 0; k < 8; k++) {
            uint32_t status = *(volatile uint32_t *)(SL6806_BT_BASE + 0x218);
            uint32_t ctrl   = *(volatile uint32_t *)(SL6806_BT_BASE + 0x200);
            Serial.print("  status=0x");
            Serial.print(status, HEX);
            Serial.print("  ctrl=0x");
            Serial.println(ctrl, HEX);
            delay(50);
        }

        Serial.println();
        Serial.println("done. Nothing was written.");
        done = true;
    }
}
