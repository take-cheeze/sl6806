/*
 * AudioProbe - is the audio controller at 0x40009000 there, and does it wake?
 *
 * WHAT THIS IS FOR
 *
 * cores/sl6806/sl6806_audio.h works out from the flash dump that 0x40009000
 * is the SoC's audio controller: two DMA directions, three playback volume
 * channels, four capture ones, a 128-word coefficient RAM, and a sample-rate
 * clock that picks between 24.576 MHz and 22.579 MHz depending on whether the
 * rate divides by 8000. As of 2026-08-13 those registers have been read, and
 * they are there - see the measured block below.
 *
 * The reason to run this before examples/ToneDemo is that it answers the one
 * question that decides whether any of the rest is worth doing, and it
 * answers it without writing a single control register:
 *
 *   Pass 1 reads the block cold. Every peripheral on this chip that is gated
 *   off reads flat zero and drops writes, so a cold read that is *not* flat
 *   zero already says the block is at least partly awake.
 *
 *   [M] **"Cold" means since the last unplug, not since the last upload.**
 *   Module 37 and romclk 19 survive a re-upload, so the second run of this
 *   sketch finds pass 1 already awake. That is not a new result; it is the
 *   previous run's. Power-cycle the device to get a genuine cold pass.
 *
 *   Pass 2 enables module clock 37 and romclk 19 - the two clocks the
 *   vendor's own bring-up takes, both of which this framework can reach
 *   without going near the 0x400E0000 wall that has the camera and the
 *   backlight stuck - and reads again. A block that changes between the two
 *   passes is the result this whole file is for.
 *
 *   Pass 3 writes to four registers, one at a time, and prints what each one
 *   gives back - which is the only way to tell "gated" from "idle": both read
 *   zero, and only one of them keeps what you give it.
 *
 * ---------------------------------------------------------------------
 *  MEASURED 2026-08-13, P20 Player - THE BLOCK IS THERE
 * ---------------------------------------------------------------------
 * Cold: every register 0x00000000. After module 37 + romclk 19: thirty of
 * them at reset defaults at once. The full transcript is in
 * cores/sl6806/sl6806_audio.h; the two lines that matter most are
 *
 *     +0x138 = 0x0000A1FF   playback level 0x1FF - maximum
 *     +0x228 = 0x0000A000   capture  level 0     - minimum
 *
 * A DAC that resets open and an ADC that resets shut, which is both the
 * confirmation that this is audio and the confirmation that TX and RX are
 * the right way round.
 *
 * **Pass 3's first version reported "drops writes" and was wrong.** It wrote
 * 0x5A5A5A50 to the buffer-address register - not a valid SRAM address on
 * this chip - and concluded a block that had visibly just woken up was gated.
 * Rewritten to use plausible values, it says:
 *
 *     +0x10C  wrote 0x830000   read 0x830000    KEPT
 *     +0x20C  wrote 0x840000   read 0x840000    KEPT
 *     +0x138  wrote 0x1AA      read 0x800001AA  bit 31 is set by hardware
 *     +0x104  wrote 0x100001   read 0x100000    bit 0 does not stick
 *
 * So both DMA address registers hold a descriptor, the 9-bit level field is
 * where this file says it is with a hardware status bit above it, and the
 * trigger's arm bit is write-only or self-clearing. examples/ToneDemo then
 * ran a whole buffer through.
 *
 * ---------------------------------------------------------------------
 *  WHAT IT DOES NOT DO
 * ---------------------------------------------------------------------
 * It does not start the DMA, touch the DAC enables, or program the audio
 * PLL. Every write in pass 3 is put back.
 *
 * Reading an unmapped address on a Cortex-M raises a BusFault, and in payload
 * mode the boot ROM owns the fault handler, so if this block is not there the
 * likely outcome is that the device stops responding. That is not damage:
 * unplug, hold the boot button, plug back in. Flash is never touched.
 *
 * Each address is printed BEFORE it is read, and the sketch waits for the
 * console ring to drain before touching it, so the last line of a monitor
 * transcript after a fault names the address that caused it rather than
 * whatever happened to be buffered. Same pacing as examples/RegFileProbe.
 *
 *     make SKETCH=examples/AudioProbe RUN_MODE=poll run
 *
 * WHAT COUNTS AS A RESULT, in increasing order of how much it unlocks:
 *
 *   1. Flat zero in all three passes. Would mean this unit differs from the
 *      one measured above - report it.
 *   2. Flat zero cold, reset defaults after the clocks. Confirmed on one
 *      unit; a second unit agreeing is worth one line.
 *   3. Which of pass 3's four registers keep what they are given. This is
 *      the open question now, and it decides whether examples/ToneDemo can
 *      program a transfer at all.
 *   4. Anything in the register list that CHANGES between two reads with
 *      nothing written. That is a running counter or a live status bit, and
 *      it is the difference between "the registers answer" and "the hardware
 *      runs" that cost the PWM twelve hardware sessions.
 */

#include <Arduino.h>
#include "sl6806_console.h"
#include "sl6806_audio.h"
#include "sl6806_module.h"
#include "sl6806_romclk.h"

typedef struct {
    uint32_t    offset;
    const char *note;
} reg_t;

/* Every register cores/sl6806/sl6806_audio.h names, plus the neighbours the
 * codec dispatcher writes that have no name yet. */
static const reg_t regs[] = {
    { 0x000, "CTRL - bits 24/25 are the two DAC enables" },
    { 0x008, "DAC path - ch0 in [15:0], ch1 in [31:16]" },
    { 0x018, "[9:5] written by the dispatcher, role unknown" },
    { 0x02C, "ADC gain, [20:16]" },
    { 0x030, "mic gain, [20:16] and [28:24]" },
    { 0x03C, "bit 10 armed by the block init" },
    { 0x07C, "[26:24], role unknown" },
    { 0x080, "[14:12] and [11:9], role unknown" },
    { 0x100, "TX control - bit 0 set by init, [15:12] cleared" },
    { 0x104, "TX trigger - watermark in [31:16], bit 0 arms" },
    { 0x108, "TX ctrl/status - len [31:16], start bit 4, flags 8 and 10" },
    { 0x10C, "TX buffer address" },
    { 0x134, "TX ch0 mute (bit 16)" },
    { 0x138, "TX ch0 level [8:0]" },
    { 0x154, "TX ch1 mute" },
    { 0x158, "TX ch1 level" },
    { 0x174, "TX ch2 mute" },
    { 0x178, "TX ch2 level" },
    { 0x200, "RX control" },
    { 0x204, "RX trigger" },
    { 0x208, "RX ctrl/status" },
    { 0x20C, "RX buffer address" },
    { 0x224, "RX ch0 mute" },
    { 0x228, "RX ch0 level" },
    { 0x254, "RX ch1 mute" },
    { 0x258, "RX ch1 level" },
    { 0x284, "RX ch2 mute" },
    { 0x288, "RX ch2 level" },
    { 0x2B4, "RX ch3 mute" },
    { 0x2B8, "RX ch3 level" },
    { 0x400, "coefficient RAM word 0" },
    { 0x5FC, "coefficient RAM word 127" },
};
#define NREGS ((int)(sizeof(regs) / sizeof(regs[0])))

static int  idx;
static int  pass;
static bool announced;
static bool done;
static uint32_t cold[NREGS];

/* Printing an address is not the same as delivering it, and blocking to force
 * delivery would stop the USB service that drains the ring. So the wait is a
 * state: announce, return, read only once the ring has emptied. */
static bool ringEmpty(void)
{
    return sl6806_console_space() >= (int)SL6806_CONSOLE_SIZE;
}

static bool roomToPrint(void)
{
    return sl6806_console_space() >= (int)(SL6806_CONSOLE_SIZE / 2);
}

static void printHex(uint32_t v)
{
    Serial.print("0x");
    Serial.print(v, HEX);
}

/*
 * Write one value, read it back, put the old one back, and print all three.
 * A register that returns something *related* to what it was given - masked,
 * truncated, aligned - is a live register with a narrower field than
 * expected, which is a completely different result from one that returns
 * zero, and a yes/no test cannot tell them apart.
 */
static void writeTest(uint32_t off, uint32_t value, const char *note)
{
    uint32_t addr = SL6806_AUD_BASE + off;
    uint32_t saved = sl6806_mmio_read(addr);
    uint32_t back;

    sl6806_mmio_write(addr, value);
    back = sl6806_mmio_read(addr);
    sl6806_mmio_write(addr, saved);

    Serial.print("  +0x");
    Serial.print(off, HEX);
    Serial.print("  was ");
    printHex(saved);
    Serial.print("  wrote ");
    printHex(value);
    Serial.print("  read ");
    printHex(back);
    Serial.print(back == value ? "  KEPT   " : "  differs ");
    Serial.println(note);
}

/* The clocks, between pass 1 and pass 2. Transcribed rather than called for
 * the reason sl6806_romclk.h gives - the vendor's veneer is not resident. */
static void enableClocks(void)
{
    unsigned i;

    Serial.println();
    Serial.print("module_enable(37) -> ");
    Serial.println(sl6806_module_enable(SL6806_AUD_MODULE_ID) ? "acked"
                                                              : "NO ACK");

    for (i = 0; i < SL6806_ROMCLK_COUNT; i++) {
        if (sl6806_romclk[i].id != SL6806_AUD_ROMCLK_ID)
            continue;
        Serial.print("romclk 19: ");
        printHex(sl6806_romclk[i].addr);
        Serial.print(" |= ");
        printHex(sl6806_romclk[i].bit);
        Serial.println();
        *(volatile uint32_t *)sl6806_romclk[i].addr |= sl6806_romclk[i].bit;
    }
    Serial.println();
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("=== SL6806 audio controller probe ===");
    Serial.println("Base 0x40009000 - see docs/AUDIO.md.");
    Serial.println("Pass 1 reads cold, pass 2 after module 37 + romclk 19.");
    Serial.println("Each address is printed before it is read, so if this");
    Serial.println("stops, the last address shown is the one that faulted.");
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
        addr = SL6806_AUD_BASE + regs[idx].offset;
        Serial.print("  +0x");
        Serial.print(regs[idx].offset, HEX);
        Serial.print("  (");
        printHex(addr);
        Serial.print(") ..");
        announced = true;
        return;                  /* let the host collect it */
    }

    if (!ringEmpty())
        return;

    announced = false;

    addr = SL6806_AUD_BASE + regs[idx].offset;
    v = *(volatile uint32_t *)addr;

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

    if (pass == 1) {
        enableClocks();
        Serial.println("--- pass 2: after the clocks ---");
        idx = 0;
        pass = 2;
        return;
    }

    /*
     * Pass 3. Four registers, each with a value it could plausibly hold,
     * each put back. Print the read-back rather than a verdict: the first
     * version of this printed "drops writes" because it wrote an address no
     * DMA register on this chip could keep, and a bare yes/no hid that.
     */
    Serial.println();
    Serial.println("--- pass 3: does it keep what it is given? ---");
    writeTest(0x10C, 0x00830000u, "TX address, a real SRAM address");
    writeTest(0x20C, 0x00840000u, "RX address, likewise");
    writeTest(0x138, 0x000001AAu, "TX ch0 level, 9 bits");
    writeTest(0x104, 0x00100001u, "TX trigger, watermark + arm");
    Serial.print("sl6806_audio_writable() says: ");
    Serial.println(sl6806_audio_writable() ? "yes" : "no");

    Serial.println();
    Serial.println("watching +0x108 and +0x208 for 8 samples. A value that");
    Serial.println("moves with nothing written is the strongest signal this");
    Serial.println("probe can give.");
    for (int k = 0; k < 8; k++) {
        Serial.print("  tx=");
        printHex(*(volatile uint32_t *)(SL6806_AUD_BASE + 0x108));
        Serial.print("  rx=");
        printHex(*(volatile uint32_t *)(SL6806_AUD_BASE + 0x208));
        Serial.println();
        delay(50);
    }

    Serial.println();
    Serial.println("done. Two clock enables and four writes, all restored.");
    done = true;
}
